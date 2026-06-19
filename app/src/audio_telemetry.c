/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Audio/wake-word telemetry to Memfault heartbeat metrics. Gauges (peak level,
 * noise floor, max wake-word probability) are aggregated over the heartbeat
 * window and pushed in memfault_metrics_heartbeat_collect_data(), which runs
 * just before each heartbeat is serialized. Counters (clipped samples,
 * detections) are added as events occur; Memfault resets them each heartbeat.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <memfault/metrics/metrics.h>
#include <memfault_ncs.h>

#include "audio_telemetry.h"

/* Aggregates over the current heartbeat window. Written from the audio thread,
 * read+reset from the Memfault heartbeat callback (different thread). The
 * read-compare-set below is not strictly atomic, but a lost update only skews
 * best-effort telemetry, never corrupts state. */
static atomic_t peak_db_dx10 = ATOMIC_INIT(INT32_MIN);  /* max peak, deci-dBFS */
static atomic_t floor_db_dx10 = ATOMIC_INIT(INT32_MAX); /* min RMS, deci-dBFS */
static atomic_t prob_max_pct = ATOMIC_INIT(0);          /* max probability, % */

void audio_telemetry_levels(float peak_db, float rms_db, uint32_t clipped)
{
	const int32_t peak = (int32_t)(peak_db * 10.f);
	const int32_t floor = (int32_t)(rms_db * 10.f);

	if (peak > atomic_get(&peak_db_dx10)) {
		atomic_set(&peak_db_dx10, peak);
	}
	if (floor < atomic_get(&floor_db_dx10)) {
		atomic_set(&floor_db_dx10, floor);
	}
	if (clipped) {
		memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(audio_clip_count),
					       (int32_t)clipped);
	}
}

void audio_telemetry_prob(float prob)
{
	const int32_t pct = (int32_t)(prob * 100.f);

	if (pct > atomic_get(&prob_max_pct)) {
		atomic_set(&prob_max_pct, pct);
	}
}

void audio_telemetry_detection(void)
{
	memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(ww_detections), 1);
}

/* Memfault calls this just before serializing each heartbeat. Publish the
 * window's aggregates and reset them for the next window. */
void memfault_metrics_heartbeat_collect_data(void)
{
	/* Preserve the NCS port's own metrics (stack/heap usage, etc.); we took
	 * over this hook from it (CONFIG_MEMFAULT_NCS_IMPLEMENT_METRICS_COLLECTION=n).
	 */
	memfault_ncs_metrics_collect_data();

	const int32_t peak = atomic_set(&peak_db_dx10, INT32_MIN);
	const int32_t floor = atomic_set(&floor_db_dx10, INT32_MAX);
	const int32_t prob = atomic_set(&prob_max_pct, 0);

	/* Only publish levels if any audio was processed this window. */
	if (peak != INT32_MIN) {
		memfault_metrics_heartbeat_set_signed(MEMFAULT_METRICS_KEY(audio_peak_dbfs),
						      peak);
	}
	if (floor != INT32_MAX) {
		memfault_metrics_heartbeat_set_signed(MEMFAULT_METRICS_KEY(audio_noise_floor_dbfs),
						      floor);
	}
	memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(ww_prob_max_pct),
						(uint32_t)prob);
}
