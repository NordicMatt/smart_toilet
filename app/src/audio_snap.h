/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup audio_snap Audio snapshot recording over the control UART
 * @{
 * @ingroup ww_kws
 */

#ifndef __AUDIO_SNAP_H__
#define __AUDIO_SNAP_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef CONFIG_APP_AUDIO_SNAP
/**
 * @brief Start listening for the snapshot trigger on the control UART.
 *
 * On receiving the trigger byte 'S', the next APP_AUDIO_SNAP_SECONDS of
 * audio fed via @c audio_snap_feed are recorded to RAM, then dumped back
 * over the control UART as an "AUDIO_SNAP <bytes>" header followed by raw
 * 16-bit little-endian PCM. Save it with: tools/uart_monitor.py snap.
 *
 * @return Operation status, 0 for success.
 */
int audio_snap_init(void);

/**
 * @brief Offer one block of 16-bit PCM audio to the recorder.
 *
 * Copies the block while a snapshot is being recorded, otherwise no-op.
 *
 * @param buffer Audio buffer of 16-bit signed samples.
 * @param num_samples Number of samples in the buffer.
 */
void audio_snap_feed(const void *buffer, size_t num_samples);
#else
static inline int audio_snap_init(void)
{
	return 0;
}

static inline void audio_snap_feed(const void *buffer, size_t num_samples)
{
}
#endif /* CONFIG_APP_AUDIO_SNAP */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __AUDIO_SNAP_H__ */

/**
 * @}
 */
