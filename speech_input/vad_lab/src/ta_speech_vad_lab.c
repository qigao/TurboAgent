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
  /* Aggregate-only 4 coarse groups of 12 FIXED sparse DFT probes.
   * Never cache a prior PCM frame or any transcript/audio path. */
  double sparse_band_share_sum[TA_VAD_LAB_SPARSE_BAND_COUNT];
  double previous_band_share[TA_VAD_LAB_SPARSE_BAND_COUNT];
  double sparse_band_entropy_sum;
  double sparse_band_flux_sum;
  uint32_t sparse_band_observed_frames;
  uint32_t sparse_band_flux_transitions;
  uint32_t previous_band_valid;
  uint32_t settled;
};

_Static_assert(TA_VAD_LAB_FRAME_SAMPLES == 320u,
               "20ms S16 ABI is fixed at 16kHz/320 samples");
_Static_assert(TA_VAD_LAB_SPARSE_BAND_COUNT == 4u,
               "sparse spectral diagnostic has exactly four groups");
_Static_assert(sizeof(struct ta_vad_lab_owner) <= 512u,
               "C11 VAD must have constant and bounded owner state");
_Static_assert(sizeof(int16_t) == 2u,
               "candidate requires exact 16bit signed PCM");

/* A fixed-length sparse DFT *probe* group, not a full-band filterbank.
 * Group 0 covers 250,450,650Hz probes; group 1 covers 850..1550Hz,
 * group 2 covers 2150..3950Hz; group 3 covers 5150..7550Hz. These
 * samples of the spectrum do NOT measure all energy in a frequency band.
 * All outputs are aggregates and must not become a reject threshold. */
static double spectral_concentration(const ta_vad_lab_owner_t *owner,
                                     const int16_t *pcm,
                                     double band_share[TA_VAD_LAB_SPARSE_BAND_COUNT],
                                     int *has_sparse_energy) {
  double group_energy[TA_VAD_LAB_SPARSE_BAND_COUNT] = {0.0};
  double total = 0.0;
  double largest = 0.0;
  size_t bin;
  size_t band;
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
    group_energy[bin / 3u] += e;
    if (e > largest) largest = e;
  }
  *has_sparse_energy = total > 0.0;
  for (band = 0u; band < TA_VAD_LAB_SPARSE_BAND_COUNT; ++band)
    band_share[band] = total > 0.0 ? group_energy[band] / total : 0.0;
  return total > 0.0 ? largest / total : 0.0;
}

static void update_sparse_band_observations(
    ta_vad_lab_owner_t *owner,
    const double band_share[TA_VAD_LAB_SPARSE_BAND_COUNT]) {
  const double inv_log_4 = 0.7213475204444817036799623405009;
  double entropy = 0.0;
  double flux = 0.0;
  size_t i;
  for (i = 0u; i < TA_VAD_LAB_SPARSE_BAND_COUNT; ++i) {
    const double fraction = band_share[i];
    owner->sparse_band_share_sum[i] += fraction;
    if (fraction > 0.0)
      entropy -= fraction * log(fraction);
    if (owner->previous_band_valid)
      flux += fabs(fraction - owner->previous_band_share[i]);
    owner->previous_band_share[i] = fraction;
  }
  owner->sparse_band_entropy_sum += entropy * inv_log_4;
  ++owner->sparse_band_observed_frames;
  if (owner->previous_band_valid) {
    /* Total variation distance of normalized sparse spectral groups,
     * range [0,1], insensitive to uniform overall amplitude scaling. */
    owner->sparse_band_flux_sum += 0.5 * flux;
    ++owner->sparse_band_flux_transitions;
  }
  owner->previous_band_valid = 1u;
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
    double band_share[TA_VAD_LAB_SPARSE_BAND_COUNT] = {0.0};
    int has_sparse_energy = 0;
    const double concentration = spectral_concentration(
        owner, pcm, band_share, &has_sparse_energy);
    const double correlation = max_positive_correlation(pcm);
    ++owner->above_floor_count;
    owner->concentration_sum += concentration;
    owner->correlation_sum += correlation;
    if (has_sparse_energy)
      update_sparse_band_observations(owner, band_share);
    else
      owner->previous_band_valid = 0u;
    /* High periodicity with multiple spectral probes may be a voiced
     * syllable, but is NOT a sufficient basis for a speech/no-speech gate. */
    if (correlation >= 0.62 && concentration < 0.82)
      ++owner->voicing_count;
    if (correlation >= 0.75 && concentration >= 0.72)
      ++owner->tonal_count;
  } else {
    /* Silence / below-floor input interrupts spectral flux continuity.
     * No cross-gap transition is falsely recorded as a speech feature. */
    owner->previous_band_valid = 0u;
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
  output.sparse_band_observed_frames =
      owner->sparse_band_observed_frames;
  output.sparse_band_flux_transitions =
      owner->sparse_band_flux_transitions;
  if (owner->sparse_band_observed_frames != 0u) {
    size_t band;
    for (band = 0u; band < TA_VAD_LAB_SPARSE_BAND_COUNT; ++band)
      output.mean_sparse_band_share[band] =
          owner->sparse_band_share_sum[band] /
          (double)owner->sparse_band_observed_frames;
    output.mean_sparse_band_entropy =
        owner->sparse_band_entropy_sum /
        (double)owner->sparse_band_observed_frames;
  }
  if (owner->sparse_band_flux_transitions != 0u)
    output.mean_sparse_band_flux =
        owner->sparse_band_flux_sum /
        (double)owner->sparse_band_flux_transitions;
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
