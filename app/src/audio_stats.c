/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <math.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_stats.h"
#include "audio_telemetry.h"
#include "audio_watchdog.h"
#include "dmic.h"

LOG_MODULE_REGISTER(audio_stats);

/* One report per second of audio. */
#define STATS_WINDOW_SAMPLES DMIC_PCM_RATE

/* Samples at or above this magnitude count as clipped. The PDM decimation
 * filter saturates at full scale, so anything this close to the rail means
 * the gain is too high for the input level.
 */
#define CLIP_LEVEL 32700

void audio_stats_update(const void *buffer, size_t num_samples)
{
	static uint32_t window_samples;
	static uint32_t peak;
	static uint64_t sum_sq;
	static uint32_t clipped;

	const int16_t *samples = buffer;

	for (size_t i = 0; i < num_samples; i++) {
		const int32_t s = samples[i];
		const uint32_t mag = (s < 0) ? -s : s;

		peak = MAX(peak, mag);
		sum_sq += (uint64_t)((int64_t)s * s);
		clipped += (mag >= CLIP_LEVEL);
	}

	window_samples += num_samples;
	if (window_samples < STATS_WINDOW_SAMPLES) {
		return;
	}

	const float rms = sqrtf((float)(sum_sq / window_samples));
	const float peak_db = 20.f * log10f(MAX(peak, 1) / 32768.f);
	const float rms_db = 20.f * log10f(MAX(rms, 1.f) / 32768.f);

#ifdef CONFIG_APP_AUDIO_STATS
	LOG_INF("audio: peak %.1f dBFS, rms %.1f dBFS, clipped %u/%u", (double)peak_db,
		(double)rms_db, clipped, window_samples);
#endif

#ifdef CONFIG_MEMFAULT
	/* Forward levels to Memfault for remote (serial-less) diagnosis. Independent
	 * of APP_AUDIO_STATS so the metrics survive turning the UART logging off. */
	audio_telemetry_levels(peak_db, rms_db, clipped);

	/* Feed the bad-data mic watchdog. This second is "unhealthy" when the mic
	 * is railed (peak at full scale). The original criterion also required the
	 * majority of samples to clip, but a second wedge variant (Toilet #2,
	 * 2026-07-19, ~10 h deaf) rails the peak every second while clipping only
	 * a minority of samples (~-13 dBFS RMS) -- it sailed under the AND and the
	 * reboot never fired. A railed peak alone is safe as the trigger: a real
	 * room rails only on brief transients (clap, door slam), and the watchdog
	 * reboots only after MIC_STUCK_REBOOT_S with not a single clean second --
	 * a bathroom never rails full-scale every second for 5 straight minutes. */
	const bool saturated = (peak >= CLIP_LEVEL);

	audio_watchdog_note_audio_quality(!saturated);
#endif

	window_samples = 0;
	peak = 0;
	sum_sq = 0;
	clipped = 0;
}
