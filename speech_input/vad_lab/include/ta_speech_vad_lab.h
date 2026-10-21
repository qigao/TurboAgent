#ifndef TURBO_AGENT_SPEECH_VAD_LAB_H
#define TURBO_AGENT_SPEECH_VAD_LAB_H

/* ISOLATED STRICT C11 VAD LAB — never installed/exported in Agent SDK.
 *
 * Source PCM: one owned 16 kHz mono S16 utterance; exactly 320 signed
 * samples per 20 ms frame. Caller serializes accept, settle and destroy.
 * Data, samples, speech IDs, model weights and transcripts are NOT retained.
 * No callbacks or additional worker, no fallback engine, no tool authority.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TA_VAD_LAB_RATE 16000u
#define TA_VAD_LAB_FRAME_SAMPLES 320u
#define TA_VAD_LAB_MAX_FRAMES 3000u
#define TA_VAD_LAB_SPARSE_BAND_COUNT 4u

typedef struct ta_vad_lab_owner ta_vad_lab_owner_t;

typedef enum {
  TA_VAD_LAB_UNCERTAIN = 0,
  TA_VAD_LAB_SPEECH_PRESENT = 1,
  TA_VAD_LAB_NON_SPEECH_CONFIDENT = 2
} ta_vad_lab_decision_t;

typedef enum {
  TA_VAD_LAB_OK = 0,
  TA_VAD_LAB_EINVAL = -1,
  TA_VAD_LAB_ENOMEM = -2,
  TA_VAD_LAB_ECAPACITY = -3,
  TA_VAD_LAB_ESETTLED = -4,
  TA_VAD_LAB_EEMPTY = -5
} ta_vad_lab_error_t;

/* All fields are aggregate numerical diagnostics, no source PCM or text. */
typedef struct {
  uint32_t input_frames;
  uint32_t nonzero_frames;
  uint32_t above_floor_frames;
  uint32_t below_floor_nonzero_frames;
  uint32_t voicing_cue_frames;
  uint32_t tonal_cue_frames;
  uint32_t power_modulation_events;
  uint64_t maximum_frame_power;
  uint64_t minimum_nonzero_frame_power;
  double mean_spectral_concentration;
  double mean_peak_autocorrelation;

  /* Experimental four GROUPS of sparse spectral DFT probes, NOT a
   * comprehensive filterbank. Shares are aggregated only for accepted
   * frames above the diagnostic noise floor. All four shares sum to
   * approximately one when such frames exist; no PCM is retained.
   * Spectral flux measures L1/2 change between CONSECUTIVE eligible
   * 20ms frames. The first frame after silence has no transition.
   * These cues NEVER authorize NON_SPEECH_CONFIDENT. */
  double mean_sparse_band_share[TA_VAD_LAB_SPARSE_BAND_COUNT];
  double mean_sparse_band_entropy;
  double mean_sparse_band_flux;
  uint32_t sparse_band_observed_frames;
  uint32_t sparse_band_flux_transitions;
  ta_vad_lab_decision_t decision;
  uint32_t diagnostic_only;
  uint32_t real_world_speech_or_noise_certified;
  uint32_t provider_guard_enabled;
} ta_vad_lab_result_t;

ta_vad_lab_error_t ta_vad_lab_create(ta_vad_lab_owner_t **out_owner);

/* Strictly one complete 20ms frame. Caller-owned input lifetime ends when
 * this call returns; neither input pointer nor raw sample bytes are saved.
 * A failed call never commits another frame, except the caller may destroy
 * owner to abort the experiment. */
ta_vad_lab_error_t ta_vad_lab_accept(
    ta_vad_lab_owner_t *owner, const int16_t *pcm, size_t samples);

/* Once only: reports a conservative decision and seals this Owner.
 * UNCERTAIN MUST be allowed to proceed to separately authorized ASR;
 * this lab makes NO production admission decision at all.
 *
 * NON_SPEECH_CONFIDENT is currently reserved for >=50 verified 20ms
 * frames ALL DIGITAL ZERO. This does NOT claim arbitrary noise coverage.
 * Never reject human speech on power CV, pitch, a missing voicing cue
 * or model errors. Every nonzero input returns SPEECH or UNCERTAIN. */
ta_vad_lab_error_t ta_vad_lab_settle(
    ta_vad_lab_owner_t *owner, ta_vad_lab_result_t *out_result);

size_t ta_vad_lab_accepted_frames(const ta_vad_lab_owner_t *owner);
ta_vad_lab_error_t ta_vad_lab_destroy(ta_vad_lab_owner_t **inout_owner);

#ifdef __cplusplus
}
#endif
#endif
