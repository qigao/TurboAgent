#include "turbo_agent_speech_capture.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_speech_capture_bridge_s {
  turbo_agent_speech_input_t *input; /* borrowed, never retained beyond host */
  uint64_t utterance_id;
  atomic_int accepting;
  atomic_int first_error;
  atomic_uint in_callback;
  int begun; /* single control-plane owner only */
  int terminal;
};

static void remember_failure(turbo_agent_speech_capture_bridge_t *bridge,
                             int error) {
  int expected = TURBO_AGENT_SPEECH_OK;
  if (error == TURBO_AGENT_SPEECH_OK) return;
  (void)atomic_compare_exchange_strong_explicit(
      &bridge->first_error, &expected, error,
      memory_order_acq_rel, memory_order_acquire);
}

int turbo_agent_speech_capture_bridge_create(
    turbo_agent_speech_input_t *input, uint64_t utterance_id,
    turbo_agent_speech_capture_bridge_t **out_bridge) {
  turbo_agent_speech_capture_bridge_t *bridge;
  if (!out_bridge) return TURBO_AGENT_SPEECH_EINVAL;
  *out_bridge = NULL;
  if (!input || utterance_id == 0u) return TURBO_AGENT_SPEECH_EINVAL;
  bridge = (turbo_agent_speech_capture_bridge_t *)calloc(1, sizeof(*bridge));
  if (!bridge) return TURBO_AGENT_SPEECH_ENOMEM;
  bridge->input = input;
  bridge->utterance_id = utterance_id;
  atomic_init(&bridge->accepting, 0);
  atomic_init(&bridge->first_error, TURBO_AGENT_SPEECH_OK);
  atomic_init(&bridge->in_callback, 0u);
  *out_bridge = bridge;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_capture_bridge_begin(
    turbo_agent_speech_capture_bridge_t *bridge) {
  int rc;
  if (!bridge || bridge->begun || bridge->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  bridge->begun = 1; /* no re-entrancy through the Provider begin hook */
  rc = turbo_agent_speech_begin(bridge->input, bridge->utterance_id);
  if (rc != TURBO_AGENT_SPEECH_OK) {
    bridge->terminal = 1;
    return rc;
  }
  atomic_store_explicit(&bridge->accepting, 1, memory_order_release);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_capture_bridge_offer(
    turbo_agent_speech_capture_bridge_t *bridge,
    const uint8_t *borrowed_pcm, size_t bytes, uint64_t timestamp_us) {
  int16_t aligned_pcm[TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME];
  int rc = TURBO_AGENT_SPEECH_OK;
  if (!bridge) return TURBO_AGENT_SPEECH_EINVAL;
  /* The host's stop/drain contract keeps bridge alive across this callback.
   * In-flight count also guards the control-plane destroy precondition. */
  atomic_fetch_add_explicit(&bridge->in_callback, 1u, memory_order_acq_rel);
  if (!atomic_load_explicit(&bridge->accepting, memory_order_acquire)) {
    rc = TURBO_AGENT_SPEECH_ESTATE;
    goto done;
  }
  if (!borrowed_pcm || bytes != TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES) {
    rc = TURBO_AGENT_SPEECH_EINVAL;
    remember_failure(bridge, rc);
    goto done;
  }
  for (size_t i = 0; i < TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME; i++) {
    /* Explicitly decode S16 little-endian. No alignment or native endian
     * assumptions when Salts Capture borrows a uint8_t PCM buffer. */
    uint16_t raw = (uint16_t)borrowed_pcm[2u * i] |
                   (uint16_t)((uint16_t)borrowed_pcm[2u * i + 1u] << 8u);
    int32_t value = raw <= 32767u ? (int32_t)raw : (int32_t)raw - 65536;
    aligned_pcm[i] = (int16_t)value;
  }
  rc = turbo_agent_speech_offer_frame(
      bridge->input, bridge->utterance_id, aligned_pcm,
      TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME, timestamp_us);
  if (rc != TURBO_AGENT_SPEECH_OK) remember_failure(bridge, rc);
  memset(aligned_pcm, 0, sizeof(aligned_pcm));
done:
  atomic_fetch_sub_explicit(&bridge->in_callback, 1u, memory_order_acq_rel);
  return rc;
}

int turbo_agent_speech_capture_bridge_stop_admission(
    turbo_agent_speech_capture_bridge_t *bridge) {
  if (!bridge || !bridge->begun || bridge->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  if (!atomic_exchange_explicit(&bridge->accepting, 0,
                                memory_order_acq_rel))
    return TURBO_AGENT_SPEECH_ESTATE;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_capture_bridge_first_error(
    const turbo_agent_speech_capture_bridge_t *bridge) {
  if (!bridge) return TURBO_AGENT_SPEECH_EINVAL;
  return atomic_load_explicit(&bridge->first_error, memory_order_acquire);
}

int turbo_agent_speech_capture_bridge_finish(
    turbo_agent_speech_capture_bridge_t *bridge) {
  int rc;
  if (!bridge || !bridge->begun || bridge->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  if (atomic_load_explicit(&bridge->accepting, memory_order_acquire) ||
      atomic_load_explicit(&bridge->in_callback, memory_order_acquire))
    return TURBO_AGENT_SPEECH_EBUSY;
  bridge->terminal = 1;
  rc = turbo_agent_speech_capture_bridge_first_error(bridge);
  if (rc != TURBO_AGENT_SPEECH_OK) {
    (void)turbo_agent_speech_cancel(bridge->input);
    return rc; /* do not turn truncated/dropped microphone data into text */
  }
  return turbo_agent_speech_request_finish(bridge->input);
}

int turbo_agent_speech_capture_bridge_cancel(
    turbo_agent_speech_capture_bridge_t *bridge) {
  if (!bridge || !bridge->begun || bridge->terminal)
    return TURBO_AGENT_SPEECH_ESTATE;
  if (atomic_load_explicit(&bridge->accepting, memory_order_acquire) ||
      atomic_load_explicit(&bridge->in_callback, memory_order_acquire))
    return TURBO_AGENT_SPEECH_EBUSY; /* stop/drain must happen first */
  bridge->terminal = 1;
  return turbo_agent_speech_cancel(bridge->input);
}

int turbo_agent_speech_capture_bridge_destroy(
    turbo_agent_speech_capture_bridge_t **ptr) {
  turbo_agent_speech_capture_bridge_t *bridge;
  if (!ptr || !*ptr) return TURBO_AGENT_SPEECH_EINVAL;
  bridge = *ptr;
  if (atomic_load_explicit(&bridge->accepting, memory_order_acquire) ||
      atomic_load_explicit(&bridge->in_callback, memory_order_acquire) ||
      (bridge->begun && !bridge->terminal))
    return TURBO_AGENT_SPEECH_EBUSY;
  memset(bridge, 0, sizeof(*bridge));
  free(bridge);
  *ptr = NULL;
  return TURBO_AGENT_SPEECH_OK;
}
