#include "ta_speex_ns_candidate.h"

#include <speex/speex_preprocess.h>
#include <speex/speexdsp_types.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(spx_int16_t) == sizeof(int16_t),
               "SpeexDSP frame sample must be 16-bit signed PCM");
_Static_assert(TA_SPEEX_NS_FRAME_SAMPLES == 320u,
               "Diagnostic must retain SpeechInput's 20ms frame ABI");

struct ta_speex_ns_owner_s {
  SpeexPreprocessState *state;
  size_t used_frames;
  int failed;
  int finished;
  int noise_suppress_db;
};

static int set_checked(SpeexPreprocessState *state, int set_cmd,
                       int get_cmd, int expected) {
  int got = -12345;
  int value = expected;
  if (speex_preprocess_ctl(state, set_cmd, &value) != 0)
    return -1;
  if (speex_preprocess_ctl(state, get_cmd, &got) != 0 ||
      got != expected)
    return -1;
  return 0;
}

int ta_speex_ns_create(int db, ta_speex_ns_owner_t **out_owner) {
  ta_speex_ns_owner_t *owner = NULL;
  if (!out_owner || *out_owner != NULL || (db != -12 && db != -18))
    return TA_SPEEX_NS_EINVAL;

  owner = (ta_speex_ns_owner_t *)calloc(1u, sizeof(*owner));
  if (!owner) return TA_SPEEX_NS_EENGINE;
  owner->state = speex_preprocess_state_init(
      (int)TA_SPEEX_NS_FRAME_SAMPLES, (int)TA_SPEEX_NS_RATE);
  if (!owner->state) goto failure;

  /* An optional noise-suppression filter, NOT a voice gate or AGC. All
   * options must be supported and read back exactly by pinned SpeexDSP.
   * Never silently fall back to different settings or raw PCM passthrough. */
  if (set_checked(owner->state,
                  SPEEX_PREPROCESS_SET_DENOISE,
                  SPEEX_PREPROCESS_GET_DENOISE, 1) != 0 ||
      set_checked(owner->state,
                  SPEEX_PREPROCESS_SET_AGC,
                  SPEEX_PREPROCESS_GET_AGC, 0) != 0 ||
      set_checked(owner->state,
                  SPEEX_PREPROCESS_SET_VAD,
                  SPEEX_PREPROCESS_GET_VAD, 0) != 0 ||
      set_checked(owner->state,
                  SPEEX_PREPROCESS_SET_DEREVERB,
                  SPEEX_PREPROCESS_GET_DEREVERB, 0) != 0 ||
      set_checked(owner->state,
                  SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,
                  SPEEX_PREPROCESS_GET_NOISE_SUPPRESS, db) != 0)
    goto failure;

  owner->noise_suppress_db = db;
  *out_owner = owner;
  return TA_SPEEX_NS_OK;

failure:
  if (owner->state) speex_preprocess_state_destroy(owner->state);
  memset(owner, 0, sizeof(*owner));
  free(owner);
  return TA_SPEEX_NS_EENGINE;
}

int ta_speex_ns_process(ta_speex_ns_owner_t *owner,
                        const int16_t *input, size_t count,
                        int16_t *output) {
  spx_int16_t local[TA_SPEEX_NS_FRAME_SAMPLES];
  int rc;
  if (!owner || !owner->state || !input || !output ||
      count != TA_SPEEX_NS_FRAME_SAMPLES)
    return TA_SPEEX_NS_EINVAL;
  if (owner->failed) return TA_SPEEX_NS_EFAILED;
  if (owner->finished) return TA_SPEEX_NS_ESETTLED;
  if (owner->used_frames == TA_SPEEX_NS_MAX_FRAMES)
    return TA_SPEEX_NS_ECAPACITY;

  memcpy(local, input, sizeof(local));
  rc = speex_preprocess_run(owner->state, local);
  /* With VAD disabled the returned speech bit is NOT authoritative.
   * Reject only actual external engine errors, never produce NO_SPEECH. */
  if (rc < 0) {
    memset(local, 0, sizeof(local));
    owner->failed = 1;
    return TA_SPEEX_NS_EENGINE;
  }
  memcpy(output, local, sizeof(local));
  memset(local, 0, sizeof(local));
  ++owner->used_frames;
  return TA_SPEEX_NS_OK;
}

int ta_speex_ns_finish(ta_speex_ns_owner_t *owner,
                       int16_t *out_tail, size_t count) {
  spx_int16_t silent_frame[TA_SPEEX_NS_FRAME_SAMPLES] = {0};
  int rc;
  if (!owner || !owner->state || !out_tail ||
      count != TA_SPEEX_NS_FRAME_SAMPLES || owner->used_frames == 0u)
    return TA_SPEEX_NS_EINVAL;
  if (owner->failed) return TA_SPEEX_NS_EFAILED;
  if (owner->finished) return TA_SPEEX_NS_ESETTLED;
  /* The frame emitted after this zero-input drive may correspond to the
   * prior *real* frame due to FFT overlap. The tool must prove the latency
   * before treating it as the speech tail; never classify it as VAD. */
  rc = speex_preprocess_run(owner->state, silent_frame);
  if (rc < 0) {
    memset(silent_frame, 0, sizeof(silent_frame));
    owner->failed = 1;
    return TA_SPEEX_NS_EENGINE;
  }
  memcpy(out_tail, silent_frame, sizeof(silent_frame));
  memset(silent_frame, 0, sizeof(silent_frame));
  owner->finished = 1;
  return TA_SPEEX_NS_OK;
}

size_t ta_speex_ns_processed_frames(const ta_speex_ns_owner_t *owner) {
  return owner ? owner->used_frames : 0u;
}

int ta_speex_ns_destroy(ta_speex_ns_owner_t **ptr) {
  ta_speex_ns_owner_t *owner;
  if (!ptr || !*ptr) return TA_SPEEX_NS_EINVAL;
  owner = *ptr;
  *ptr = NULL;
  if (owner->state) speex_preprocess_state_destroy(owner->state);
  memset(owner, 0, sizeof(*owner));
  free(owner);
  return TA_SPEEX_NS_OK;
}
