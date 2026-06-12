/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "control_output.h"

LOG_MODULE_REGISTER(control_output);

struct k_work control_output_work;

K_MSGQ_DEFINE(control_msg_queue, sizeof(struct control_message), 10,
	      alignof(struct control_message));

static const char *const messages[] = {
	[CONTROL_MESSAGE_WAITING_WW] = "Waiting for wakeword\r\n",
	[CONTROL_MESSAGE_WW_DETECTED] = "Wakeword detected\r\n",

	[CONTROL_MESSAGE_WAITING_KW] = "Waiting for keywords\r\n",
	[CONTROL_MESSAGE_KW_SPOTTED] = "Keyword spotted: %s\r\n",
	[CONTROL_MESSAGE_TIMEOUT_KWS] = "Keyword spotting window timeout\r\n",
};

BUILD_ASSERT(CONTROL_MESSAGE_COUNT == ARRAY_SIZE(messages),
	     "Mismatch between control_message_type and messages size");

static const struct device *uart_dev = DEVICE_DT_GET(DT_CHOSEN(ncs_control_output_uart));
static atomic_t uart_busy;

/* Raw binary mode (audio snapshot dumps): while active, queued control
 * messages are held back so text cannot interleave with binary data, and
 * TX completions wake the dumping thread instead of the message worker.
 */
static atomic_t raw_active;
static K_SEM_DEFINE(raw_tx_sem, 0, 1);

/* Single registered consumer of bytes received on the control UART. */
static void (*input_cb)(uint8_t byte);
static uint8_t rx_bufs[2][8];
static uint8_t rx_buf_idx;

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	int err;

	switch (evt->type) {
	case UART_TX_DONE:
		atomic_set(&uart_busy, false);

		if (atomic_get(&raw_active)) {
			k_sem_give(&raw_tx_sem);
			return;
		}

		if (k_msgq_num_used_get(&control_msg_queue) == 0) {
			return;
		}

		err = k_work_submit(&control_output_work);
		if (err < 0) {
			LOG_ERR("Failed to submit work (err %d)", err);
		}
		break;

	case UART_RX_RDY:
		for (size_t i = 0; i < evt->data.rx.len; i++) {
			input_cb(evt->data.rx.buf[evt->data.rx.offset + i]);
		}
		break;

	case UART_RX_BUF_REQUEST:
		rx_buf_idx ^= 1;
		err = uart_rx_buf_rsp(dev, rx_bufs[rx_buf_idx], sizeof(rx_bufs[0]));
		if (err) {
			LOG_ERR("Failed to provide RX buffer (err %d)", err);
		}
		break;

	case UART_RX_DISABLED:
		err = uart_rx_enable(dev, rx_bufs[rx_buf_idx], sizeof(rx_bufs[0]),
				     10 * USEC_PER_MSEC);
		if (err) {
			LOG_ERR("Failed to re-enable RX (err %d)", err);
		}
		break;

	default:
		break;
	}
}

static void control_output_work_handler(struct k_work *work)
{
	int err;
	struct control_message message_item;

	static char output_buffer[40];
	const char *buffer = NULL;
	size_t buffer_len = 0;

	if (atomic_get(&raw_active) || atomic_get(&uart_busy)) {
		return;
	}

	/* Peek message to not lose it if uart_tx fails. */
	err = k_msgq_peek(&control_msg_queue, &message_item);
	if (err) {
		return;
	}

	atomic_set(&uart_busy, true);

	switch (message_item.type) {
	case CONTROL_MESSAGE_WAITING_WW:
	case CONTROL_MESSAGE_WW_DETECTED:
	case CONTROL_MESSAGE_WAITING_KW:
	case CONTROL_MESSAGE_TIMEOUT_KWS:
		buffer = messages[message_item.type];
		buffer_len = strlen(buffer);
		break;

	case CONTROL_MESSAGE_KW_SPOTTED:
		buffer = output_buffer;
		buffer_len = snprintf(output_buffer, sizeof(output_buffer),
				      messages[message_item.type], message_item.name);
		__ASSERT(buffer_len >= 0, "Error in snprintf call (%d)", buffer_len);
		__ASSERT(buffer_len < sizeof(output_buffer), "Output buffer is too small");

		break;
	default:
		atomic_set(&uart_busy, false);
		k_msgq_get(&control_msg_queue, &message_item, K_NO_WAIT);
		LOG_WRN("Unhandled case");
		return;
	}

	err = uart_tx(uart_dev, buffer, buffer_len, 0);
	if (err) {
		atomic_set(&uart_busy, false);
		LOG_ERR("Failed to transmit data (err %d)", err);
		return;
	}

	k_msgq_get(&control_msg_queue, &message_item, K_NO_WAIT);
}

int control_input_register(void (*cb)(uint8_t byte))
{
	input_cb = cb;

	return uart_rx_enable(uart_dev, rx_bufs[0], sizeof(rx_bufs[0]), 10 * USEC_PER_MSEC);
}

void control_output_raw_begin(void)
{
	atomic_set(&raw_active, true);

	/* Let an in-flight control message finish, and discard the stale
	 * wake-up its completion posts, so binary data starts clean.
	 */
	while (atomic_get(&uart_busy)) {
		k_msleep(1);
	}
	k_sem_reset(&raw_tx_sem);
}

void control_output_raw_end(void)
{
	atomic_set(&raw_active, false);

	if (k_msgq_num_used_get(&control_msg_queue) > 0) {
		(void)k_work_submit(&control_output_work);
	}
}

int control_output_raw_tx(const uint8_t *buffer, size_t len)
{
	int err;

	atomic_set(&uart_busy, true);

	err = uart_tx(uart_dev, buffer, len, SYS_FOREVER_US);
	if (err) {
		atomic_set(&uart_busy, false);
		return err;
	}

	k_sem_take(&raw_tx_sem, K_FOREVER);

	return 0;
}

int control_output_init(void)
{
	int err;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("Device is not ready");
		return -ENODEV;
	}

	err = uart_callback_set(uart_dev, uart_cb, NULL);
	if (err) {
		LOG_ERR("Failed to set UART callback (err %d)", err);
	}

	k_work_init(&control_output_work, control_output_work_handler);

	return err;
}

void print_control_output(const struct control_message message)
{
	int err;

	err = k_msgq_put(&control_msg_queue, &message, K_NO_WAIT);
	if (err) {
		LOG_ERR("Failed to put message in queue (err %d)", err);
		return;
	}

	err = k_work_submit(&control_output_work);
	if (err < 0) {
		LOG_ERR("Failed to submit work (err %d)", err);
	}
}
