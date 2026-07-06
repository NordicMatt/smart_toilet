/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include <app_version.h>

#include "actuator.h"
#include "audio_proc.h"
#include "audio_snap.h"
#include "audio_stats.h"
#include "audio_telemetry.h"
#include "audio_watchdog.h"
#include "cloud.h"
#include "control_output.h"
#include "dmic.h"
#include "kws/kws.h"
#include "leds.h"
#include "ww/wakeword.h"

LOG_MODULE_REGISTER(main);

#define DMIC_READ_TIMEOUT 100

/* Give Wi-Fi/Memfault a short head start before audio capture so their bring-up
 * doesn't fight the DMIC + edge-AI inference for CPU/RAM. Bounded: after this we
 * start audio regardless, so a Wi-Fi outage at boot never leaves the toilet
 * deaf. The cloud thread keeps trying to connect independently. */
#define AUDIO_START_NET_WAIT_S 30

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

static int ww_loop(void)
{
	int err;
	void *audio_buffer;
	size_t audio_buffer_size;
	bool ww_detected;

	ww_reset();

	print_control_output((struct control_message){CONTROL_MESSAGE_WAITING_WW});

	while (true) {
		/* Pet the liveness watchdog: reaching here proves the loop iterates. */
		audio_watchdog_feed();

		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			/* A DMIC read error is transient (e.g. -EAGAIN: no block within
			 * the timeout, from a momentary CPU stall during Wi-Fi
			 * reconnection). Skip this block and keep listening — a dropped
			 * ~10 ms block is harmless. Crucially, do NOT return: that would
			 * exit the audio loop and leave the toilet deaf until reboot.
			 */
			LOG_WRN("DMIC read error %d; skipping block", err);
			k_sleep(K_MSEC(5));
			continue;
		}

		audio_proc_run(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_snap_feed(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_stats_update(audio_buffer, DMIC_SAMPLES_IN_BLOCK);

		err = ww_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &ww_detected);
		if (err == -EBUSY) {
			/* More data is needed. */
			continue;
		} else if (err < 0) {
			/* Edge-AI feed/inference error (e.g. -EPERM). Re-init the model
			 * state and keep listening — do NOT return, which would exit the
			 * audio loop and leave the toilet deaf until reboot (same rationale
			 * as the DMIC error above). dmic_read() at the loop top paces
			 * retries.
			 */
			LOG_WRN("Wakeword detection error %d; resetting model and continuing", err);
			ww_reset();
			continue;
		}

		if (ww_detected) {
			audio_telemetry_detection();
			print_control_output((struct control_message){CONTROL_MESSAGE_WW_DETECTED});
			if (IS_ENABLED(CONFIG_APP_MODE_WW_ONLY)) {
				leds_blink_led0();
				actuator_flush();
			} else {
				return 0;
			}
		}
	}
}

static int kws_loop(void)
{
	int err;
	void *audio_buffer;
	size_t audio_buffer_size;
	struct kws_prediction prediction;

	uint32_t spotting_timeout = k_uptime_get_32() + CONFIG_KWS_PERIOD_MS;

	kws_reset();

	print_control_output((struct control_message){.type = CONTROL_MESSAGE_WAITING_KW});

	while (IS_ENABLED(CONFIG_APP_MODE_KWS_ONLY) || spotting_timeout > k_uptime_get_32()) {
		/* Pet the liveness watchdog: reaching here proves the loop iterates. */
		audio_watchdog_feed();

		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			/* Transient (e.g. -EAGAIN). Skip the block and keep going rather
			 * than exiting the audio loop. See ww_loop() for rationale.
			 */
			LOG_WRN("DMIC read error %d; skipping block", err);
			k_sleep(K_MSEC(5));
			continue;
		}

		audio_proc_run(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_snap_feed(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_stats_update(audio_buffer, DMIC_SAMPLES_IN_BLOCK);

		err = kws_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &prediction);
		if (err == -EBUSY) {
			/* More data is needed. */
			continue;
		} else if (err) {
			/* Recover and keep going rather than exiting the audio loop.
			 * See ww_loop() for rationale.
			 */
			LOG_WRN("Keyword spotting error %d; resetting model and continuing", err);
			kws_reset();
			continue;
		}

		if (prediction.valid) {
			leds_blink_led1();
			spotting_timeout = k_uptime_get_32() + CONFIG_KWS_PERIOD_MS;
			print_control_output(
				(struct control_message){.type = CONTROL_MESSAGE_KW_SPOTTED,
							 .kw_class = prediction.class,
							 .name = prediction.name});
		}
	}

	print_control_output((struct control_message){.type = CONTROL_MESSAGE_TIMEOUT_KWS});

	return 0;
}

