#include "ta_speech_vad_lab.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "VAD C11 lab check failed at %d\n", __LINE__); \
  return 1; \
} } while (0)

static const double pi = 3.1415926535897932384626433832795;

static void sine_tone(int16_t *pcm, int amplitude) {
  size_t i;
  for (i = 0u; i < TA_VAD_LAB_FRAME_SAMPLES; ++i) {
    const double sample = (double)amplitude *
        sin((2.0 * pi * 5.0 * (double)i) /
            (double)TA_VAD_LAB_FRAME_SAMPLES);
    pcm[i] = (int16_t)sample;
  }
}

static void periodic_broadband(int16_t *pcm, size_t frame_index) {
  uint32_t seed = UINT32_C(2166136261);
  int16_t base[80];
  size_t i;
  const int amplitude = (frame_index / 5u) % 2u ? 3 : 1;
  for (i = 0u; i < 80u; ++i) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    base[i] = (int16_t)((int)((seed >> 20) % 201u) - 100);
  }
  for (i = 0u; i < TA_VAD_LAB_FRAME_SAMPLES; ++i)
    pcm[i] = (int16_t)((int)base[i % 80u] * 40 * amplitude);
}

static int verify_zero_speech_safety(void) {
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t result;
  int16_t zero[TA_VAD_LAB_FRAME_SAMPLES] = {0};
  size_t i;

  CHECK(ta_vad_lab_destroy(NULL) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_accepted_frames(NULL) == 0u);
  CHECK(ta_vad_lab_create(NULL) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_EEMPTY);
  CHECK(ta_vad_lab_accept(owner, zero, 319u) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_accept(owner, zero, 321u) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_accept(owner, NULL, 320u) == TA_VAD_LAB_EINVAL);
  CHECK(ta_vad_lab_accepted_frames(owner) == 0u);
  for (i = 0u; i < 49u; ++i)
    CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.input_frames == 49u);
  CHECK(result.decision == TA_VAD_LAB_UNCERTAIN);
  CHECK(result.nonzero_frames == 0u);
  CHECK(result.diagnostic_only == 1u);
  CHECK(result.provider_guard_enabled == 0u);
  CHECK(result.real_world_speech_or_noise_certified == 0u);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_ESETTLED);
  CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_ESETTLED);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  CHECK(owner == NULL);

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 50u; ++i)
    CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.decision == TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(result.nonzero_frames == 0u);
  CHECK(result.minimum_nonzero_frame_power == 0u); /* invalid field? */
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  return 0;
}

static int verify_nonzero_never_rejected(void) {
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t result;
  int16_t tone[320], frame[320];
  size_t i;
  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  sine_tone(tone, 12000);
  for (i = 0u; i < 90u; ++i)
    CHECK(ta_vad_lab_accept(owner, tone, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.nonzero_frames == 90u);
  CHECK(result.decision == TA_VAD_LAB_UNCERTAIN);
  CHECK(result.mean_spectral_concentration >= 0.0);
  CHECK(result.mean_spectral_concentration <= 1.0);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 99u; ++i) {
    memset(frame, 0, sizeof(frame));
    if (i == 42u) frame[100] = (int16_t)1;
    CHECK(ta_vad_lab_accept(owner, frame, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.nonzero_frames == 1u);
  CHECK(result.decision != TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);

  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < 100u; ++i) {
    periodic_broadband(frame, i);
    CHECK(ta_vad_lab_accept(owner, frame, 320u) == TA_VAD_LAB_OK);
  }
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.nonzero_frames == 100u);
  CHECK(result.power_modulation_events >= 3u);
  CHECK(result.mean_peak_autocorrelation <= 1.0);
  CHECK(result.mean_peak_autocorrelation >= 0.0);
  CHECK(result.decision != TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  return 0;
}

static int verify_full_capacity_and_settlement(void) {
  ta_vad_lab_owner_t *owner = NULL;
  ta_vad_lab_result_t result;
  int16_t zero[320] = {0};
  size_t i;
  CHECK(ta_vad_lab_create(&owner) == TA_VAD_LAB_OK);
  for (i = 0u; i < TA_VAD_LAB_MAX_FRAMES; ++i)
    CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_OK);
  CHECK(ta_vad_lab_accepted_frames(owner) == TA_VAD_LAB_MAX_FRAMES);
  CHECK(ta_vad_lab_accept(owner, zero, 320u) == TA_VAD_LAB_ECAPACITY);
  CHECK(ta_vad_lab_accepted_frames(owner) == TA_VAD_LAB_MAX_FRAMES);
  CHECK(ta_vad_lab_settle(owner, &result) == TA_VAD_LAB_OK);
  CHECK(result.input_frames == TA_VAD_LAB_MAX_FRAMES);
  CHECK(result.decision == TA_VAD_LAB_NON_SPEECH_CONFIDENT);
  CHECK(ta_vad_lab_destroy(&owner) == TA_VAD_LAB_OK);
  return 0;
}

int main(void) {
  if (verify_zero_speech_safety() != 0 ||
      verify_nonzero_never_rejected() != 0 ||
      verify_full_capacity_and_settlement() != 0)
    return 1;
  puts("{\"C11_VAD_tri_state\":true,\"zero_1s_confident\":true,"
       "\"short_zero_uncertain\":true,\"nonzero_never_confidently_rejected\":true,"
       "\"bounded_3000_frames\":true,\"one_shot_settlement\":true,"
       "\"actual_model_or_speaker_certification\":false}");
  return 0;
}
