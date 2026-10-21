#include "ta_speech_vad_lab.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "sparse-band VAD test failed at line %d\n", __LINE__); \
  return 1; \
} } while (0)

static const double pi = 3.1415926535897932384626433832795;

static void make_sparse_probe_tone_amplitude(
    int16_t *pcm, unsigned int bin, double amplitude) {
  size_t sample;
  for (sample = 0u; sample < TA_VAD_LAB_FRAME_SAMPLES; ++sample)
    pcm[sample] = (int16_t)(amplitude *
        sin((2.0 * pi * (double)bin * (double)sample) /
            (double)TA_VAD_LAB_FRAME_SAMPLES));
}

static void make_sparse_probe_tone(int16_t *pcm, unsigned int bin) {
  make_sparse_probe_tone_amplitude(pcm, bin, 7000.0);
}

static int check_band_result(const ta_vad_lab_result_t *r) {
  double sum = 0.0;
  size_t i;
  CHECK(r != NULL);
  CHECK(r->diagnostic_only == 1u);
  CHECK(r->real_world_speech_or_noise_certified == 0u);
  CHECK(r->provider_guard_enabled == 0u);
  CHECK(r->sparse_band_observed_frames <= r->above_floor_frames);
  CHECK(r->sparse_band_flux_transitions <= r->sparse_band_observed_frames);
  for (i = 0u; i < TA_VAD_LAB_SPARSE_BAND_COUNT; ++i) {
    const double share = r->mean_sparse_band_share[i];
    CHECK(isfinite(share));
    CHECK(share >= 0.0 && share <= 1.000001);
    sum += share;
  }
  if (r->sparse_band_observed_frames > 0u)
    CHECK(fabs(sum - 1.0) < 0.00001);
  else
    CHECK(sum == 0.0);
  CHECK(isfinite(r->mean_sparse_band_entropy));
  CHECK(r->mean_sparse_band_entropy >= 0.0);
  CHECK(r->mean_sparse_band_entropy <= 1.00001);
  CHECK(isfinite(r->mean_sparse_band_flux));
  CHECK(r->mean_sparse_band_flux >= 0.0);
  CHECK(r->mean_sparse_band_flux <= 1.00001);
  CHECK(r->decision != TA_VAD_LAB_NON_SPEECH_CONFIDENT ||
        r->nonzero_frames == 0u);
  return 0;
}

