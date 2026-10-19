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
#include <nrf_edgeai_obsv/nrf_edgeai_obsv_metrics.h>
#if defined(CONFIG_NRF_EDGEAI_OBSV_MEMFAULT)
#include <nrf_edgeai_obsv/nrf_edgeai_obsv_memfault.h>
#endif
#endif

#include "../audio_telemetry.h"
#include "../dmic.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model.h"
#include "nrf_edgeai_generated/nrf_edgeai_user_model_labels.h"
#include "wakeword.h"

LOG_MODULE_REGISTER(ww);

static nrf_edgeai_t *ww_model;

/* The abracadabra model (Edge AI Lab solution 96848, add-on v3.0.0) is a
 * 3-class keyword-spotting export: OTHER, SILENCE, abracadabra. The detector
 * votes on P(abracadabra), the wake-word class's own probability, not on the
 * argmax class. (The v2.x export was single-output, P(wakeword) only.)
 */
#define WW_NUM_CLASSES MODEL_USER_LABEL_COUNT
#define WW_CLASS MODEL_USER_LABEL_ABRACADABRA

#if defined(CONFIG_NRF_EDGEAI_OBSV)
/* The observability metrics see the model's real 3-class probability vector,
 * so the class-predictions streak rows and the transition matrix follow the
 * model's own argmax (v2.x fed a synthetic { threshold, p } pair instead).
 * tools/decode_edgeai_cdr.py reads row WW_CLASS as the wake-word row.
 */
BUILD_ASSERT(CONFIG_NRF_EDGEAI_OBSV_MAX_CLASSES >= WW_NUM_CLASSES,
	     "CONFIG_NRF_EDGEAI_OBSV_MAX_CLASSES is below the model class count");

/* Upper bound on the mel vector length fed to the input-side metrics (the
 * abracadabra model extracts 40 mel bins per frame). */
#define WW_OBSV_MEL_MAX 64

/* On-wire model identity carried in every snapshot, so the cloud can tell which
 * model produced a given distribution (A/B across models). This is a compact
 * app-defined id: the obsv model_id field is uint16_t and the 5-digit Edge AI
 * Lab solution numbers do not fit. 1 = "abracadabra" wake word (solution
 * 93800), 2 = "Okay Nordic" (solution 36455), 3 = "abracadabra" 3-class KWS
 * (solution 96848). Keep these stable. */
#define WW_OBSV_MODEL_ID 3U

#define WW_OBSV_WORDS(bytes) DIV_ROUND_UP(bytes, sizeof(uint32_t))

static nrf_edgeai_obsv_ctx_t ww_obsv_ctx;

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_CLASS_PRED_DIST)
/* Class predictions distribution: per-class probability histogram plus
 * per-class streak-length histogram (merged v2.x probability-distribution and
 * class-streak metrics). uint32_t arrays give the alignment the storage
 * macros require. */
static nrf_edgeai_obsv_metric_t ww_obsv_cpd_metric;
static uint32_t ww_obsv_cpd_buf[WW_OBSV_WORDS(NRF_EDGEAI_OBSV_CPD_STORAGE_BYTES(WW_NUM_CLASSES))];
#endif

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_TRANSITION_MATRIX)
/* Class-to-class transitions between consecutive inferences, e.g. how often
 * the model goes OTHER -> abracadabra (speech mistaken for the word) versus
 * SILENCE -> abracadabra. */
static nrf_edgeai_obsv_metric_t ww_obsv_tm_metric;
static uint32_t ww_obsv_tm_buf[WW_OBSV_WORDS(NRF_EDGEAI_OBSV_TM_STORAGE_BYTES(WW_NUM_CLASSES))];
#endif

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_MEL_SPECTRAL_DESC)
/* Mel spectral descriptor: eight scale-invariant shape statistics of each
 * frame's 40-bin mel vector (band ratios, centroid, spread, entropy, flatness,
 * contrast), histogrammed. Input-side observability: shows whether what the
 * model hears looks like speech, clipped audio, or far-field mush, which the
 * output-side histograms cannot tell apart. Storage is independent of the
 * feature count. */
