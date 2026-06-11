/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup actuator Flush actuator control functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __ACTUATOR_H__
#define __ACTUATOR_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Initialize the flush motor drive and Hall sensor GPIOs.
 *
 * @return Operation status, 0 for success.
 */
int actuator_init(void);

/**
 * @brief Trigger a flush: run the motor until the shaft magnet is sensed by
 *        the Hall sensor (or a safety timeout elapses), then stop. Requests
 *        received while the motor runs or during the lockout are ignored.
 */
void actuator_flush(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __ACTUATOR_H__ */

/**
 * @}
 */
