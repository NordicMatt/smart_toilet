/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Liveness watchdog for the wake-word audio loop (main.c). See audio_watchdog.h.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#ifdef CONFIG_TASK_WDT
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <hal/nrf_wdt.h>
#endif

#include <memfault/panics/assert.h>

#include "audio_watchdog.h"

/* Reboot if the audio loop makes no progress for this long. Nothing in the loop
 * legitimately blocks beyond the DMIC read timeout (DMIC_READ_TIMEOUT = 100 ms),
 * and transient DMIC/inference errors retry with a 5 ms sleep, so anywhere near
 * this long means the thread is truly wedged in a blocking call. Kept well above
 * the loop's real worst case to avoid tripping on momentary CPU starvation
 * during Wi-Fi bring-up. */
#define AUDIO_STALL_REBOOT_S 60
/* How often the monitor samples the progress timestamp. */
#define STALL_CHECK_PERIOD_S 15

/* Bad-data recovery: the liveness monitor above is fed on pipeline progress -- a
 * block read AND accepted by the model -- so it does NOT catch a mic that keeps
 * delivering blocks the model accepts but which carry no real audio. A PDM
 * peripheral can wedge into a saturated, free-running mode (peak pinned to full
 * scale, the majority of samples clipping, wake-word probability stuck at 0);
 * the pipeline looks alive while the toilet is stone deaf. This is exactly what
 * left both units deaf on v2.0.11. If no usable-audio second is seen for this
 * long, force the same recover-by-reboot (which fully re-inits the DMIC). 5 min
 * is far above any real overload -- a bathroom never sustains >50% clipping that
 * long -- so a false reboot of a working mic is not a practical risk. */
#define MIC_STUCK_REBOOT_S 300
#ifdef CONFIG_TASK_WDT
LOG_MODULE_REGISTER(audio_wdt, LOG_LEVEL_INF);

/* Task-watchdog channel backstop: if the audio loop stops feeding for this long,
 * task_wdt_cb() captures a coredump and resets. Longer than AUDIO_STALL_REBOOT_S so
 * the software monitor fires first for an ordinary thread stall.
 *
 * This is NOT the hardware watchdog's timeout. task_wdt programs the nRF54 wdt31
 * with CONFIG_TASK_WDT_MIN_TIMEOUT + CONFIG_TASK_WDT_HW_FALLBACK_DELAY and feeds it
 * from its own timer; the hardware only fires if the kernel itself is dead. Up to
 * 3.0.4 that window was the 100 + 20 ms default, and because a soft reset does not
 * stop the nRF WDT, MCUboot and the next image inherited it: on 2026-10-29/30 every
 * FOTA to Toilet #2 ended in a 120 ms reset during the swap or the new image's first
 * boot, then a revert, a re-download and a 4-minute loop (reproduced on the bench).
 * prj.conf now sets a 10 s window, and inherited_wdt_stop() below stops a watchdog
 * left running by the previous image. */
#define HW_WDT_TIMEOUT_MS 600000
static int task_wdt_ch = -1;
#endif

/* Uptime (seconds) of the audio loop's last sign of life, fed every iteration.
 * Read by the monitor timer (ISR context). */
static atomic_t last_progress_s;
/* Uptime (seconds) of the last second of plausibly-real mic audio (not
 * saturated-and-clipping). Fed from the audio thread via
 * audio_watchdog_note_audio_quality(); read by the monitor timer. */
static atomic_t last_good_audio_s;
/* Set once audio_watchdog_start() has armed the monitor; keeps the timer from
 * firing against a stale (zero) timestamp before the loop starts feeding it. */
static atomic_t armed = ATOMIC_INIT(0);

void audio_watchdog_note_audio_quality(bool healthy)
{
	if (healthy) {
		atomic_set(&last_good_audio_s,
			   (atomic_val_t)(k_uptime_get_32() / MSEC_PER_SEC));
	}
}

void audio_watchdog_feed(void)
{
	atomic_set(&last_progress_s, (atomic_val_t)(k_uptime_get_32() / MSEC_PER_SEC));
#ifdef CONFIG_TASK_WDT
	if (task_wdt_ch >= 0) {
		(void)task_wdt_feed(task_wdt_ch);
	}
#endif
}

/* Runs in timer (ISR) context, so it fires even if every thread is wedged. If
 * the audio loop has not iterated for AUDIO_STALL_REBOOT_S it is parked in an
 * unbounded call (dmic_read / edge-AI inference); there is no safe way to kill a
 * thread blocked in a syscall, so capture a coredump and reboot. The coredump's
 * all-thread stack collection (MEMFAULT_COREDUMP_COLLECT_TASKS_REGIONS) records
 * the main thread's backtrace = the wedge point; RESET_ON_FATAL_ERROR then
 * reboots, and the voice path recovers on the next boot. */
static void audio_stall_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (atomic_get(&armed) != 1) {
		return;
	}

	const uint32_t now_s = k_uptime_get_32() / MSEC_PER_SEC;
	const uint32_t last_s = (uint32_t)atomic_get(&last_progress_s);

	if ((now_s - last_s) < AUDIO_STALL_REBOOT_S) {
		return;
	}

	/* TaskWatchdog reason (vs. the cloud monitor's SoftwareWatchdog) so an
	 * audio-loop stall groups as a distinct issue in Memfault. */
	MEMFAULT_TASK_WATCHDOG();
}

static K_TIMER_DEFINE(audio_stall_timer, audio_stall_expiry, NULL);

