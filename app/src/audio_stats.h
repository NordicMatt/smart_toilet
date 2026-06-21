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

#if defined(CONFIG_APP_AUDIO_STATS) || defined(CONFIG_MEMFAULT)
/**
 * @brief Accumulate level statistics for one block of 16-bit PCM audio.
 *
 * Once per second, computes the mic peak/RMS level in dBFS and a clipped-sample
 * count. Logs them to UART when CONFIG_APP_AUDIO_STATS is set, and forwards them
 * to the Memfault audio metrics when CONFIG_MEMFAULT is set.
 *
 * @param buffer Audio buffer of 16-bit signed samples.
 * @param num_samples Number of samples in the buffer.
 */
void audio_stats_update(const void *buffer, size_t num_samples);
#else
static inline void audio_stats_update(const void *buffer, size_t num_samples)
{
}
#endif /* CONFIG_APP_AUDIO_STATS || CONFIG_MEMFAULT */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_STATS_H__ */

/**
 * @}
 */
