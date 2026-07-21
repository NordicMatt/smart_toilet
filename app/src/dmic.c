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

/* 16 blocks = ~140 ms of stall tolerance (v2.0.17). With the old 4-block
 * minimum -- 2 permanently held by EasyDMA double-buffering, 1 held by the app
 * between read and free -- a single block of margin meant any >~20 ms consumer
 * delay overran the ring and halted capture (Toilet #2, 2026-07-21: coop-thread
 * Wi-Fi work is the prime suspect, since even the priority-0 audio thread
 * cannot preempt cooperative threads). Spare blocks let the loop absorb such
 * bursts and catch up by draining the queue in real time.
 *
 * HISTORY: a bigger slab was tried in v2.0.11 and backfired -- a wedged
 * restart "succeeded" into a saturated free-running mode the model happily
 * consumed, feeding the liveness watchdog while the device sat PERMANENTLY
 * DEAF (both toilets, 2026-07-14), so v2.0.12 pinned the slab to the minimum
 * to force restart failures into watchdog reboots. That trade-off is obsolete:
 * the railed-peak detector (audio_stats.c, v2.0.16) now catches any
 * "flowing but garbage" mic within MIC_STUCK_REBOOT_S and reboots, so spare
 * capacity no longer risks silent deafness.
 *
 * Defined manually (not K_MEM_SLAB_DEFINE_STATIC) so dmic_restart() can
 * k_mem_slab_init() the slab again to forcibly reclaim blocks leaked by the
 * driver's halt/error paths -- the reason the v2.0.16-era in-place restart
 * always failed with -12 (see dmic_restart below). */
#define DMIC_SLAB_BLOCKS 16
static char __aligned(4) dmic_slab_buf[BLOCK_SIZE * DMIC_SLAB_BLOCKS];
static struct k_mem_slab dmic_mem_slab;

/* Configure the PDM stream and re-apply the gain boost. Shared by dmic_init()
 * and the full re-init path in dmic_restart(); dmic_configure() resets the
 * PDM gain to the 0 dB default, so the boost must always follow it.
 */
static int dmic_apply_config(const struct device *dmic_dev)
{
	int err;

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

	nrf_pdm_gain_set((NRF_PDM_Type *)DT_REG_ADDR(DT_NODELABEL(dmic_dev)),
			 DMIC_PDM_GAIN, DMIC_PDM_GAIN);

	return 0;
}

int dmic_init(void)
{
	int err;
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Device is not ready");
		return -ENODEV;
	}

	err = k_mem_slab_init(&dmic_mem_slab, dmic_slab_buf, BLOCK_SIZE, DMIC_SLAB_BLOCKS);
	if (err) {
		LOG_ERR("Failed to init mem slab (err %d)", err);
		return err;
	}

	err = dmic_apply_config(dmic_dev);
	if (err < 0) {
		return err;
	}

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
	void *buffer;
	size_t size;
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	/* A consumer stall (e.g. unpreemptible cooperative-thread Wi-Fi work) can
	 * overrun the PDM ring; the nrfx PDM driver then halts capture and every
	 * dmic_read() returns -EAGAIN until capture is retriggered -- reads alone
	 * never recover it. Field signature: a wall of "DMIC read error -11"
	 * (coredumps 2026-07-11 issue 1805218207, 2026-07-21 issue 1805234264).
	 *
	 * A bare STOP/START cannot recover it either: blocks parked in the
	 * driver's rx_queue and blocks leaked by its halt/error paths stay
	 * allocated, so the restarted capture immediately dies again on buffer
	 * alloc (-12; the 2026-07-21 coredump log tail shows exactly this).
	 * Recover in-place with the same sequence a reboot performs, minus the
	 * reboot: stop, let the driver's stop event release its DMA buffers,
	 * drain whatever is still queued, forcibly reclaim the whole slab, then
	 * reconfigure from scratch and start.
	 *
	 * STOP may legitimately fail if the driver already stopped -- ignore it.
	 */
	(void)dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	/* Let the driver's stop event run (frees its in-flight DMA buffers). */
	k_sleep(K_MSEC(20));

	/* Drain delivered-but-unread blocks. Bounded: the queue cannot hold more
	 * than the slab has blocks; anything beyond that means the driver is
	 * still producing, and reconfigure below handles it.
	 */
	for (int i = 0; i < DMIC_SLAB_BLOCKS; i++) {
		if (dmic_read(dmic_dev, 0, &buffer, &size, 0) < 0) {
			break;
		}
		free_dmic_buffer(buffer);
	}

	/* Reclaim every block, including any leaked by the driver's error paths.
	 * Safe here: capture is stopped and the stop event has settled, so no
	 * DMA is outstanding. Worst case this path still fails to revive the
	 * mic -- then the audio watchdog reboots, exactly as before.
	 */
	err = k_mem_slab_init(&dmic_mem_slab, dmic_slab_buf, BLOCK_SIZE, DMIC_SLAB_BLOCKS);
	if (err) {
		LOG_ERR("DMIC slab re-init failed (err %d)", err);
		return err;
	}

	err = dmic_apply_config(dmic_dev);
	if (err < 0) {
		return err;
	}

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err < 0) {
		LOG_ERR("DMIC restart failed (err %d)", err);
		return err;
	}

	LOG_WRN("DMIC capture fully re-initialised after persistent read failures");
	return 0;
}
