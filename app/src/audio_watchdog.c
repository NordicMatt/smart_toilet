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

/* Uptime (seconds) of the audio loop's last sign of life, fed every iteration.
 * Read by the monitor timer (ISR context). */
static atomic_t last_progress_s;
/* Set once audio_watchdog_start() has armed the monitor; keeps the timer from
 * firing against a stale (zero) timestamp before the loop starts feeding it. */
static atomic_t armed = ATOMIC_INIT(0);

void audio_watchdog_feed(void)
{
	atomic_set(&last_progress_s, (atomic_val_t)(k_uptime_get_32() / MSEC_PER_SEC));
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

void audio_watchdog_start(void)
{
	audio_watchdog_feed();
	atomic_set(&armed, 1);
	k_timer_start(&audio_stall_timer, K_SECONDS(STALL_CHECK_PERIOD_S),
		      K_SECONDS(STALL_CHECK_PERIOD_S));
}