static nrf_edgeai_obsv_metric_t ww_obsv_msd_metric;
static uint32_t ww_obsv_msd_buf[WW_OBSV_WORDS(NRF_EDGEAI_OBSV_MSD_STORAGE_BYTES(WW_OBSV_MEL_MAX))];
#endif

/* Create and register one metric. Failures are logged by the caller. */
static int ww_obsv_add(nrf_edgeai_obsv_metric_t *metric, const char *name)
{
	int err = nrf_edgeai_obsv_register(&ww_obsv_ctx, metric, NULL);

	if (err) {
		LOG_WRN("obsv %s metric register failed (err %d)", name, err);
	}

	return err;
}

/* Set up the observability metrics and bind the Memfault CDR transport.
 * Called from ww_init() once the model is initialized (num_classes is only
 * valid after nrf_edgeai_init()). Failures are non-fatal: observability is
 * diagnostics, so a problem here must never stop the toilet flushing. The
 * CDR payload follows the registration order.
 */
static void ww_obsv_init(void)
{
	const nrf_edgeai_dsp_feature_extraction_t *fx = nrf_edgeai_dsp_features_ctx(ww_model);

	__ASSERT_NO_MSG(nrf_edgeai_model_outputs_num(ww_model) == WW_NUM_CLASSES);

	/* version 3: the model's real 3-class probability vector. */
	const nrf_edgeai_obsv_model_info_t model = {
		.model_id = WW_OBSV_MODEL_ID,
		.num_classes = WW_NUM_CLASSES,
		.num_features = fx != NULL ? fx->overall_num : 0,
		.version = 3,
	};

	int err = nrf_edgeai_obsv_init(&ww_obsv_ctx, &model);

	if (err) {
		LOG_WRN("obsv init failed (err %d); ML metrics disabled", err);
		return;
	}

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_CLASS_PRED_DIST)
	nrf_edgeai_obsv_metric_cpd_create(&ww_obsv_cpd_metric, ww_obsv_cpd_buf, WW_NUM_CLASSES);
	if (ww_obsv_add(&ww_obsv_cpd_metric, "class-pred")) {
		return;
	}
#endif

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_TRANSITION_MATRIX)
	nrf_edgeai_obsv_metric_tm_create(&ww_obsv_tm_metric, ww_obsv_tm_buf, WW_NUM_CLASSES);
	if (ww_obsv_add(&ww_obsv_tm_metric, "transition")) {
		return;
	}
#endif

#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_MEL_SPECTRAL_DESC)
	if (fx != NULL && fx->overall_num <= WW_OBSV_MEL_MAX) {
		nrf_edgeai_obsv_metric_msd_create(&ww_obsv_msd_metric, ww_obsv_msd_buf,
						  fx->overall_num);
		if (ww_obsv_add(&ww_obsv_msd_metric, "mel")) {
			return;
		}
	}
#endif

#if defined(CONFIG_NRF_EDGEAI_OBSV_MEMFAULT)
	err = nrf_edgeai_obsv_memfault_init(&ww_obsv_ctx);
	if (err) {
		LOG_WRN("obsv Memfault CDR bind failed (err %d)", err);
		return;
	}
#endif

	LOG_INF("obsv: %u-class metrics -> Memfault CDR (model %u)", WW_NUM_CLASSES,
		WW_OBSV_MODEL_ID);
}
#endif /* CONFIG_NRF_EDGEAI_OBSV */

