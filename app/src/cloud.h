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

/**
 * @brief Notify the cloud layer that a flush has occurred.
 *
 * Safe to call from any thread (e.g. the motor thread): it only bumps a
 * counter and wakes the cloud thread, which then reports the flush event and
 * updated flush count to nRF Cloud. Never blocks and never calls into the
 * CoAP stack directly.
 */
void cloud_report_flush(void);

#endif /* CLOUD_H_ */
