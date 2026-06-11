/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup audio_proc Audio front-end cleanup (high-pass filter, AGC)
 * @{
 * @ingroup ww_kws
 */

#ifndef __AUDIO_PROC_H__
#define __AUDIO_PROC_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef CONFIG_APP_AUDIO_PROC
/**
 * @brief Clean up one block of 16-bit PCM audio in place.
 *
 * Applies the 120 Hz high-pass filter (APP_AUDIO_HPF) and/or the software
 * automatic gain control (APP_AGC) before the block reaches detection.
 *
 * @param buffer Audio buffer of 16-bit signed samples, modified in place.
 * @param num_samples Number of samples in the buffer.
 */
void audio_proc_run(void *buffer, size_t num_samples);
#else
static inline void audio_proc_run(void *buffer, size_t num_samples)
{
}
#endif /* CONFIG_APP_AUDIO_PROC */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_PROC_H__ */

/**
 * @}
 */
