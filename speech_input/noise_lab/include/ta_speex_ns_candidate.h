#ifndef TURBOAGENT_SPEEX_NS_CANDIDATE_H
#define TURBOAGENT_SPEEX_NS_CANDIDATE_H

/* EXPERIMENTAL NS-ONLY DIAGNOSTIC. Not installed, not part of the Agent ABI.
 * The caller owns one state for one utterance / one mono channel. No DSP
 * instance, stream, microphone, model or transcript is persisted here. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TA_SPEEX_NS_RATE 16000u
#define TA_SPEEX_NS_FRAME_SAMPLES 320u
#define TA_SPEEX_NS_MAX_FRAMES 3000u

typedef struct ta_speex_ns_owner_s ta_speex_ns_owner_t;

enum {
  TA_SPEEX_NS_OK = 0,
  TA_SPEEX_NS_EINVAL = -1,
  TA_SPEEX_NS_EENGINE = -2,
  TA_SPEEX_NS_ECAPACITY = -3,
  TA_SPEEX_NS_EFAILED = -4
};

/* Exactly -12 or -18 dB noise floor; no implied fallback. SpeexDSP 1.2.1
 * must be explicitly available in the isolated diagnostic build. AGC,
 * VAD, dereverb and residual echo suppression remain OFF. */
int ta_speex_ns_create(int noise_suppress_db, ta_speex_ns_owner_t **out_owner);

/* Exactly 320 mono S16 input samples per call. Output can alias input.
 * No output is committed on invalid arguments / capacity / engine failure.
 * Upon engine failure, the owner is poisoned until explicitly destroyed.
 * Caller serializes calls, must not use concurrently with destroy. */
int ta_speex_ns_process(ta_speex_ns_owner_t *owner,
                        const int16_t *input, size_t count,
                        int16_t *output);

size_t ta_speex_ns_processed_frames(const ta_speex_ns_owner_t *owner);

/* Returns an error for null pointer/owner. Must drain any caller callbacks
 * before destruction: this owner makes no callbacks of its own. */
int ta_speex_ns_destroy(ta_speex_ns_owner_t **owner);

#ifdef __cplusplus
}
#endif
#endif
