#include "turbo_agent_voice_coordinator.h"

#include <stdlib.h>
#include <string.h>

struct turbo_voice_coordinator {
  turbo_tts_output_t *source; /* borrowed */
  salts_playback_t *playback; /* borrowed */
  turbo_tts_playback_bridge_t *bridge; /* the ONLY PCM staging owner */
  uint64_t generation;
  turbo_voice_state_t state;
  int checked_final;
  int input_quiesced;
  int source_started;
  int faulted;
};

static int source_pending(const turbo_voice_coordinator_t *v) {
  turbo_tts_state_t s = turbo_tts_state(v->source);
  return s == TURBO_TTS_ACTIVE || s == TURBO_TTS_CANCEL_PENDING;
}

static int bridge_cancel(turbo_voice_coordinator_t *v) {
  int rc = turbo_tts_playback_bridge_cancel(v->bridge);
  if (rc == TURBO_TTS_PLAYBACK_EDEVICE) return TURBO_VOICE_EDEVICE;
  if (rc != TURBO_TTS_PLAYBACK_OK) return TURBO_VOICE_EPROVIDER;
  return TURBO_VOICE_OK;
}

int turbo_voice_create(turbo_tts_output_t *source, salts_playback_t *playback,
                       turbo_voice_coordinator_t **out_voice) {
  if (!out_voice) return TURBO_VOICE_EINVAL;
  *out_voice = NULL;
  if (!source || !playback || source_pending(
      &(turbo_voice_coordinator_t){ .source = source }))
    return TURBO_VOICE_EINVAL;
  turbo_voice_coordinator_t *v = calloc(1u, sizeof(*v));
  if (!v) return TURBO_VOICE_EBUSY;
  v->source = source;
  v->playback = playback;
  v->state = TURBO_VOICE_READY;
  *out_voice = v;
  return TURBO_VOICE_OK;
}

int turbo_voice_begin_input(turbo_voice_coordinator_t *v,
                            uint64_t generation) {
  if (!v || !generation) return TURBO_VOICE_EINVAL;
  if (generation <= v->generation) return TURBO_VOICE_ESTALE;
  if (v->bridge || source_pending(v) ||
      !(v->state == TURBO_VOICE_READY ||
        v->state == TURBO_VOICE_COMPLETED ||
        v->state == TURBO_VOICE_INTERRUPTED))
    return TURBO_VOICE_EBUSY;
  v->generation = generation;
  v->state = TURBO_VOICE_INPUT;
  v->checked_final = 0;
  v->input_quiesced = 0;
  v->source_started = 0;
  v->faulted = 0;
  return TURBO_VOICE_OK;
}

int turbo_voice_asr_event(turbo_voice_coordinator_t *v,
                          uint64_t generation, turbo_voice_asr_kind_t kind) {
  if (!v) return TURBO_VOICE_EINVAL;
  if (generation != v->generation || !generation) return TURBO_VOICE_ESTALE;
  if (kind != TURBO_VOICE_ASR_PARTIAL &&
      kind != TURBO_VOICE_ASR_CHECKED_FINAL) return TURBO_VOICE_EINVAL;
  if (v->state != TURBO_VOICE_INPUT) return TURBO_VOICE_EBUSY;
  if (kind == TURBO_VOICE_ASR_PARTIAL) return TURBO_VOICE_IGNORED;
  if (v->checked_final) return TURBO_VOICE_EBUSY;
  v->checked_final = 1;
  if (v->input_quiesced) v->state = TURBO_VOICE_WAIT_APPROVAL;
  return TURBO_VOICE_OK;
}

int turbo_voice_input_quiesced(turbo_voice_coordinator_t *v,
                               uint64_t generation) {
  if (!v) return TURBO_VOICE_EINVAL;
  if (generation != v->generation || !generation) return TURBO_VOICE_ESTALE;
  if (v->state == TURBO_VOICE_CANCEL_PENDING &&
      !v->source_started && !v->bridge) {
    v->input_quiesced = 1;
    v->state = TURBO_VOICE_INTERRUPTED;
    return TURBO_VOICE_OK;
  }
  if (v->state != TURBO_VOICE_INPUT) return TURBO_VOICE_EBUSY;
  v->input_quiesced = 1;
  if (v->checked_final) v->state = TURBO_VOICE_WAIT_APPROVAL;
  return TURBO_VOICE_OK;
}

