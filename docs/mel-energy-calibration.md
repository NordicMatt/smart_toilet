# Calibrating the obsv mel energy descriptor (loudness metric)

How the nRF Edge AI observability **mel energy descriptor** (`MEL_ENERGY_DESC`,
"med") was enabled and calibrated on the smart toilet, 2026-10-29, written so
it can be repeated on another Edge AI audio project. The firmware side is in
`app/src/ww/wakeword.c` (feature feed, `ww_mel_cal()`), `app/Kconfig`
(`WW_MEL_CAL`), `app/bench_melcal.conf` and `app/prj.conf`.

## 1. Check what the feature buffer actually holds (do this first)

`nrf_edgeai_dsp_features_ctx(model)->buffer` is a union (`p_i16`, `p_i32`,
`p_f32`, `p_void`). **The element type depends on the model export.** The
add-on's `ww_kws` reference reads `p_f32`. Copying it is wrong for an Axon q8
export.

- Look at the generated `nrf_edgeai_user_types.h` / `nrf_edgeai_user_model.c`:
  `nrf_user_feature_t`, the feature function (here
  `nrf_edgeai_feature_audio_mels_i16`) and the inference interface (here
  `nrf_edgeai_run_inference_axon_audiomels` + `..._dequantize_axon_q8_f32`).
- Then **dump one frame's raw bytes** on the bench to confirm. The type names
  alone mislead: here `nrf_user_feature_t` is `int32_t`, but the buffer holds
  one **int8 per mel bin** (the model's quantized input), with `0x80` (-128) at
  the quantizer floor:

  ```c
  LOG_HEXDUMP_INF(fx->buffer.p_void, 4 * fx->overall_num, "mel raw frame");
  ```

  Quiet room on this unit: first 40 bytes were mostly `80`, with a few hot
  low bins (`06 d6 cf e4 b4 …`). After that came unrelated memory (stray float
  bytes).
- Symptom of the wrong type: the calibration log shows absurd values (min
  -4.8e37, mean NaN, p01 -4.3e9). Reading 40 floats took the 40 int8 bytes
  plus 120 bytes past them. On this project that bug had garbled the **mel
  spectral descriptor** in every v3 CDR up to 3.0.3, so reports built from
  those CDRs show a wrong "sound fingerprint".

Fix used here: convert to steps above the quantizer floor and feed that one
vector to both input-side metrics:

```c
const int8_t *mel = fx->buffer.p_void;
float feats[WW_OBSV_MEL_MAX];

for (uint16_t i = 0; i < n; i++) {
	feats[i] = (float)mel[i] + 128.f;   /* 0..255, 0 = floor */
}
nrf_edgeai_obsv_update_features(&ww_obsv_ctx, feats, n);
```

These values are non-negative, which the spectral descriptor needs (it clamps
negatives). They are also on a fixed, quantizer-defined scale, which the
energy descriptor needs (a uniform level change must move them). Do **not**
lift each frame by its own minimum. That cancels the level change the
loudness metric exists to see.

## 2. Enable the metric

The Kconfig option is not enough: the app must create and register it.

```
CONFIG_NRF_EDGEAI_OBSV_METRIC_MEL_ENERGY_DESC=y
CONFIG_NRF_EDGEAI_OBSV_MEL_ENERGY_DESC_BIN_NUM=16
CONFIG_NRF_EDGEAI_OBSV_MEL_ENERGY_DESC_SCALE_P01_MILLI=<measured>
CONFIG_NRF_EDGEAI_OBSV_MEL_ENERGY_DESC_SCALE_P99_MILLI=<measured>
```

```c
static nrf_edgeai_obsv_metric_t med_metric;
static uint32_t med_buf[DIV_ROUND_UP(NRF_EDGEAI_OBSV_MED_STORAGE_BYTES(64), 4)];

nrf_edgeai_obsv_metric_med_create(&med_metric, med_buf, fx->overall_num);
nrf_edgeai_obsv_register(&ctx, &med_metric, NULL);
```

`fx->overall_num` must be `<= MEL_ENERGY_DESC_MAX_FEATURES` (default 40). Cost
here: about 0.5 KB of RAM, and the CDR grew from 353 B to about 416 B. The
library defaults (0 / 4000) are placeholders, so never ship them.

## 3. Measure p01 / p99 on the bench

`CONFIG_WW_MEL_CAL=y` (overlay `app/bench_melcal.conf`, which also turns on
`APP_AUDIO_STATS` for dBFS) keeps a 1024-value reservoir sample of every fed
feature value since boot. Every 10 s (330 inferences at 30 ms) it logs:

```
mel cal 10 s: min … max … mean …
mel cal since boot (N values): p01 … p99 … -> SCALE_P01_MILLI=… SCALE_P99_MILLI=…
```

Feed it the **same converted vector** the metrics get. Build with the normal
release overlays plus the bench overlay, flash and capture the UART log with
timestamps. Then run timed phases on the real unit, in the real mounting,
about 1 min each, and note the wall-clock start of each:

1. quiet room
2. normal talking (a video playing nearby works)
3. the wake word, repeatedly
4. a flush / the loudest normal event (skipped here)

## 4. Results on Toilet #1 (new mic + heat-staked lid mount, gain +20 dB)

| Phase | Audio RMS | Peak | 10 s mean (steps) | 10 s max |
|---|---|---|---|---|
| Quiet | -64.2 dBFS | -43.9 dBFS | 12.7-14.7 | 160-189 |
| Talking video | -50.6 dBFS | -29.0 dBFS | 14.2-23.4 | 208-220 |
| Wake word x12 (all fired, peak P 0.64-0.98) | -55.2 dBFS avg | -26.8 dBFS | 27.0-30.7 in voiced windows | 209-221 |

Cumulative over about 14 min: **p01 = 0, p99 = 164**, giving
`SCALE_P01_MILLI=0`, `SCALE_P99_MILLI=164000`.

## 5. Choosing the scale and bin count

- p01 is pinned at 0: in a quiet room most mel bins sit at the quantizer floor.
- The library defines p01/p99 over **all feature values** (not per-frame
  maxima). The cumulative p99 is time-weighted, so it drifts toward the quiet
  level the longer the quiet phase runs (it ranged 151-179 during the run).
  Use the final cumulative value over a session that mixes the phases roughly
  as the field does. The **max-energy row then saturates** at the top bin for
  most frames. That is expected, so read loudness from the **mean** row.
- With 8 bins (about 20 steps each) quiet (13), talking (22) and voice (30)
  all land in bins 0-1. **16 bins** (about 10 steps each) separates them into
  bins 1, 2 and 3, for about 4 x 8 x 4 B more RAM.
- The scale is in quantizer steps, so it depends on the model's input
  quantization and the PDM gain. Re-measure after a model re-export or a gain
  change, and on a different mic or mounting.

## 6. Replicating on another project: checklist

1. Read the generated model types, then hex-dump one feature frame. Settle the
   element type and the floor value before writing any metric code.
2. Convert to a non-negative, fixed-scale vector and feed both mel metrics from it.
3. Create and register med in the app; set BIN_NUM (16 for this skewed int8 range).
4. Build a bench variant with the reservoir logger, then run the timed phases on
   the real hardware in its real mounting.
5. Set p01/p99 from the final cumulative reading, and record the phase table and
   the conditions (mic, mount, gain, model id) next to the Kconfig values.
6. Remove the hex dump, ship, and check the next CDR: `mel_energy_desc` present
   with `config_named.scale_p01_milli/scale_p99_milli` matching, and the
   decoder's `row_sums_match_num_features` true.
