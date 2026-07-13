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
#include <zephyr/task_wdt/task_wdt.h>
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
#ifdef CONFIG_TASK_WDT
/* Hardware-backed backstop: if the audio loop stops feeding for this long, the
 * task watchdog -- and, if even the kernel clock is dead, the nRF54 hardware WDT
 * it sits on -- forces a reset. Longer than AUDIO_STALL_REBOOT_S so the software
 * monitor fires first (with a coredump) for an ordinary thread stall; this layer
 * exists for what software CANNOT catch: a total lockup where the sysclock/ISRs
 * are dead so no k_timer runs at all.
 *
 * Sized to survive a FOTA apply: the nRF WDT keeps counting across the soft
 * reset into MCUboot, whose swap runs against the external MX25R64 SPI-NOR
 * secondary slot and can outlast the old 120 s budget -- every OTA then ended
 * in a mid-swap WDT reset, reported fleet-wide as a bogus "Hardware Watchdog"
 * reboot instead of "Firmware Update" (reboot history 2026-07-10..13: one per
 * version boundary, on both devices). 10 min clears the slowest swap while
 * still catching a true total lockup; the 60 s software layers above remain
 * the fast, coredump-producing detectors. */
#define HW_WDT_TIMEOUT_MS 600000
static int task_wdt_ch = -1;
#endif

/* Uptime (seconds) of the audio loop's last sign of life, fed every iteration.
 * Read by the monitor timer (ISR context). */
static atomic_t last_progress_s;
/* Set once audio_watchdog_start() has armed the monitor; keeps the timer from
 * firing against a stale (zero) timestamp before the loop starts feeding it. */
static atomic_t armed = ATOMIC_INIT(0);

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

void audio_watchdog_start(void)
{
#ifdef CONFIG_TASK_WDT
	/* Arm the hardware-backed task watchdog. The nRF54 wdt31 (aliased
	 * watchdog0) is the fallback: even a total CPU lockup that stops every
	 * k_timer still gets a hardware reset. Best-effort -- if the WDT is not
	 * ready the software monitor below still runs. */
	const struct device *hw_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	if (device_is_ready(hw_wdt) && task_wdt_init(hw_wdt) == 0) {
		task_wdt_ch = task_wdt_add(HW_WDT_TIMEOUT_MS, task_wdt_cb, NULL);
	}
#endif
	audio_watchdog_feed();
	atomic_set(&armed, 1);
	k_timer_start(&audio_stall_timer, K_SECONDS(STALL_CHECK_PERIOD_S),
		      K_SECONDS(STALL_CHECK_PERIOD_S));
}
