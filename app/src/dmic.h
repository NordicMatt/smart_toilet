/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup dmic DMIC control functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __DMIC_H__
#define __DMIC_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define SAMPLES_BLOCK_LENGTH_MS (10)
#define DMIC_SAMPLE_BYTES	(2)
#define DMIC_PCM_RATE		(16000)
#define DMIC_SAMPLES_IN_BLOCK	(DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000)

/**
 * @brief Initialize DMIC.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_init(void);

/**
 * @brief Free the audio buffer acquired with @c dmic_read.
 *
 * @param buffer Audio buffer.
 */
void free_dmic_buffer(void *buffer);

/**
 * @brief Restart PDM capture after the driver has wedged.
 *
 * A PDM ring overrun (CPU stalled by e.g. Wi-Fi scan work) halts capture;
 * dmic_read() then returns -EAGAIN forever until a STOP/START retrigger.
 * Call after several consecutive read failures. Re-applies the PDM gain.
 *
 * @return 0 on success, negative errno if the START trigger failed.
 */
int dmic_restart(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __DMIC_H__ */

/**
 * @}
 */