/* Companion to audio_stall_expiry for the "deaf but flowing" case: the audio
 * loop is progressing (so the liveness monitor stays fed) yet the mic has
 * produced no usable audio for MIC_STUCK_REBOOT_S -- a PDM wedged into a
 * saturated/free-running mode that a plain capture restart cannot clear. Capture
 * a coredump and reboot so the next boot's full dmic_init()/dmic_configure()
 * re-initialises the peripheral. Separate call site and timer so it groups as
 * its own Memfault issue, giving fleet visibility into how often the mic wedges.
 * Runs in timer (ISR) context. */
static void mic_stuck_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (atomic_get(&armed) != 1) {
		return;
	}

	const uint32_t now_s = k_uptime_get_32() / MSEC_PER_SEC;
	const uint32_t last_s = (uint32_t)atomic_get(&last_good_audio_s);

	if ((now_s - last_s) < MIC_STUCK_REBOOT_S) {
		return;
	}

	MEMFAULT_TASK_WATCHDOG();
}

static K_TIMER_DEFINE(mic_stuck_timer, mic_stuck_expiry, NULL);

#ifdef CONFIG_TASK_WDT
/* task_wdt timeout callback (ISR context). Reached only if the audio loop stalled
 * past HW_WDT_TIMEOUT_MS while the kernel is still alive enough to run task_wdt
 * (the software monitor above should have fired first). Capture a coredump, then
 * reset. A true total lockup skips this -- the hardware WDT resets with no callback. */
static void task_wdt_cb(int channel_id, void *user_data)
{
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);
	MEMFAULT_TASK_WATCHDOG();
}
#endif

#ifdef CONFIG_TASK_WDT
/* Set when inherited_wdt_stop() found the hardware watchdog still running from the
 * previous image; logged once the logger is up. */
static bool inherited_wdt_was_running;
static bool inherited_wdt_stopped;

/* A soft reset (FOTA apply, coredump reboot, reason-marked reboot) leaves the nRF WDT
 * counting with the previous image's configuration, which task_wdt_init() cannot
 * change: the CRV/CONFIG registers are locked while it runs, and the driver's RAM
 * state says "not started". MCUboot feeds it during a swap, but nothing feeds it in
 * this image until audio_watchdog_start(), seconds later, so this image would be
 * reset before it boots far enough to confirm itself -- and MCUboot then reverts.
 * Stop it before the drivers initialise; the previous image started it with STOPEN
 * set (wdt_nrfx sets NRF_WDT_BEHAVIOUR_STOP_ENABLE_MASK), and task_wdt starts it
 * afresh later. PRE_KERNEL_1 runs within a few ms of reset, inside any window. */
static int inherited_wdt_stop(void)
{
	NRF_WDT_Type *const wdt = (NRF_WDT_Type *)DT_REG_ADDR(DT_ALIAS(watchdog0));

	if (!nrf_wdt_started_check(wdt)) {
		return 0;
	}
	inherited_wdt_was_running = true;

	if (!(wdt->CONFIG & WDT_CONFIG_STOPEN_Msk)) {
		/* Not stoppable: feed every reload channel once, as MCUboot does, so at
		 * least a full window is left. */
		for (uint8_t i = 0; i < NRF_WDT_CHANNEL_NUMBER; i++) {
			nrf_wdt_reload_request_set(wdt, (nrf_wdt_rr_register_t)(NRF_WDT_RR0 + i));
		}
		return 0;
	}

	nrf_wdt_event_clear(wdt, NRF_WDT_EVENT_STOPPED);
	nrf_wdt_task_stop_enable_set(wdt, true);
	nrf_wdt_task_trigger(wdt, NRF_WDT_TASK_STOP);
	nrf_wdt_task_stop_enable_set(wdt, false);

	/* STOPPED follows within a few 32 kHz cycles; bound the wait. */
	for (uint32_t spin = 0; spin < 100000U; spin++) {
		if (!nrf_wdt_started_check(wdt)) {
			inherited_wdt_stopped = true;
			break;
		}
	}
	return 0;
}

SYS_INIT(inherited_wdt_stop, PRE_KERNEL_1, 0);
#endif

void audio_watchdog_start(void)
{
#ifdef CONFIG_TASK_WDT
	/* Arm the hardware-backed task watchdog. The nRF54 wdt31 (aliased
	 * watchdog0) is the fallback: even a total CPU lockup that stops every
	 * k_timer still gets a hardware reset. Best-effort -- if the WDT is not
	 * ready the software monitor below still runs. */
	const struct device *hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	if (inherited_wdt_was_running) {
		LOG_WRN("Hardware watchdog was still running from the previous image: %s",
			inherited_wdt_stopped ? "stopped at boot" : "NOT stoppable, fed once");
	}

	int err = device_is_ready(hw_wdt) ? task_wdt_init(hw_wdt) : -ENODEV;

	if (err == 0) {
		task_wdt_ch = task_wdt_add(HW_WDT_TIMEOUT_MS, task_wdt_cb, NULL);
	} else {
		LOG_ERR("task_wdt init failed (err %d); hardware watchdog not armed", err);
	}
#endif
	audio_watchdog_feed();
	audio_watchdog_note_audio_quality(true);
	atomic_set(&armed, 1);
	k_timer_start(&audio_stall_timer, K_SECONDS(STALL_CHECK_PERIOD_S),
		      K_SECONDS(STALL_CHECK_PERIOD_S));
	k_timer_start(&mic_stuck_timer, K_SECONDS(STALL_CHECK_PERIOD_S),
		      K_SECONDS(STALL_CHECK_PERIOD_S));
}
