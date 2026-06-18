/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <math.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_proc.h"
#include "dmic.h"

LOG_MODULE_REGISTER(audio_proc);

#ifdef CONFIG_APP_AUDIO_HPF
/* 2nd-order Butterworth high-pass, fc = 120 Hz at 16 kHz, normalized
 * coefficients. There is little speech energy below the cutoff, so this
 * strips rumble and handling noise at negligible cost to the model input.
 */
#define HPF_B0 0.967232f
#define HPF_B1 -1.934463f
#define HPF_B2 0.967232f
#define HPF_A1 -1.933388f
#define HPF_A2 0.935528f
#endif /* CONFIG_APP_AUDIO_HPF */

#ifdef CONFIG_APP_AGC
/* Gain bounds and per-10 ms-block adaptation rates. Raising gain uses a
 * ~300 ms time constant so converging gain does not warp the waveform
 * mid-utterance; lowering uses ~50 ms so a suddenly loud source steps away
 * from the saturation cliff quickly. The AGC acts after the PDM hardware
 * gain and cannot undo saturation that happens in the peripheral itself.
 */
#define AGC_GAIN_MIN  0.251f /* -12 dB */
#define AGC_GAIN_MAX  3.981f /* +12 dB */
#define AGC_RATE_UP   0.03f
#define AGC_RATE_DOWN 0.2f
/* Peak envelope release, ~3 s time constant: the gain rides utterance
 * peaks, not the silence between them.
 */
#define AGC_RELEASE 0.99667f
/* Hold the gain when the envelope is below this peak level (-38 dBFS):
 * that is room noise, not speech, and amplifying it toward the target
 * would just pump the noise floor.
 */
#define AGC_GATE 0.0126f

static float agc_env;
static float agc_gain = 1.f;

static void agc_update(float block_peak)
{
	agc_env = MAX(block_peak, agc_env * AGC_RELEASE);

#ifdef CONFIG_APP_AUDIO_STATS
	/* Per-second tuning telemetry, alongside the audio_stats report. */
	static uint32_t blocks;

	if (++blocks >= 100) {
		LOG_INF("agc: gain %+.1f dB (env %.1f dBFS)",
			(double)(20.f * log10f(agc_gain)),
			(double)(20.f * log10f(MAX(agc_env, 1e-5f))));
		blocks = 0;
	}
#endif /* CONFIG_APP_AUDIO_STATS */

	if (agc_env < AGC_GATE) {
		return;
	}

	static float target;

	if (target == 0.f) {
		target = powf(10.f, CONFIG_APP_AGC_TARGET_DBFS / 20.f);
	}

	const float wanted = CLAMP(target / agc_env, AGC_GAIN_MIN, AGC_GAIN_MAX);
	const float rate = (wanted < agc_gain) ? AGC_RATE_DOWN : AGC_RATE_UP;

	agc_gain += (wanted - agc_gain) * rate;
}
#endif /* CONFIG_APP_AGC */

void audio_proc_run(void *buffer, size_t num_samples)
{
	int16_t *samples = buffer;
#ifdef CONFIG_APP_AUDIO_HPF
	static float z1, z2;
#endif
#ifdef CONFIG_APP_AGC
	float block_peak = 0.f;
#endif

	for (size_t i = 0; i < num_samples; i++) {
		float y = samples[i];

#ifdef CONFIG_APP_AUDIO_HPF
		const float x = y;

		y = HPF_B0 * x + z1;
		z1 = HPF_B1 * x - HPF_A1 * y + z2;
		z2 = HPF_B2 * x - HPF_A2 * y;
#endif
#ifdef CONFIG_APP_AGC
		block_peak = MAX(block_peak, fabsf(y));
		y *= agc_gain;
#endif
		samples[i] = (int16_t)CLAMP(y, -32768.f, 32767.f);
	}

#ifdef CONFIG_APP_AGC
	agc_update(block_peak / 32768.f);
#endif
}
