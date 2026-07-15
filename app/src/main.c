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

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

/* Consecutive dmic_read() failures before restarting PDM capture in-place
 * (~1 s: each failed read blocks up to DMIC_READ_TIMEOUT=100 ms + 5 ms sleep).
 * A halted driver (ring overrun during Wi-Fi scan/reconnect bursts) never
 * recovers via reads alone; restarting here self-heals in ~100 ms where the
 * audio watchdog would otherwise reboot at 60 s. The watchdog stays as the
 * backstop if even restarts fail. */
#define DMIC_ERRS_BEFORE_RESTART 10

static int ww_loop(void)
{
	int err;
	void *audio_buffer;
	size_t audio_buffer_size;
	bool ww_detected;
	int dmic_errs = 0;

	ww_reset();

	print_control_output((struct control_message){CONTROL_MESSAGE_WAITING_WW});

	while (true) {
		/* NOTE: the liveness watchdog is fed on PIPELINE PROGRESS (mic block
		 * read AND accepted by the model), not merely on loop iteration.
		 * Feeding at the loop top masked the "loop alive, mic dead" wedge:
		 * a persistently failing dmic_read() spun through the error path,
		 * petting the watchdog forever while producing zero inferences
		 * (seen in the field: 45 min deaf with healthy heartbeats). The
		 * error paths below deliberately do NOT feed, so a persistent
		 * DMIC/inference failure trips the 60 s watchdog -> coredump (with
		 * the error visible) -> reboot -> recovered voice path. */
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			/* A DMIC read error is transient (e.g. -EAGAIN: no block within
			 * the timeout, from a momentary CPU stall during Wi-Fi
			 * reconnection). Skip this block and keep listening — a dropped
			 * ~10 ms block is harmless. Crucially, do NOT return: that would
			 * exit the audio loop and leave the toilet deaf until reboot.
			 * No watchdog feed here: if this error persists, the mic is
			 * dead and the audio watchdog must fire.
			 */
			LOG_WRN("DMIC read error %d; skipping block", err);
			if (++dmic_errs >= DMIC_ERRS_BEFORE_RESTART) {
				dmic_errs = 0;
				(void)dmic_restart();
			}
			k_sleep(K_MSEC(5));
			continue;
		}
		dmic_errs = 0;

		audio_proc_run(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_snap_feed(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_stats_update(audio_buffer, DMIC_SAMPLES_IN_BLOCK);

		err = ww_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &ww_detected);
		if (err == -EBUSY) {
			/* More data is needed: the mic delivered audio and the model
			 * accepted it — the pipeline is healthy. */
			audio_watchdog_feed();
			continue;
		} else if (err < 0) {
			/* Edge-AI feed/inference error (e.g. -EPERM). Re-init the model
			 * state and keep listening — do NOT return, which would exit the
			 * audio loop and leave the toilet deaf until reboot (same rationale
			 * as the DMIC error above). dmic_read() at the loop top paces
			 * retries. No watchdog feed: persistent inference failure must
			 * trip the audio watchdog.
			 */
			LOG_WRN("Wakeword detection error %d; resetting model and continuing", err);
			ww_reset();
			continue;
		}

		/* Full inference completed. */
		audio_watchdog_feed();

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
	int dmic_errs = 0;

	uint32_t spotting_timeout = k_uptime_get_32() + CONFIG_KWS_PERIOD_MS;

	kws_reset();

	print_control_output((struct control_message){.type = CONTROL_MESSAGE_WAITING_KW});

	while (IS_ENABLED(CONFIG_APP_MODE_KWS_ONLY) || spotting_timeout > k_uptime_get_32()) {
		/* Watchdog fed on pipeline progress, not loop iteration — see
		 * ww_loop() for rationale. */
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			/* Transient (e.g. -EAGAIN). Skip the block and keep going rather
			 * than exiting the audio loop. See ww_loop() for rationale.
			 * No watchdog feed on the error path.
			 */
			LOG_WRN("DMIC read error %d; skipping block", err);
			if (++dmic_errs >= DMIC_ERRS_BEFORE_RESTART) {
				dmic_errs = 0;
				(void)dmic_restart();
			}
			k_sleep(K_MSEC(5));
			continue;
		}
		dmic_errs = 0;

		audio_proc_run(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_snap_feed(audio_buffer, DMIC_SAMPLES_IN_BLOCK);
		audio_stats_update(audio_buffer, DMIC_SAMPLES_IN_BLOCK);

		err = kws_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &prediction);
		if (err == -EBUSY) {
			/* More data is needed: pipeline healthy. */
			audio_watchdog_feed();
			continue;
		} else if (err) {
			/* Recover and keep going rather than exiting the audio loop.
			 * See ww_loop() for rationale.
			 */
			LOG_WRN("Keyword spotting error %d; resetting model and continuing", err);
			kws_reset();
			continue;
		}

		/* Full inference completed. */
		audio_watchdog_feed();

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

	/* Start audio immediately -- the DMIC + edge-AI inference are the toilet's
	 * core function and must not wait on the network. The cloud thread brings up
	 * Wi-Fi/Memfault in parallel via conn_mgr. Earlier builds waited up to 30 s
	 * for Wi-Fi first so its TLS bring-up wouldn't compete with inference for CPU,
	 * but that cost an up-to-30 s voice outage after every recovery reboot; now
	 * that the mic has its own liveness/quality watchdog and Wi-Fi recovers
	 * without a reboot, we start audio first and let Wi-Fi race. (The DMIC is
	 * DMA-fed, so no samples are lost if inference briefly jitters during the
	 * concurrent TLS handshake.) */
	LOG_INF("Starting audio capture immediately; Wi-Fi/cloud come up in parallel");
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
