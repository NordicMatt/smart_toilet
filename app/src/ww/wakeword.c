/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/logging/log.h>
#include <nrf_edgeai/nrf_edgeai.h>
#include <nrf_edgeai/rt/nrf_edgeai_runtime_aux.h>
#if defined(CONFIG_NRF_EDGEAI_OBSV)
#include <nrf_edgeai_obsv/nrf_edgeai_obsv.h>
#if defined(CONFIG_NRF_EDGEAI_OBSV_MEMFAULT)
#include <nrf_edgeai_obsv/nrf_edgeai_obsv_memfault.h>
#endif
#endif

#include "../audio_telemetry.h"
#include "../dmic.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model.h"
#include "wakeword.h"

LOG_MODULE_REGISTER(ww);

static nrf_edgeai_t *ww_model;

#if defined(CONFIG_NRF_EDGEAI_OBSV)
/* The abracadabra/okay_nordic wake-word models are single-output (one
 * probability = P(wakeword)); see MODEL_OUTPUTS_NUM in the generated model. The
 * observability storage is sized for this at compile time and cross-checked
 * against the model's runtime num_classes in ww_obsv_init().
 */
#define WW_OBSV_NUM_CLASSES 1

/* On-wire model identity carried in every snapshot, so the cloud can tell which
 * model produced a given probability distribution (A/B across models). This is
 * a compact app-defined id (the obsv model_id field is uint16_t, so the 5-6
 * digit Edge AI Lab solution numbers do not fit): 1 = "abracadabra" (solution
 * 93800), 2 = "Okay Nordic" (solution 36455). Keep these stable. */
#if defined(CONFIG_APP_WW_MODEL_OKAY_NORDIC)
#define WW_OBSV_MODEL_ID 2U
#else
#define WW_OBSV_MODEL_ID 1U
#endif

static nrf_edgeai_obsv_ctx_t ww_obsv_ctx;
static nrf_edgeai_obsv_metric_t ww_obsv_pd_metric;
/* uint32_t array gives the natural alignment the storage macro requires. */
static uint32_t ww_obsv_pd_buf[(NRF_EDGEAI_OBSV_PD_STORAGE_BYTES(WW_OBSV_NUM_CLASSES)
				+ sizeof(uint32_t) - 1) / sizeof(uint32_t)];

/* Set up the probability-distribution metric and bind the Memfault CDR
 * transport. Called from ww_init() once the model is initialized (num_classes
 * is only valid after nrf_edgeai_init()). Failures are non-fatal: observability
 * is diagnostics, so a problem here must never stop the toilet flushing.
 */
static void ww_obsv_init(void)
{
	const uint16_t num_classes = ww_model->decoded_output.classif.num_classes;

	__ASSERT_NO_MSG(num_classes == WW_OBSV_NUM_CLASSES);

	const nrf_edgeai_obsv_model_info_t model = {
		.model_id = WW_OBSV_MODEL_ID,
		.num_classes = num_classes,
		.version = 1,
	};

	int err = nrf_edgeai_obsv_init(&ww_obsv_ctx, &model);

	if (err) {
		LOG_WRN("obsv init failed (err %d); ML metrics disabled", err);
		return;
	}

	nrf_edgeai_obsv_metric_pd_create(&ww_obsv_pd_metric, ww_obsv_pd_buf, num_classes);

	err = nrf_edgeai_obsv_register(&ww_obsv_ctx, &ww_obsv_pd_metric, NULL);
	if (err) {
		LOG_WRN("obsv metric register failed (err %d)", err);
		return;
	}

#if defined(CONFIG_NRF_EDGEAI_OBSV_MEMFAULT)
	err = nrf_edgeai_obsv_memfault_init(&ww_obsv_ctx);
	if (err) {
		LOG_WRN("obsv Memfault CDR bind failed (err %d)", err);
		return;
	}
#endif

	LOG_INF("obsv: probability-distribution metric -> Memfault CDR (model %u)",
		WW_OBSV_MODEL_ID);
}
#endif /* CONFIG_NRF_EDGEAI_OBSV */

int ww_init(void)
{
#ifdef CONFIG_APP_WW_MODEL_OKAY_NORDIC
	/* The add-on's bundled "Okay Nordic" reference wake word model. */
	ww_model = nrf_edgeai_user_model_wakeword();
#else
	/* "Abracadabra" wake word model (Edge AI Lab solution 93800). */
	ww_model = nrf_edgeai_user_model_93800();
#endif
	__ASSERT_NO_MSG(ww_model);
	__ASSERT_NO_MSG(ww_model->input.window_size == DMIC_SAMPLES_IN_BLOCK);

	nrf_edgeai_err_t err = nrf_edgeai_init(ww_model);

	if (err) {
		LOG_ERR("Model initialization failed (err %d)", err);
		return -ENOENT;
	}

#if defined(CONFIG_NRF_EDGEAI_OBSV)
	ww_obsv_init();
#endif

	return 0;
}

