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
#include "cloud.h"

/* Aggregates over the current heartbeat window. Written from the audio thread,
 * read+reset from the Memfault heartbeat callback (different thread). The
 * read-compare-set below is not strictly atomic, but a lost update only skews
 * best-effort telemetry, never corrupts state. */
static atomic_t peak_db_dx10 = ATOMIC_INIT(INT32_MIN);  /* max peak, deci-dBFS */
static atomic_t floor_db_dx10 = ATOMIC_INIT(INT32_MAX); /* min RMS, deci-dBFS */
static atomic_t prob_max_pct = ATOMIC_INIT(0);          /* max probability, % */
/* Mean-probability summary: sum of per-inference prob% and the inference count
 * over the window. Published as ww_prob_mean_pct + ww_inferences so the
 * wake-word confidence distribution (peak via prob_max, centre via the mean,
 * volume via the count) is visible natively in the heartbeat dashboard, not
 * only in the observability CDR. */
static atomic_t prob_sum_pct = ATOMIC_INIT(0);
static atomic_t infer_count = ATOMIC_INIT(0);

/* Near-miss context. The audio-stats module reports levels once per second;
 * keep the last two seconds so a near miss (typically well under a second)
 * can be tagged with the loudest peak around it and the RMS of the second
 * before it (the background the attempt had to beat). Window aggregates:
 * highest near-miss peak probability, and the level/floor of the loudest
 * near miss. */
static atomic_t sec_peak_dx10[2] = { ATOMIC_INIT(INT32_MIN), ATOMIC_INIT(INT32_MIN) };
static atomic_t sec_rms_dx10[2] = { ATOMIC_INIT(INT32_MAX), ATOMIC_INIT(INT32_MAX) };
static atomic_t near_miss_peak_pct = ATOMIC_INIT(0);
static atomic_t near_miss_level_dx10 = ATOMIC_INIT(INT32_MIN);
static atomic_t near_miss_floor_dx10 = ATOMIC_INIT(INT32_MAX);
/* Uptime (ms, 32-bit) of the latest near miss, 0 = none yet. Read from the
 * button GPIO ISR; presses that qualify are staged in miss_button_pending and
 * drained at heartbeat time (memfault_metrics_* is not ISR-safe). */
static atomic_t near_miss_uptime_ms = ATOMIC_INIT(0);
static atomic_t miss_button_pending = ATOMIC_INIT(0);

void audio_telemetry_levels(float peak_db, float rms_db, uint32_t clipped)
{
	const int32_t peak = (int32_t)(peak_db * 10.f);
	const int32_t floor = (int32_t)(rms_db * 10.f);

	/* Shift the 2 s level history: [0] = this second, [1] = the one before. */
	atomic_set(&sec_peak_dx10[1], atomic_get(&sec_peak_dx10[0]));
	atomic_set(&sec_rms_dx10[1], atomic_get(&sec_rms_dx10[0]));
	atomic_set(&sec_peak_dx10[0], peak);
	atomic_set(&sec_rms_dx10[0], floor);

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

	atomic_add(&prob_sum_pct, pct);
	atomic_inc(&infer_count);
}

void audio_telemetry_detection(void)
{
	memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(ww_detections), 1);
}

void audio_telemetry_retry_detection(void)
{
	memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(ww_retry_detections), 1);
}

void audio_telemetry_near_miss(uint32_t peak_pct)
{
	const int32_t level = MAX(atomic_get(&sec_peak_dx10[0]), atomic_get(&sec_peak_dx10[1]));
	const int32_t floor = atomic_get(&sec_rms_dx10[1]);

	memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(ww_near_miss_count), 1);

	if ((int32_t)peak_pct > atomic_get(&near_miss_peak_pct)) {
		atomic_set(&near_miss_peak_pct, (int32_t)peak_pct);
	}
	/* Level and floor follow the loudest near miss of the window, so the pair
	 * describes one attempt rather than mixing two. */
	if (level > atomic_get(&near_miss_level_dx10)) {
		atomic_set(&near_miss_level_dx10, level);
		atomic_set(&near_miss_floor_dx10, floor);
	}

	/* k_uptime_get_32() wraps after 49 days; a wrap makes at most one
	 * button press mis-attribute, which is acceptable for a diagnostic. */
	atomic_set(&near_miss_uptime_ms, MAX(k_uptime_get_32(), 1));
}

void audio_telemetry_button_press(void)
{
	const uint32_t miss = (uint32_t)atomic_get(&near_miss_uptime_ms);

	if (miss != 0 && (k_uptime_get_32() - miss) <= CONFIG_WW_MISS_BUTTON_WINDOW_MS) {
		atomic_inc(&miss_button_pending);
		/* One press per near miss: a second press is a jam, not a miss. */
		atomic_set(&near_miss_uptime_ms, 0);
	}
}

/* Memfault calls this just before serializing each heartbeat. Publish the
 * window's aggregates and reset them for the next window. */
void memfault_metrics_heartbeat_collect_data(void)
{
	/* Preserve the NCS port's own metrics (stack/heap usage, etc.); we took
	 * over this hook from it (CONFIG_MEMFAULT_NCS_IMPLEMENT_METRICS_COLLECTION=n).
	 */
	memfault_ncs_metrics_collect_data();

	/* Wi-Fi driver heap health (wifi_heap_* metrics) — owned by cloud.c. */
	cloud_collect_heap_metrics();

	const int32_t peak = atomic_set(&peak_db_dx10, INT32_MIN);
	const int32_t floor = atomic_set(&floor_db_dx10, INT32_MAX);
	const int32_t prob = atomic_set(&prob_max_pct, 0);
	const int32_t psum = atomic_set(&prob_sum_pct, 0);
	const int32_t pcount = atomic_set(&infer_count, 0);

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

	/* Inference volume this window, and mean wake-word probability across it
	 * (guarded against divide-by-zero when the audio loop ran no inferences). */
	memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(ww_inferences),
						(uint32_t)pcount);
	if (pcount > 0) {
		memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(ww_prob_mean_pct),
							(uint32_t)(psum / pcount));
	}

	/* Near-miss diagnostics: gauges only when a near miss occurred, so an
	 * absent value means "no near miss", not "quiet". */
	const int32_t nm_peak = atomic_set(&near_miss_peak_pct, 0);
	const int32_t nm_level = atomic_set(&near_miss_level_dx10, INT32_MIN);
	const int32_t nm_floor = atomic_set(&near_miss_floor_dx10, INT32_MAX);

	if (nm_peak > 0) {
		memfault_metrics_heartbeat_set_unsigned(MEMFAULT_METRICS_KEY(ww_near_miss_peak_pct),
							(uint32_t)nm_peak);
	}
	if (nm_level != INT32_MIN) {
		memfault_metrics_heartbeat_set_signed(MEMFAULT_METRICS_KEY(ww_near_miss_level_dbfs),
						      nm_level);
	}
	if (nm_floor != INT32_MAX) {
		memfault_metrics_heartbeat_set_signed(MEMFAULT_METRICS_KEY(ww_near_miss_floor_dbfs),
						      nm_floor);
	}
	while (atomic_get(&miss_button_pending) > 0) {
		atomic_dec(&miss_button_pending);
		memfault_metrics_heartbeat_add(MEMFAULT_METRICS_KEY(ww_miss_then_button_count), 1);
	}
}