static int test_stationary_tone_and_alternating_sparse_bands(void) {
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t steady;
  ta_vad_lab_result_t alternate;
  int16_t source[TA_VAD_LAB_FRAME_SAMPLES];
  size_t i;

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 80u; ++i) {
    make_sparse_probe_tone(source, 5u);
    CHECK(ta_vad_lab_accept(owner, source, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_settle(owner, &steady) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&steady) == 0);
  CHECK(steady.sparse_band_observed_frames == 80u);
  CHECK(steady.sparse_band_flux_transitions == 79u);
  CHECK(steady.mean_sparse_band_share[0] >= 0.75);
  CHECK(steady.mean_sparse_band_flux < 0.01);
  CHECK(steady.mean_sparse_band_entropy < 0.50);
  CHECK(steady.decision == TA_VAD_LAB_UNCERTAIN);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 80u; ++i) {
    make_sparse_probe_tone(source, i % 2u ? 103u : 5u);
    CHECK(ta_vad_lab_accept(owner, source, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_settle(owner, &alternate) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&alternate) == 0);
  CHECK(alternate.sparse_band_observed_frames == 80u);
  CHECK(alternate.sparse_band_flux_transitions == 79u);
  CHECK(alternate.mean_sparse_band_share[0] >= 0.30);
  CHECK(alternate.mean_sparse_band_share[3] >= 0.30);
  CHECK(alternate.mean_sparse_band_flux > 0.50);
  CHECK(alternate.mean_sparse_band_flux >
        steady.mean_sparse_band_flux + 0.45);
  /* Same frame RMS under alternating frequencies: multiband flux, not
   * frame power only, records the changing acoustic spectrum. */
  CHECK(alternate.power_modulation_events == 0u);
  CHECK(alternate.decision != TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  return 0;
}

static int test_uniform_amplitude_scaling_preserves_normalized_flux(void) {
  ta_vad_lab_owner_t *steady_owner = NULL;
  ta_vad_lab_owner_t *scaled_owner = NULL;
  ta_vad_lab_result_t steady;
  ta_vad_lab_result_t scaled;
  int16_t frame[TA_VAD_LAB_FRAME_SAMPLES];
  size_t i;
  CHECK(ta_vad_lab_create(&steady_owner) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_create(&scaled_owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 80u; ++i) {
    make_sparse_probe_tone_amplitude(frame, 23u, 7000.0);
    CHECK(ta_vad_lab_accept(steady_owner, frame, 320u) == TA_VAD_LAB_OK);
    make_sparse_probe_tone_amplitude(
        frame, 23u, i % 2u == 0u ? 3500.0 : 12000.0);
    CHECK(ta_vad_lab_accept(scaled_owner, frame, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_settle(steady_owner, &steady) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(scaled_owner, &scaled) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&steady) == 0);
  CHECK(check_band_result(&scaled) == 0);
  CHECK(steady.sparse_band_flux_transitions == 79u);
  CHECK(scaled.sparse_band_flux_transitions == 79u);
  CHECK(steady.mean_sparse_band_flux < 0.01);
  CHECK(scaled.mean_sparse_band_flux < 0.01);
  CHECK(fabs(steady.mean_sparse_band_flux -
             scaled.mean_sparse_band_flux) < 0.01);
  CHECK(ta_vad_lab_destroy(&steady_owner) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_destroy(&scaled_owner) == TA_VAD_LAB_OK);
  return 0;
}

static int test_silence_discontinuity_and_zero(void) {
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t r;
  int16_t tone[TA_VAD_LAB_FRAME_SAMPLES];
  int16_t zero[TA_VAD_LAB_FRAME_SAMPLES] = {0};
  size_t i;
  make_sparse_probe_tone(tone, 5u);
  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 20u; ++i)
    CHECK(ta_vad_lab_accept(owner, tone, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_OK);
  for (i = 0u; i < 20u; ++i)
    CHECK(ta_vad_lab_accept(owner, tone, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(owner, &r) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&r) == 0);
  CHECK(r.input_frames == 41u);
  CHECK(r.sparse_band_observed_frames == 40u);
  CHECK(r.sparse_band_flux_transitions == 38u);
  CHECK(r.mean_sparse_band_flux < 0.01);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 50u; ++i)
    CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(owner, &r) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&r) == 0);
  CHECK(r.sparse_band_observed_frames == 0u);
  CHECK(r.sparse_band_flux_transitions == 0u);
  CHECK(r.decision == TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  return 0;
}

static int test_owner_independent_replay_and_cap(void) {
  ta_vad_lab_owner_t *a = NULL, *b = NULL;
  ta_vad_lab_result_t left, right;
  int16_t frame[320];
  int16_t zero[320] = {0};
  size_t i;
  CHECK(ta_vad_lab_create(&a) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_create(&b) == TA_VAD_LAB_OK);
  for (i = 0u; i < 80u; ++i) {
    make_sparse_probe_tone(frame, i % 3u ? 23u : 127u);
    CHECK(ta_vad_lab_accept(a, frame, 320u) == TA_VAD_LAB_OK);
    CHECK(ta_vad_lab_accept(b, frame, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_accept(a, zero, 319u) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_accepted_frames(a) == 80u);
  CHECK(ta_vad_lab_settle(a, &left) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(b, &right) == TA_VAD_LAB_OK);
  CHECK(check_band_result(&left) == 0);
  CHECK(check_band_result(&right) == 0);
  for (i = 0u; i < TA_VAD_LAB_SPARSE_BAND_COUNT; ++i)
    CHECK(left.mean_sparse_band_share[i] == right.mean_sparse_band_share[i]);
  CHECK(left.mean_sparse_band_flux == right.mean_sparse_band_flux);
  CHECK(left.mean_sparse_band_entropy == right.mean_sparse_band_entropy);
  CHECK(ta_vad_lab_accept(a, frame, 320u) == TA_VAD_LAB_ESETTLED);
  CHECK(ta_vad_lab_destroy(&a) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_destroy(&b) == TA_VAD_LAB_OK);
  return 0;
}

int main(void) {
  if (test_stationary_tone_and_alternating_sparse_bands() != 0 ||
      test_uniform_amplitude_scaling_preserves_normalized_flux() != 0 ||
      test_silence_discontinuity_and_zero() != 0 ||
      test_owner_independent_replay_and_cap() != 0)
    return 1;
  puts("{\"sparse_bands\":4,\"sample_rate\":16000,"
       "\"samples_per_frame\":320,\"flux_detects_frequency_switch\":true,"
       "\"silence_does_not_create_flux_transition\":true,"
       "\"two_owners_deterministic\":true,\"pcm_retained\":false,"
       "\"model_inference_used\":false,\"confidence_reject_unchanged\":true}");
  return 0;
}