int turbo_voice_approved_final(turbo_voice_coordinator_t *v,
                               uint64_t generation,
                               const char *approved_utf8, size_t bytes,
                               const char *voice_id, const char *language) {
  if (!v) return TURBO_VOICE_EINVAL;
  if (generation != v->generation || !generation) return TURBO_VOICE_ESTALE;
  if (v->state != TURBO_VOICE_WAIT_APPROVAL ||
      !v->input_quiesced || !v->checked_final || v->bridge)
    return TURBO_VOICE_EBUSY;
  int rc = turbo_tts_begin(v->source, generation, approved_utf8, bytes,
                           voice_id, language);
  if (rc != TURBO_TTS_OK) {
    if (turbo_tts_state(v->source) == TURBO_TTS_CANCEL_PENDING) {
      v->source_started = 1;
      v->faulted = 1;
      v->state = TURBO_VOICE_CANCEL_PENDING;
      return TURBO_VOICE_EPROVIDER;
    }
    return rc == TURBO_TTS_EINVAL || rc == TURBO_TTS_ECAPACITY ?
        TURBO_VOICE_EINVAL : TURBO_VOICE_EPROVIDER;
  }
  v->source_started = 1;
  rc = turbo_tts_playback_bridge_create(v->source, v->playback, &v->bridge);
  if (rc != TURBO_TTS_PLAYBACK_OK) {
    v->faulted = 1;
    v->state = TURBO_VOICE_CANCEL_PENDING;
    (void)turbo_tts_cancel(v->source);
    return TURBO_VOICE_EPROVIDER;
  }
  v->state = TURBO_VOICE_SPEAKING;
  return TURBO_VOICE_OK;
}

static int settle_cancel(turbo_voice_coordinator_t *v) {
  if (v->bridge) {
    turbo_tts_playback_bridge_state_t s =
        turbo_tts_playback_bridge_get_state(v->bridge);
    if (s != TURBO_TTS_PLAYBACK_CANCELED &&
        s != TURBO_TTS_PLAYBACK_FAILED) return TURBO_VOICE_EBUSY;
    if (s == TURBO_TTS_PLAYBACK_FAILED) v->faulted = 1;
    if (turbo_tts_playback_bridge_destroy(&v->bridge) !=
        TURBO_TTS_PLAYBACK_OK) return TURBO_VOICE_EBUSY;
  }
  if (source_pending(v)) return TURBO_VOICE_EBUSY;
  if (!v->input_quiesced) return TURBO_VOICE_EBUSY;
  v->state = v->faulted ? TURBO_VOICE_FAILED : TURBO_VOICE_INTERRUPTED;
  return TURBO_VOICE_OK;
}

int turbo_voice_interrupt(turbo_voice_coordinator_t *v,
                          uint64_t generation) {
  if (!v) return TURBO_VOICE_EINVAL;
  if (!generation || generation != v->generation) return TURBO_VOICE_ESTALE;
  if (v->state == TURBO_VOICE_INTERRUPTED ||
      v->state == TURBO_VOICE_COMPLETED) return TURBO_VOICE_OK;
  if (v->state == TURBO_VOICE_INPUT ||
      v->state == TURBO_VOICE_WAIT_APPROVAL) {
    v->state = v->input_quiesced ? TURBO_VOICE_INTERRUPTED :
                                  TURBO_VOICE_CANCEL_PENDING;
    return TURBO_VOICE_OK;
  }
  if (v->state == TURBO_VOICE_SPEAKING ||
      v->state == TURBO_VOICE_DRAIN_PENDING)
    v->state = TURBO_VOICE_CANCEL_PENDING;
  if (v->state != TURBO_VOICE_CANCEL_PENDING) return TURBO_VOICE_EBUSY;
  if (v->bridge) return bridge_cancel(v);
  if (source_pending(v) &&
      turbo_tts_state(v->source) == TURBO_TTS_ACTIVE &&
      turbo_tts_cancel(v->source) != TURBO_TTS_OK)
    return TURBO_VOICE_EPROVIDER;
  return TURBO_VOICE_OK;
}

