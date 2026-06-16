/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Control output: emits short status strings on the console (VCOM0 / uart30).
 *
 * NOTE: with the nRF7002 EB II shield, the console is rerouted to uart30 -- the
 * same UART this used to own via the async API. Two owners on one UART break
 * console output, so we now print through the console (printk) instead. Status
 * messages share VCOM0 with the Zephyr log.
 */

#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "control_output.h"

static const char *const messages[] = {
	[CONTROL_MESSAGE_WAITING_WW] = "Waiting for wakeword\r\n",
	[CONTROL_MESSAGE_WW_DETECTED] = "Wakeword detected\r\n",

	[CONTROL_MESSAGE_WAITING_KW] = "Waiting for keywords\r\n",
	[CONTROL_MESSAGE_KW_SPOTTED] = "Keyword spotted: %s\r\n",
	[CONTROL_MESSAGE_TIMEOUT_KWS] = "Keyword spotting window timeout\r\n",
};

BUILD_ASSERT(CONTROL_MESSAGE_COUNT == ARRAY_SIZE(messages),
	     "Mismatch between control_message_type and messages size");

int control_output_init(void)
{
	return 0;
}

void print_control_output(const struct control_message message)
{
	if (message.type >= CONTROL_MESSAGE_COUNT) {
		return;
	}

	if (message.type == CONTROL_MESSAGE_KW_SPOTTED) {
		printk(messages[message.type], message.name);
	} else {
		printk("%s", messages[message.type]);
	}
}
