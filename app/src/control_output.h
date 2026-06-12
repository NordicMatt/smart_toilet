/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup control_output Printing control output functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __CONTROL_OUTPUT_H__
#define __CONTROL_OUTPUT_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Control message type.
 */
enum control_message_type {
	CONTROL_MESSAGE_WAITING_WW,
	CONTROL_MESSAGE_WW_DETECTED,

	CONTROL_MESSAGE_WAITING_KW,
	CONTROL_MESSAGE_KW_SPOTTED,
	CONTROL_MESSAGE_TIMEOUT_KWS,

	CONTROL_MESSAGE_COUNT
};

/**
 * @brief Control message.
 */
struct control_message {
	enum control_message_type type;
	uint16_t kw_class;
	const char *name;
};

/**
 * @brief Initialize control output backend.
 *
 * @return Operation status, 0 for success.
 */
int control_output_init(void);

/**
 * @brief Print the control messages to @c ncs_control_output_uart serial.
 *
 * @param message Control message to be printed.
 */
void print_control_output(const struct control_message message);

/**
 * @brief Register a consumer for bytes received on the control UART.
 *
 * Enables reception; @p cb is called from interrupt context per byte.
 *
 * @param cb Callback invoked for every received byte.
 * @return Operation status, 0 for success.
 */
int control_input_register(void (*cb)(uint8_t byte));

/**
 * @brief Enter raw binary output mode.
 *
 * Queued control messages are held back until @c control_output_raw_end
 * so text cannot interleave with binary data. Blocks until an in-flight
 * message has finished transmitting.
 */
void control_output_raw_begin(void);

/**
 * @brief Leave raw binary output mode and flush held-back messages.
 */
void control_output_raw_end(void);

/**
 * @brief Transmit a raw buffer on the control UART, blocking until sent.
 *
 * Only valid between @c control_output_raw_begin and
 * @c control_output_raw_end. The buffer must stay valid for the call.
 *
 * @param buffer Data to transmit.
 * @param len Number of bytes.
 * @return Operation status, 0 for success.
 */
int control_output_raw_tx(const uint8_t *buffer, size_t len);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __CONTROL_OUTPUT_H__ */

/**
 * @}
 */
