/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup audio_stats Audio level statistics for mic tuning
 * @{
 * @ingroup ww_kws
 */

#ifndef __AUDIO_STATS_H__
#define __AUDIO_STATS_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef CONFIG_APP_AUDIO_STATS
/**
 * @brief Accumulate level statistics for one block of 16-bit PCM audio.
 *
 * Logs peak/RMS level in dBFS and a clipped-sample count once per second.
 *
 * @param buffer Audio buffer of 16-bit signed samples.
 * @param num_samples Number of samples in the buffer.
 */
void audio_stats_update(const void *buffer, size_t num_samples);
#else
static inline void audio_stats_update(const void *buffer, size_t num_samples)
{
}
#endif /* CONFIG_APP_AUDIO_STATS */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_STATS_H__ */

/**
 * @}
 */