int main(void)
{
	int err;

	/* Boot marker: prints the running firmware version. Use this to confirm a
	 * FOTA swap actually took effect (the new image reports the new version).
	 */
	LOG_INF("Smart Toilet firmware v%s starting", APP_VERSION_EXTENDED_STRING);

	err = dmic_init();
	if (err) {
		return err;
	}

	err = leds_init();
	if (err) {
		return err;
	}

	err = control_output_init();
	if (err) {
		return err;
	}

	err = audio_snap_init();
	if (err) {
		return err;
	}

	err = actuator_init();
	if (err) {
		return err;
	}

	err = ww_init();
	if (err) {
		return err;
	}

	/* The keyword-spotting stage (and its ~26 KB model) is only needed in the
	 * KWS modes; in WW-only mode it is neither initialised nor linked
	 * (see CMakeLists.txt), saving the RAM the cloud/Memfault stack needs.
	 */
	if (!IS_ENABLED(CONFIG_APP_MODE_WW_ONLY)) {
		err = kws_init();
		if (err) {
			return err;
		}
	}

	LOG_INF("Initialization completed, check output on VCOM0");

	/* Prefer to let Wi-Fi come up first (so its TLS bring-up doesn't compete
	 * with the DMIC + edge-AI inference), but never gate the toilet's core
	 * voice function on the network: wait at most AUDIO_START_NET_WAIT_S, then
	 * start audio regardless. The cloud thread (cloud.c) keeps bringing up
	 * Wi-Fi/Memfault independently via conn_mgr, so they still connect later
	 * even if they are not ready yet here.
	 */
	LOG_INF("Waiting up to %d s for network before starting audio...",
		AUDIO_START_NET_WAIT_S);
	for (int i = 0; i < AUDIO_START_NET_WAIT_S && !cloud_is_connected(); i++) {
		k_sleep(K_SECONDS(1));
	}
	if (cloud_is_connected()) {
		LOG_INF("Network connected; starting audio capture");
	} else {
		LOG_WRN("No network after %d s; starting audio anyway "
			"(voice works offline; Wi-Fi/cloud will connect when available)",
			AUDIO_START_NET_WAIT_S);
	}

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err < 0) {
		LOG_ERR("Failed to start DMIC (err %d)", err);
		return err;
	}

	/* Steady LED1 = audio sampling is live (mic is being read). Signals when
	 * to start speaking; LED0 still blinks on each wake-word detection.
	 */
	leds_on_led1();
	LOG_INF("Audio sampling started (LED1 on)");

	/* Arm the audio-loop liveness watchdog now that capture is running. Boot and
	 * the network wait above are deliberately excluded so they cannot trip it; a
	 * wedge from here on (a silently deaf toilet) captures a coredump + reboots.
	 */
	audio_watchdog_start();

	while (true) {
		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS) ||
		    IS_ENABLED(CONFIG_APP_MODE_WW_ONLY)) {
			err = ww_loop();
			if (err) {
				return err;
			}
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS)) {
			leds_on_led0();
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS) ||
		    IS_ENABLED(CONFIG_APP_MODE_KWS_ONLY)) {
			err = kws_loop();
			if (err) {
				return err;
			}
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS)) {
			leds_off_led0();
		}
	}

	return 0;
}
