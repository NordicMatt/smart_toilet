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

#endif /* CONFIG_MEMFAULT */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_TELEMETRY_H__ */

/**
 * @}
 */