int turbo_voice_pump(turbo_voice_coordinator_t *v,
                     size_t poll_budget, size_t *out_written) {
  if (out_written) *out_written = 0u;
  if (!v || !out_written || !poll_budget ||
      poll_budget > TURBO_VOICE_MAX_POLL_BUDGET)
    return TURBO_VOICE_EINVAL;
  if (v->state == TURBO_VOICE_SPEAKING) {
    int rc = turbo_tts_playback_bridge_pump(v->bridge,
                                            poll_budget, out_written);
    if (rc != TURBO_TTS_PLAYBACK_OK) {
      v->faulted = 1;
      v->state = TURBO_VOICE_CANCEL_PENDING;
      (void)bridge_cancel(v);
      return rc == TURBO_TTS_PLAYBACK_EDEVICE ?
          TURBO_VOICE_EDEVICE : TURBO_VOICE_EPROVIDER;
    }
    turbo_tts_playback_bridge_state_t s =
        turbo_tts_playback_bridge_get_state(v->bridge);
    if (s == TURBO_TTS_PLAYBACK_SOURCE_DONE)
      v->state = TURBO_VOICE_DRAIN_PENDING;
    else if (s == TURBO_TTS_PLAYBACK_CANCEL_PENDING ||
             s == TURBO_TTS_PLAYBACK_FAILED) {
      v->faulted = 1;
      v->state = TURBO_VOICE_CANCEL_PENDING;
    }
    return TURBO_VOICE_OK;
  }
  if (v->state != TURBO_VOICE_CANCEL_PENDING)
    return TURBO_VOICE_EBUSY;
  if (v->bridge) {
    turbo_tts_playback_bridge_state_t s =
        turbo_tts_playback_bridge_get_state(v->bridge);
    if (s == TURBO_TTS_PLAYBACK_STREAMING ||
        s == TURBO_TTS_PLAYBACK_BACKPRESSURE ||
        s == TURBO_TTS_PLAYBACK_SOURCE_DONE) {
      int rc = bridge_cancel(v);
      if (rc != TURBO_VOICE_OK) return rc;
    }
    s = turbo_tts_playback_bridge_get_state(v->bridge);
    if (s == TURBO_TTS_PLAYBACK_CANCEL_PENDING) {
      size_t discarded = 0u;
      int rc = turbo_tts_playback_bridge_pump(v->bridge, 1u, &discarded);
      if (rc != TURBO_TTS_PLAYBACK_OK)
        return rc == TURBO_TTS_PLAYBACK_EDEVICE ?
            TURBO_VOICE_EDEVICE : TURBO_VOICE_EPROVIDER;
      if (discarded != 0u) return TURBO_VOICE_EPROVIDER;
    }
    return settle_cancel(v);
  }
  if (source_pending(v)) {
    if (turbo_tts_state(v->source) == TURBO_TTS_ACTIVE &&
        turbo_tts_cancel(v->source) != TURBO_TTS_OK)
      return TURBO_VOICE_EPROVIDER;
    int16_t scratch[TURBO_TTS_SAMPLES_PER_FRAME];
    size_t count = 0u;
    turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
    int rc = turbo_tts_poll(v->source, scratch,
                            TURBO_TTS_SAMPLES_PER_FRAME, &count, &kind);
    memset(scratch, 0, sizeof(scratch));
    if (rc != TURBO_TTS_OK) return TURBO_VOICE_EPROVIDER;
  }
  return settle_cancel(v);
}

