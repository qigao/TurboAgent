#include "ta_speech_vad_lab.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
  TA_VAD_LAB_SPECTRAL_BINS = 12,
  TA_VAD_LAB_MIN_CERTAIN_ZERO_FRAMES = 50,
  TA_VAD_LAB_MIN_SPEECH_CUE_FRAMES = 6,
  TA_VAD_LAB_MIN_SPEECH_NONZERO_FRAMES = 12,
  TA_VAD_LAB_MIN_SPEECH_MODULATION_EVENTS = 3
};

/* A fixed, intentionally sparse set of DFT bins spanning 250–7550 Hz.
 * They are feature probes, NOT a validated wideband phonetic classifier.
 * A single tonal/whisper/broadband cue is NEVER used for NO_SPEECH. */
static const int probe_bins[TA_VAD_LAB_SPECTRAL_BINS] = {
  5, 9, 13, 17, 23, 31, 43, 59, 79, 103, 127, 151
};
static const double pi = 3.1415926535897932384626433832795;

struct ta_vad_lab_owner {
  double coefficients[TA_VAD_LAB_SPECTRAL_BINS];
  uint32_t frame_count;
  uint32_t nonzero_count;
  uint32_t above_floor_count;
  uint32_t voicing_count;
  uint32_t tonal_count;
  uint32_t modulation_count;
  uint64_t largest_power;
  uint64_t smallest_nonzero_power;
  uint64_t previous_power;
  double concentration_sum;
  double correlation_sum;
  uint32_t settled;
};

_Static_assert(TA_VAD_LAB_FRAME_SAMPLES == 320u,
               "20ms S16 ABI is fixed at 16kHz/320 samples");
_Static_assert(sizeof(int16_t) == 2u,
               "candidate requires exact 16bit signed PCM");

static double spectral_concentration(const ta_vad_lab_owner_t *owner,
                                     const int16_t *pcm) {
  double total = 0.0;
  double largest = 0.0;
  size_t bin;
  for (bin = 0u; bin < TA_VAD_LAB_SPECTRAL_BINS; ++bin) {
    double before = 0.0;
    double earlier = 0.0;
    double current = 0.0;
    double e;
    const double coefficient = owner->coefficients[bin];
    size_t sample;
    for (sample = 0u; sample < TA_VAD_LAB_FRAME_SAMPLES; ++sample) {
      current = (double)pcm[sample] + coefficient * before - earlier;
      earlier = before;
      before = current;
    }
    e = before * before + earlier * earlier
        - coefficient * before * earlier;
    /* Clamp tiny floating-point negative cancellation to zero. */
    if (e < 0.0) e = 0.0;
    total += e;
    if (e > largest) largest = e;
  }
  return total > 0.0 ? largest / total : 0.0;
}

/* Normalized positive time-domain autocorrelation, pitch-like diagnostic.
 * A periodic alarm or tonal hum ALSO has high autocorrelation, so this
 * must never authorize confident NO_SPEECH or Agent tool execution. */
static double max_positive_correlation(const int16_t *pcm) {
  double best = 0.0;
  size_t lag;
  for (lag = 40u; lag <= 160u; lag += 4u) {
    int64_t dot = 0;
    uint64_t left = 0u;
    uint64_t right = 0u;
    size_t i;
    const size_t span = TA_VAD_LAB_FRAME_SAMPLES - lag;
    for (i = 0u; i < span; ++i) {
      const int64_t x = (int64_t)pcm[i];
      const int64_t y = (int64_t)pcm[i + lag];
      dot += x * y;
      left += (uint64_t)(x * x);
      right += (uint64_t)(y * y);
    }
    if (dot > 0 && left != 0u && right != 0u) {
      const double norm = (double)dot /
          sqrt((double)left * (double)right);
      if (norm > best) best = norm;
    }
  }
  return best > 1.0 ? 1.0 : best;
}

ta_vad_lab_error_t ta_vad_lab_create(ta_vad_lab_owner_t **out_owner) {
  ta_vad_lab_owner_t *owner;
  size_t i;
  if (!out_owner || *out_owner != NULL) return TA_VAD_LAB_EINVAL;
  owner = (ta_vad_lab_owner_t *)calloc(1u, sizeof(*owner));
  if (!owner) return TA_VAD_LAB_ENOMEM;
  for (i = 0u; i < TA_VAD_LAB_SPECTRAL_BINS; ++i) {
    owner->coefficients[i] =
        2.0 * cos((2.0 * pi * (double)probe_bins[i]) /
                  (double)TA_VAD_LAB_FRAME_SAMPLES);
  }
  owner->smallest_nonzero_power = UINT64_MAX;
  *out_owner = owner;
  return TA_VAD_LAB_OK;
}

