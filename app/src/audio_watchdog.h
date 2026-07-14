/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup audio_watchdog Liveness watchdog for the wake-word audio loop
 * @{
 *
 * Software watchdog that guards the DMIC + edge-AI audio loop in main.c. The
 * loop feeds it every iteration; if it makes no progress for
 * AUDIO_STALL_REBOOT_S (blocked in dmic_read / inference, i.e. the toilet has
 * gone silently deaf while the rest of the system - Wi-Fi, motor, the GPIO-ISR
 * flush button - keeps running), a Memfault coredump is captured so the stuck
 * backtrace is recoverable, then the device reboots to auto-recover.
 *
 * Mirrors the cloud thread's stall monitor in cloud.c, but captures a coredump
 * (MEMFAULT_TASK_WATCHDOG) rather than only marking a reboot reason.
 *
 * No-ops when Memfault is not enabled.
 */

#ifndef __AUDIO_WATCHDOG_H__
#define __AUDIO_WATCHDOG_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef CONFIG_MEMFAULT

/**
 * @brief Arm the audio-loop watchdog.
 *
 * Call once, immediately before entering the audio loop (after the DMIC is
 * triggered), so the network wait and one-time init at boot cannot trip it.
 */
void audio_watchdog_start(void);

/**
 * @brief Note forward progress of the audio loop.
 *
 * Call once per loop iteration. A dropped/retried block still counts as
 * progress; only a true wedge (no iteration for AUDIO_STALL_REBOOT_S) trips it.
 */
void audio_watchdog_feed(void);

/**
 * @brief Report whether the last second of mic audio was usable.
 *
 * The liveness feed above only proves the loop iterates and the model accepts
 * blocks -- it cannot tell real audio from a wedged PDM delivering saturated
 * garbage, which keeps the pipeline "alive" yet deaf. Call once per one-second
 * stats window with @p healthy = false when the mic is railed and clipping the
 * majority of samples. If no healthy second is seen for MIC_STUCK_REBOOT_S, the
 * watchdog reboots to fully re-init the DMIC.
 *
 * @param healthy True if the second carried plausibly-real audio.
 */
void audio_watchdog_note_audio_quality(bool healthy);

#else /* CONFIG_MEMFAULT */

static inline void audio_watchdog_start(void)
{
}
static inline void audio_watchdog_feed(void)
{
}
static inline void audio_watchdog_note_audio_quality(bool healthy)
{
	(void)healthy;
}

#endif /* CONFIG_MEMFAULT */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_WATCHDOG_H__ */

/**
 * @}
 */
