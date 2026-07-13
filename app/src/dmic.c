/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_pdm.h>

#include "dmic.h"

LOG_MODULE_REGISTER(dmic);

#define BLOCK_SIZE (DMIC_SAMPLE_BYTES * DMIC_SAMPLES_IN_BLOCK)

/* PDM digital gain to boost mic sensitivity. Register units are 0.5 dB/step:
 * 0x28 = 0 dB (default), 0x50 = +20 dB (max). The Zephyr nrfx PDM driver resets
 * gain to the 0 dB default during dmic_configure(), so it must be re-applied
 * afterwards (see dmic_init). Tune via CONFIG_APP_PDM_GAIN_DB.
 */
#define DMIC_PDM_GAIN (NRF_PDM_GAIN_DEFAULT + (CONFIG_APP_PDM_GAIN_DB) * 2)

/* 8 blocks, not the driver's minimum of 4: each PDM halt/restart cycle can
 * strand the up-to-2 in-flight blocks inside the driver (they sit in its
 * internal queue until a later stop event releases them), and with only 4
 * blocks two failed cycles left the slab empty -- every restart then died in
 * the driver with "Failed to allocate buffer: -12" and the mic never came
 * back (field wedge of 2026-07-13, issue 1805217631). The extra 4 blocks
 * (~2.6 KB) buy several recovery attempts between reboots. */
K_MEM_SLAB_DEFINE_STATIC(dmic_mem_slab, BLOCK_SIZE, 8, 4);

int dmic_init(void)
{
	int err;
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Device is not ready");
		return -ENODEV;
	}

	struct pcm_stream_cfg stream = {
		.pcm_rate = DMIC_PCM_RATE,
		.pcm_width = DMIC_SAMPLE_BYTES * 8,
		.block_size = BLOCK_SIZE,
		.mem_slab = &dmic_mem_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
			.req_chan_map_hi = 0,
			.req_num_chan = 1,
			.req_num_streams = 1,
		},
	};

	err = dmic_configure(dmic_dev, &cfg);
	if (err < 0) {
		LOG_ERR("Failed to configure (err %d)", err);
		return err;
	}

	/* Boost PDM gain above the driver's 0 dB default for more mic
	 * sensitivity. Must run after dmic_configure(), which resets the gain.
	 */
	nrf_pdm_gain_set((NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(dmic_dev)),
			 DMIC_PDM_GAIN, DMIC_PDM_GAIN);
	LOG_INF("PDM gain set to %+d dB (reg 0x%02x)", CONFIG_APP_PDM_GAIN_DB, DMIC_PDM_GAIN);

	return 0;
}

void free_dmic_buffer(void *buffer)
{
	k_mem_slab_free(&dmic_mem_slab, buffer);
}

int dmic_restart(void)
{
	int err;
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	/* A CPU stall (e.g. Wi-Fi scan/reconnect work at higher priority) can
	 * overrun the PDM ring; the nrfx PDM driver then halts capture and every
	 * dmic_read() returns -EAGAIN until capture is retriggered -- reads alone
	 * never recover it. Field signature: a wall of "DMIC read error -11" with
	 * a wpa_supp scan timeout amid it (coredump 2026-07-11, issue 1805218207).
	 * STOP may legitimately fail if the driver already stopped -- ignore it.
	 */
	(void)dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	/* Reclaim any blocks the halted session delivered but nobody read: they
	 * sit in the driver's RX queue holding slab blocks, and the restarted
	 * capture needs those blocks back (the wedge that halts capture also
	 * strands up to 2 more in the driver's internal queue, which the app
	 * cannot reach -- the slab is oversized to absorb those; see its
	 * definition above). Non-blocking reads until the queue reports empty.
	 */
	void *stale;
	size_t stale_size;

	while (dmic_read(dmic_dev, 0, &stale, &stale_size, 0) == 0) {
		free_dmic_buffer(stale);
	}

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err < 0) {
		LOG_ERR("DMIC restart failed (err %d)", err);
		return err;
	}

	/* Defensive: re-apply the PDM gain in case the driver reset it on the
	 * stop/start cycle (it does so on configure; cheap and idempotent here).
	 */
	nrf_pdm_gain_set((NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(dmic_dev)),
			 DMIC_PDM_GAIN, DMIC_PDM_GAIN);

	LOG_WRN("DMIC capture restarted after persistent read failures");
	return 0;
}