ta_vad_lab_error_t ta_vad_lab_accept(ta_vad_lab_owner_t *owner,
                                     const int16_t *pcm, size_t samples) {
  uint64_t power = 0u;
  uint32_t nonzero = 0u;
  size_t i;
  if (!owner || !pcm || samples != TA_VAD_LAB_FRAME_SAMPLES)
    return TA_VAD_LAB_EINVAL;
  if (owner->settled) return TA_VAD_LAB_ESETTLED;
  if (owner->frame_count == TA_VAD_LAB_MAX_FRAMES)
    return TA_VAD_LAB_ECAPACITY;
  for (i = 0u; i < samples; ++i) {
    const int64_t x = (int64_t)pcm[i];
    power += (uint64_t)(x * x);
    nonzero += (uint32_t)(x != 0);
  }

  if (power > owner->largest_power) owner->largest_power = power;
  if (power != 0u) {
    ++owner->nonzero_count;
    if (power < owner->smallest_nonzero_power)
      owner->smallest_nonzero_power = power;
  }

  /* Do not multiply two potentially huge uint64 powers. Double conversion
   * here does not alter any original PCM, only a numeric diagnostic. */
  if (owner->frame_count != 0u) {
    const double prior = (double)owner->previous_power;
    const double now = (double)power;
    if ((prior > 0.0 && now > prior * 1.7) ||
        (now > 0.0 && prior > now * 1.7))
      ++owner->modulation_count;
  }
  owner->previous_power = power;

  if (nonzero != 0u && power >= UINT64_C(1310720)) {
    const double concentration = spectral_concentration(owner, pcm);
    const double correlation = max_positive_correlation(pcm);
    ++owner->above_floor_count;
    owner->concentration_sum += concentration;
    owner->correlation_sum += correlation;
    /* High periodicity with multiple spectral probes may be a voiced
     * syllable, but is NOT a sufficient basis for a speech/no-speech gate. */
    if (correlation >= 0.62 && concentration < 0.82)
      ++owner->voicing_count;
    if (correlation >= 0.75 && concentration >= 0.72)
      ++owner->tonal_count;
  }
  ++owner->frame_count;
  return TA_VAD_LAB_OK;
}

ta_vad_lab_error_t ta_vad_lab_settle(ta_vad_lab_owner_t *owner,
                                     ta_vad_lab_result_t *out_result) {
  ta_vad_lab_result_t output;
  if (!owner || !out_result) return TA_VAD_LAB_EINVAL;
  if (owner->settled) return TA_VAD_LAB_ESETTLED;
  if (owner->frame_count == 0u) return TA_VAD_LAB_EEMPTY;
  memset(&output, 0, sizeof(output));
  output.input_frames = owner->frame_count;
  output.nonzero_frames = owner->nonzero_count;
  output.above_floor_frames = owner->above_floor_count;
  output.voicing_cue_frames = owner->voicing_count;
  output.tonal_cue_frames = owner->tonal_count;
  output.power_modulation_events = owner->modulation_count;
  output.maximum_frame_power = owner->largest_power;
  output.minimum_nonzero_frame_power =
      owner->nonzero_count ? owner->smallest_nonzero_power : 0u;
  if (owner->above_floor_count != 0u) {
    output.mean_spectral_concentration =
        owner->concentration_sum / (double)owner->above_floor_count;
    output.mean_peak_autocorrelation =
        owner->correlation_sum / (double)owner->above_floor_count;
  }
  /* "Confident no voice" is limited to >=1s of BYTE-EXACT digital zero.
   * No nonzero audio, including alarms, power-stationary speech, dynamic
   * broadband noise or silence+voice, can be rejected by this prototype. */
  if (owner->frame_count >= TA_VAD_LAB_MIN_CERTAIN_ZERO_FRAMES &&
      owner->nonzero_count == 0u)
    output.decision = TA_VAD_LAB_NON_SPEECH_CONFIDENT;
  else if (owner->voicing_count >= TA_VAD_LAB_MIN_SPEECH_CUE_FRAMES &&
           owner->nonzero_count >= TA_VAD_LAB_MIN_SPEECH_NONZERO_FRAMES &&
           owner->modulation_count >= TA_VAD_LAB_MIN_SPEECH_MODULATION_EVENTS)
    output.decision = TA_VAD_LAB_SPEECH_PRESENT;
  else
    output.decision = TA_VAD_LAB_UNCERTAIN;

  output.diagnostic_only = 1u;
  output.real_world_speech_or_noise_certified = 0u;
  output.provider_guard_enabled = 0u;
  *out_result = output;
  owner->settled = 1u;
  return TA_VAD_LAB_OK;
}

size_t ta_vad_lab_accepted_frames(const ta_vad_lab_owner_t *owner) {
  return owner ? (size_t)owner->frame_count : 0u;
}

ta_vad_lab_error_t ta_vad_lab_destroy(ta_vad_lab_owner_t **inout_owner) {
  ta_vad_lab_owner_t *owner;
  if (!inout_owner || !*inout_owner) return TA_VAD_LAB_EINVAL;
  owner = *inout_owner;
  *inout_owner = NULL;
  memset(owner, 0, sizeof(*owner));
  free(owner);
  return TA_VAD_LAB_OK;
}
