/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "audio_snap.h"
#include "control_output.h"
#include "dmic.h"

LOG_MODULE_REGISTER(audio_snap);

/* Trigger byte received on the control UART that arms a recording. */
#define SNAP_TRIGGER 'S'
/* One async UART transfer at a time; keeps each DMA well within limits. */
#define SNAP_CHUNK 4096

#define SNAP_SAMPLES (CONFIG_APP_AUDIO_SNAP_SECONDS * DMIC_PCM_RATE)

enum snap_state {
	SNAP_IDLE,
	SNAP_RECORDING,
	SNAP_DUMPING,
};

static atomic_t state = ATOMIC_INIT(SNAP_IDLE);
/* 32 KB of RAM per configured second. */
static int16_t snap_buf[SNAP_SAMPLES];
static uint32_t snap_pos;

static K_SEM_DEFINE(dump_sem, 0, 1);

/* Runs in UART interrupt context. */
static void input_byte(uint8_t byte)
{
	if (byte != SNAP_TRIGGER) {
		return;
	}

	if (!atomic_cas(&state, SNAP_IDLE, SNAP_RECORDING)) {
		LOG_WRN("snap: trigger ignored, recording or dump in progress");
		return;
	}

	LOG_INF("snap: recording %d s", CONFIG_APP_AUDIO_SNAP_SECONDS);
}

void audio_snap_feed(const void *buffer, size_t num_samples)
{
	if (atomic_get(&state) != SNAP_RECORDING) {
		return;
	}

	const size_t n = MIN(num_samples, SNAP_SAMPLES - snap_pos);

	memcpy(&snap_buf[snap_pos], buffer, n * sizeof(int16_t));
	snap_pos += n;

	if (snap_pos == SNAP_SAMPLES) {
		atomic_set(&state, SNAP_DUMPING);
		k_sem_give(&dump_sem);
	}
}

static void snap_dump_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		k_sem_take(&dump_sem, K_FOREVER);

		char header[24];
		const uint32_t len = snap_pos * sizeof(int16_t);
		const int header_len =
			snprintf(header, sizeof(header), "AUDIO_SNAP %u\r\n", len);

		LOG_INF("snap: dumping %u bytes", len);

		control_output_raw_begin();

		int err = control_output_raw_tx((const uint8_t *)header, header_len);
		const uint8_t *data = (const uint8_t *)snap_buf;

		for (uint32_t off = 0; !err && off < len; off += SNAP_CHUNK) {
			err = control_output_raw_tx(&data[off], MIN(SNAP_CHUNK, len - off));
		}

		control_output_raw_end();

		if (err) {
			LOG_ERR("snap: dump failed (err %d)", err);
		} else {
			LOG_INF("snap: dump complete");
		}

		snap_pos = 0;
		atomic_set(&state, SNAP_IDLE);
	}
}

K_THREAD_DEFINE(snap_tid, 1024, snap_dump_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(10), 0, 0);

int audio_snap_init(void)
{
	int err = control_input_register(input_byte);

	if (err) {
		LOG_ERR("Failed to enable control UART input (err %d)", err);
	}

	return err;
}
