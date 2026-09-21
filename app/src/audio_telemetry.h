/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup audio_telemetry Audio/wake-word telemetry to Memfault
 * @{
 *
 * Feeds per-interval audio level and wake-word metrics into Memfault heartbeat
 * metrics so wake-word behaviour can be observed remotely (off wall USB, no
 * serial). No-ops when Memfault is not enabled.
 */

#ifndef __AUDIO_TELEMETRY_H__
#define __AUDIO_TELEMETRY_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef CONFIG_MEMFAULT

/** @brief Report one second of audio levels (peak/RMS in dBFS, clipped count). */
void audio_telemetry_levels(float peak_db, float rms_db, uint32_t clipped);

/** @brief Report a wake-word inference probability (0.0-1.0). */
void audio_telemetry_prob(float prob);

/** @brief Report a confirmed wake-word detection. */
void audio_telemetry_detection(void);

/** @brief Report a detection accepted at the lowered retry bar. */
void audio_telemetry_retry_detection(void);

/**
 * @brief Report a near miss: a probability run that ended without a detection.
 *
 * @param peak_pct Peak probability of the run, percent (0-100).
 */
void audio_telemetry_near_miss(uint32_t peak_pct);

/**
 * @brief Note a failsafe-button press (ISR-safe). Counted as a confirmed miss
 *        if it lands within CONFIG_WW_MISS_BUTTON_WINDOW_MS of a near miss.
 */
void audio_telemetry_button_press(void);

#else /* CONFIG_MEMFAULT */

static inline void audio_telemetry_levels(float peak_db, float rms_db, uint32_t clipped)
{
}
static inline void audio_telemetry_prob(float prob)
{
}
static inline void audio_telemetry_detection(void)
{
}
static inline void audio_telemetry_retry_detection(void)
{
}
static inline void audio_telemetry_near_miss(uint32_t peak_pct)
{
}
static inline void audio_telemetry_button_press(void)
{
}

#endif /* CONFIG_MEMFAULT */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_TELEMETRY_H__ */

/**
 * @}
 */