int ww_init(void)
{
	/* "Abracadabra" 3-class KWS model (Edge AI Lab solution 96848). */
	ww_model = nrf_edgeai_user_model_96848();
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

/* Retry assist window in inference frames (~30 ms each). */
#define WW_RETRY_FRAMES (CONFIG_WW_RETRY_WINDOW_MS / 30)

/* Near-miss tracking and retry assist (2.3.7).
 *
 * Field data (CDR exports 5-7): a real utterance drives the model to 0.98-0.99
 * for 7+ frames; a failed attempt leaves a handful of frames at 0.125-0.5 and
 * never approaches the bar. Those attempts are invisible to the vote and to
 * the per-heartbeat peak. This tracks them as "near-miss runs": frames at or
 * above CONFIG_WW_NEAR_MISS_FLOOR, ended by CONFIG_WW_NEAR_MISS_GAP_FRAMES
 * consecutive frames below it. A run that ends without a detection is
 * reported (peak probability + surrounding audio level, see audio_telemetry)
 * and arms the retry assist: for CONFIG_WW_RETRY_WINDOW_MS the per-frame bar
 * drops to CONFIG_WW_RETRY_THRESHOLD so the user's repeat is accepted. The
 * assist arms only when the run ENDS, so it never lowers the bar for the
 * utterance that triggered it (that would just be a lower bar).
 *
 * Returns true if a near-miss run just ended.
 */
static bool ww_track_near_miss(float p, bool fired, bool suppressed)
{
	static bool in_run;
	static float run_peak;
	static uint32_t below_frames;

	const float floor = CONFIG_WW_NEAR_MISS_FLOOR / 1000.f;

	if (fired || suppressed) {
		/* A detection (or its refractory tail) is a hit, not a miss. */
		in_run = false;
		run_peak = 0.f;
		below_frames = 0;
		return false;
	}

	if (p >= floor) {
		in_run = true;
		run_peak = MAX(run_peak, p);
		below_frames = 0;
		return false;
	}

	if (!in_run) {
		return false;
	}

	if (++below_frames < CONFIG_WW_NEAR_MISS_GAP_FRAMES) {
		return false;
	}

	/* Run over with no detection. */
	const uint32_t peak_pct = (uint32_t)(run_peak * 100.f);

	in_run = false;
	run_peak = 0.f;
	below_frames = 0;

	LOG_INF("ww: near miss, peak %u%%", peak_pct);
	audio_telemetry_near_miss(peak_pct);

	return true;
}

#ifdef CONFIG_WW_TRACE
/* Live-tuning trace: one log line per burst of wake-word activity. A burst
 * starts at the first frame with P(wake word) >= 10 % and ends after 15
 * frames below it. Each frame prints as <P%><class>, class o/s/a = argmax
 * OTHER/SILENCE/abracadabra.
 */
#define WW_TRACE_MAX 64
static bool trace_fired;

static void ww_trace_frame(const float *probs, uint32_t votes)
{
	static char line[WW_TRACE_MAX * 4 + 1];
	static uint32_t len, frames, quiet, peak_votes;
	const float p = probs[WW_CLASS];
	uint32_t top = 0;

	for (uint32_t i = 1; i < WW_NUM_CLASSES; i++) {
		if (probs[i] > probs[top]) {
			top = i;
		}
	}

	if (frames == 0 && p < 0.10f) {
		return;
	}

	quiet = (p < 0.10f) ? quiet + 1 : 0;
	peak_votes = MAX(peak_votes, votes);

	if (frames < WW_TRACE_MAX && quiet == 0) {
		len += snprintk(&line[len], sizeof(line) - len, "%u%c ",
				(unsigned int)(p * 100.f),
				top == WW_CLASS ? 'a' : (top == MODEL_USER_LABEL_SILENCE ? 's' : 'o'));
	}
	frames++;

	if (quiet >= 15 || frames >= WW_TRACE_MAX + 15) {
		LOG_INF("trace %s votes %u/%d: %s", trace_fired ? "FIRED" : "miss",
			peak_votes, CONFIG_WW_COUNT_THRESHOLD, line);
		len = frames = quiet = peak_votes = 0;
		line[0] = '\0';
		trace_fired = false;
	}
}
#endif /* CONFIG_WW_TRACE */

static bool ww_postprocess(void)
{
	static uint32_t ww_count;
	static uint32_t ww_history;
	static uint32_t refractory;
	static uint32_t retry_frames;

	const float ww_threshold = CONFIG_WW_PROBABILITY_THRESHOLD / 1000.f;
	const float retry_threshold = CONFIG_WW_RETRY_THRESHOLD / 1000.f;
	const bool retry_armed = retry_frames > 0;

	if (retry_armed) {
		retry_frames--;
	}

	const float *probs = ww_model->decoded_output.classif.probabilities.p_f32;
	/* Vote on the wake-word class's own probability, whatever the argmax. */
	const float class_probability = probs[WW_CLASS];
	/* The retry bar applies for CONFIG_WW_RETRY_WINDOW_MS after a near miss. */
	const bool ww_detected =
		class_probability > (retry_armed ? retry_threshold : ww_threshold);

	/* Remote diagnosis: track the peak probability per Memfault heartbeat. */
	audio_telemetry_prob(class_probability);

#if defined(CONFIG_NRF_EDGEAI_OBSV)
	/* Feed the full class-probability vector to the observability metrics.
	 * Thread-safe (takes ctx->lock), so it cannot race the auto-collect
	 * encode running on the system workqueue. */
#if defined(CONFIG_NRF_EDGEAI_OBSV_METRIC_MEL_SPECTRAL_DESC)
	/* Feed the frame's mel vector first: feature updates do not advance the
	 * inference counter, the probability update below does. The DSP front
	 * end leaves float mel values in the feature buffer (read as p_f32, as
	 * the add-on's ww_kws reference does; v2.x read it as int16, which
	 * garbled the vector). Lift by the frame's minimum so the vector is
	 * non-negative whether the mel values are linear power (min ~0, no-op)
	 * or log-scaled (becomes "dB above the frame floor"). The descriptor's
	 * shape statistics are ratios and index-weighted moments, which stay
	 * meaningful in both cases; it clamps negatives to zero, which would
	 * garble a log-mel frame. */
	{
		const nrf_edgeai_dsp_feature_extraction_t *fx = nrf_edgeai_dsp_features_ctx(ww_model);

		if (fx != NULL && fx->overall_num <= WW_OBSV_MEL_MAX) {
			const uint16_t n = fx->overall_num;
			const float *mel = fx->buffer.p_f32;
			float feats[WW_OBSV_MEL_MAX];
			float lo = mel[0];

			for (uint16_t i = 1; i < n; i++) {
				lo = MIN(lo, mel[i]);
			}
			for (uint16_t i = 0; i < n; i++) {
				feats[i] = mel[i] - lo;
			}
			nrf_edgeai_obsv_update_features(&ww_obsv_ctx, feats, n);
		}
	}
#endif
	nrf_edgeai_obsv_update_probs(&ww_obsv_ctx, probs);
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

#ifdef CONFIG_WW_TRACE
	ww_trace_frame(probs, ww_count);
#endif

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
		ww_track_near_miss(class_probability, false, true);
		return false;
	}

	if (ww_count >= CONFIG_WW_COUNT_THRESHOLD) {
		ww_count = 0;
		ww_history = 0;
		refractory = WW_REFRACTORY_FRAMES;
		ww_track_near_miss(class_probability, true, false);
#ifdef CONFIG_WW_TRACE
		trace_fired = true;
#endif

		if (retry_armed) {
			LOG_INF("ww: detection accepted at retry bar %.2f", (double)retry_threshold);
			audio_telemetry_retry_detection();
			retry_frames = 0;
		}

		return true;
	}

	if (ww_track_near_miss(class_probability, false, false)) {
		retry_frames = WW_RETRY_FRAMES;
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