int turbo_voice_drain(turbo_voice_coordinator_t *v, uint32_t timeout_ms) {
  if (!v || !timeout_ms || timeout_ms > TURBO_VOICE_MAX_DRAIN_MS)
    return TURBO_VOICE_EINVAL;
  if (v->state != TURBO_VOICE_DRAIN_PENDING || !v->bridge)
    return TURBO_VOICE_EBUSY;
  int rc = turbo_tts_playback_bridge_drain(v->bridge, timeout_ms);
  if (rc == TURBO_TTS_PLAYBACK_EBUSY) return TURBO_VOICE_EBUSY;
  if (rc != TURBO_TTS_PLAYBACK_OK) {
    v->faulted = 1;
    v->state = TURBO_VOICE_CANCEL_PENDING;
    (void)bridge_cancel(v);
    return TURBO_VOICE_EDEVICE;
  }
  if (turbo_tts_playback_bridge_destroy(&v->bridge) !=
      TURBO_TTS_PLAYBACK_OK) return TURBO_VOICE_EBUSY;
  v->state = TURBO_VOICE_COMPLETED;
  return TURBO_VOICE_OK;
}

void turbo_voice_snapshot_init(turbo_voice_snapshot_t *snapshot) {
  if (!snapshot) return;
  memset(snapshot, 0, sizeof(*snapshot));
  snapshot->struct_size = sizeof(*snapshot);
  snapshot->abi_version = TURBO_VOICE_SNAPSHOT_ABI_VERSION;
}

int turbo_voice_get_snapshot(const turbo_voice_coordinator_t *v,
                             turbo_voice_snapshot_t *snapshot) {
  if (!v || !snapshot || snapshot->struct_size != sizeof(*snapshot) ||
      snapshot->abi_version != TURBO_VOICE_SNAPSHOT_ABI_VERSION)
    return TURBO_VOICE_EINVAL;

  uint32_t blocked = 0u;
  if ((v->state == TURBO_VOICE_INPUT ||
       v->state == TURBO_VOICE_CANCEL_PENDING) &&
      !v->input_quiesced)
    blocked |= TURBO_VOICE_BLOCK_INPUT_QUIESCE;
  if (v->bridge &&
      turbo_tts_playback_bridge_has_pending_clear(v->bridge))
    blocked |= TURBO_VOICE_BLOCK_DEVICE_CLEAR;
  if (v->state == TURBO_VOICE_CANCEL_PENDING && source_pending(v))
    blocked |= TURBO_VOICE_BLOCK_PROVIDER_SETTLE;
  if (v->state == TURBO_VOICE_DRAIN_PENDING)
    blocked |= TURBO_VOICE_BLOCK_PLAYBACK_DRAIN;
  if (v->state == TURBO_VOICE_WAIT_APPROVAL)
    blocked |= TURBO_VOICE_BLOCK_HOST_APPROVAL;
  if (v->faulted)
    blocked |= TURBO_VOICE_BLOCK_FAULT;

  snapshot->generation = v->generation;
  snapshot->state = v->state;
  snapshot->blockers = blocked;
  return TURBO_VOICE_OK;
}

turbo_voice_state_t turbo_voice_get_state(
    const turbo_voice_coordinator_t *v) {
  return v ? v->state : TURBO_VOICE_FAILED;
}

uint64_t turbo_voice_get_generation(const turbo_voice_coordinator_t *v) {
  return v ? v->generation : 0u;
}

int turbo_voice_destroy(turbo_voice_coordinator_t **handle) {
  if (!handle || !*handle) return TURBO_VOICE_EINVAL;
  turbo_voice_coordinator_t *v = *handle;
  if (v->bridge || source_pending(v) ||
      !(v->state == TURBO_VOICE_READY ||
        v->state == TURBO_VOICE_COMPLETED ||
        v->state == TURBO_VOICE_INTERRUPTED ||
        v->state == TURBO_VOICE_FAILED))
    return TURBO_VOICE_EBUSY;
  memset(v, 0, sizeof(*v));
  free(v);
  *handle = NULL;
  return TURBO_VOICE_OK;
}