/* After a detection, ignore further hits for this many inference frames.
 * Inference only completes every third 10 ms block (the model buffers the
 * first two), so one frame is ~30 ms and 33 frames is ~1 s. A wake-word
 * utterance outlives the voting window, so without a refractory period a
 * single utterance fires several times; a tail re-fire that slips through
 * is absorbed by the actuator lockout.
 */
#define WW_REFRACTORY_FRAMES 33

static bool ww_postprocess(void)
{
	static uint32_t ww_count;
	static uint32_t ww_history;
	static uint32_t refractory;

	const float ww_threshold = CONFIG_WW_PROBABILITY_THRESHOLD / 1000.f;

	const uint16_t predicted_class = ww_model->decoded_output.classif.predicted_class;
	const float class_probability =
		ww_model->decoded_output.classif.probabilities.p_f32[predicted_class];
	const bool ww_detected = class_probability > ww_threshold;

	/* Remote diagnosis: track the peak probability per Memfault heartbeat. */
	audio_telemetry_prob(class_probability);

#if defined(CONFIG_NRF_EDGEAI_OBSV)
	/* Feed the full class-probability vector to the observability metrics.
	 * Thread-safe (takes ctx->lock), so it cannot race the auto-collect
	 * encode running on the system workqueue. Bins the wake-word confidence
	 * into the per-class histogram shipped to Memfault as a CDR. */
	nrf_edgeai_obsv_update_probs(&ww_obsv_ctx,
				     ww_model->decoded_output.classif.probabilities.p_f32);
#endif

	const bool oldest_entry = (bool)(ww_history & BIT(CONFIG_WW_HISTORY_SIZE - 1));

	ww_count = ww_count + ww_detected - oldest_entry;
	ww_history = (ww_history << 1) | ww_detected;

	LOG_DBG("postprocess: count: %2u, probability: %f", ww_count, (double)class_probability);

#ifdef CONFIG_APP_AUDIO_STATS
	/* Per-second tuning telemetry: how close the detector got. Inference
	 * completes every third 10 ms block, so 33 frames make one report.
	 */
	static uint32_t stat_frames;
	static float stat_max_prob;
	static uint32_t stat_max_count;

	stat_max_prob = MAX(stat_max_prob, class_probability);
	stat_max_count = MAX(stat_max_count, ww_count);

	if (++stat_frames >= 33) {
		LOG_INF("ww: peak prob %.2f (bar %.2f), peak votes %u/%d", (double)stat_max_prob,
			(double)ww_threshold, stat_max_count, CONFIG_WW_COUNT_THRESHOLD);
		stat_frames = 0;
		stat_max_prob = 0.f;
		stat_max_count = 0;
	}
#endif /* CONFIG_APP_AUDIO_STATS */

	if (refractory > 0) {
		refractory--;
		if (refractory == 0) {
			/* Votes accumulated while suppressed would otherwise
			 * half-count toward the next fire and decay before the
			 * check resumes; start the next utterance fresh.
			 */
			ww_count = 0;
			ww_history = 0;
		}
		return false;
	}

	if (ww_count >= CONFIG_WW_COUNT_THRESHOLD) {
		ww_count = 0;
		ww_history = 0;
		refractory = WW_REFRACTORY_FRAMES;

		return true;
	}

	return false;
}

int ww_process(uint8_t *const audio_buffer, const uint16_t num_samples, bool *const ww_detected)
{
	__ASSERT_NO_MSG(audio_buffer);
	__ASSERT_NO_MSG(num_samples == nrf_edgeai_input_window_size(ww_model));
	__ASSERT_NO_MSG(ww_detected);

	nrf_edgeai_err_t err;

	err = nrf_edgeai_feed_inputs(ww_model, audio_buffer, num_samples);
	free_dmic_buffer(audio_buffer);

	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		/* Skip inference, not enough data. */
		return -EBUSY;
	} else if (err) {
		LOG_ERR("Failed to feed inputs (err %d)", err);
		return -EPERM;
	}

	err = nrf_edgeai_run_inference(ww_model);
	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		/* Skip output extraction, not enough data. */
		return -EBUSY;
	} else if (err) {
		LOG_ERR("Failed to run inference (err %d)", err);
		return -EPERM;
	}

	*ww_detected = ww_postprocess();

	return 0;
}

void ww_reset(void)
{
	nrf_edgeai_model_axon_init_persistent_vars(ww_model);
}
