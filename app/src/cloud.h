/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef CLOUD_H_
#define CLOUD_H_

#include <stdbool.h>

/** @brief Whether the device is currently connected to nRF Cloud. */
bool cloud_is_connected(void);

#endif /* CLOUD_H_ */
