#include "turbo_agent_tts_playback_bridge.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct turbo_tts_playback_bridge {
  turbo_tts_output_t *source;   /* borrowed until bridge destruction */
  salts_playback_t *playback;   /* borrowed, single writer on this control owner */
  int16_t staging[TURBO_TTS_SAMPLES_PER_FRAME];
  size_t pending_bytes;
  size_t written_offset;
  int clear_needed;
  turbo_tts_playback_bridge_state_t state;
};

static void clear_staging(turbo_tts_playback_bridge_t *b) {
  memset(b->staging, 0, sizeof(b->staging));
  b->pending_bytes = 0u;
  b->written_offset = 0u;
}

/* Pending source settlement is NOT a DSO unload permission. */
static int source_is_settled(turbo_tts_output_t *source) {
  turbo_tts_state_t s = turbo_tts_state(source);
  return s == TURBO_TTS_CANCELED || s == TURBO_TTS_FAILED ||
         s == TURBO_TTS_SOURCE_DONE;
}

int turbo_tts_playback_bridge_create(
    turbo_tts_output_t *source, salts_playback_t *playback,
    turbo_tts_playback_bridge_t **out_bridge) {
  if (!out_bridge) return TURBO_TTS_PLAYBACK_EINVAL;
  *out_bridge = NULL;
  if (!source || !playback || turbo_tts_state(source) != TURBO_TTS_ACTIVE)
    return TURBO_TTS_PLAYBACK_EINVAL;
  turbo_tts_playback_bridge_t *b =
      (turbo_tts_playback_bridge_t *)calloc(1u, sizeof(*b));
  if (!b) return TURBO_TTS_PLAYBACK_EBUSY;
  b->source = source;
  b->playback = playback;
  b->state = TURBO_TTS_PLAYBACK_STREAMING;
  *out_bridge = b;
  return TURBO_TTS_PLAYBACK_OK;
}

static int pump_cancel_pending(turbo_tts_playback_bridge_t *b) {
  if (b->clear_needed) {
    if (salts_playback_clear(b->playback) != SALTS_PLAYBACK_OK)
      return TURBO_TTS_PLAYBACK_EDEVICE;
    b->clear_needed = 0;
  }
  if (source_is_settled(b->source)) {
    b->state = turbo_tts_state(b->source) == TURBO_TTS_FAILED ?
        TURBO_TTS_PLAYBACK_FAILED : TURBO_TTS_PLAYBACK_CANCELED;
    return TURBO_TTS_PLAYBACK_OK;
  }
  int16_t unused[TURBO_TTS_SAMPLES_PER_FRAME];
  size_t count = 0u;
  turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
  int rc = turbo_tts_poll(b->source, unused, TURBO_TTS_SAMPLES_PER_FRAME,
                          &count, &kind);
  memset(unused, 0, sizeof(unused));
  if (rc != TURBO_TTS_OK) {
    /* A Provider may be broken or non-cooperative: retain its borrowed
     * context/DSO, never convert this into a successful cancellation. */
    return TURBO_TTS_PLAYBACK_EPROVIDER;
  }
  if (source_is_settled(b->source))
    b->state = turbo_tts_state(b->source) == TURBO_TTS_FAILED ?
        TURBO_TTS_PLAYBACK_FAILED : TURBO_TTS_PLAYBACK_CANCELED;
  return TURBO_TTS_PLAYBACK_OK;
}

int turbo_tts_playback_bridge_pump(turbo_tts_playback_bridge_t *b,
                                   size_t poll_budget, size_t *out_written) {
  if (out_written) *out_written = 0u;
  if (!b || !out_written || poll_budget == 0u)
    return TURBO_TTS_PLAYBACK_EINVAL;
  if (b->state == TURBO_TTS_PLAYBACK_CANCEL_PENDING)
    return pump_cancel_pending(b);
  if (b->state != TURBO_TTS_PLAYBACK_STREAMING &&
      b->state != TURBO_TTS_PLAYBACK_BACKPRESSURE)
    return TURBO_TTS_PLAYBACK_EBUSY;

  size_t polls = 0u;
  for (;;) {
    if (b->pending_bytes > b->written_offset) {
      size_t remaining = b->pending_bytes - b->written_offset;
      size_t wrote = 0u;
      int rc = salts_playback_write(
          b->playback,
          (const uint8_t *)b->staging + b->written_offset,
          remaining, &wrote);
      if (rc != SALTS_PLAYBACK_OK || wrote > remaining ||
          wrote % sizeof(int16_t) != 0u) {
        /* The caller must finish cancellation and quiesce the source before
         * freeing any resource, even when the device failed. */
        (void)turbo_tts_playback_bridge_cancel(b);
        return TURBO_TTS_PLAYBACK_EDEVICE;
      }
      b->written_offset += wrote;
      *out_written += wrote;
      if (b->written_offset < b->pending_bytes) {
        b->state = TURBO_TTS_PLAYBACK_BACKPRESSURE;
        return TURBO_TTS_PLAYBACK_OK;
      }
      clear_staging(b);
      b->state = TURBO_TTS_PLAYBACK_STREAMING;
    }
    if (polls >= poll_budget) return TURBO_TTS_PLAYBACK_OK;

    size_t count = 0u;
    turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
    int rc = turbo_tts_poll(b->source, b->staging,
                            TURBO_TTS_SAMPLES_PER_FRAME, &count, &kind);
    polls++;
    if (rc != TURBO_TTS_OK) {
      clear_staging(b);
      (void)turbo_tts_playback_bridge_cancel(b);
      return TURBO_TTS_PLAYBACK_EPROVIDER;
    }
    if (kind == TURBO_TTS_WOULD_BLOCK) {
      clear_staging(b);
      return TURBO_TTS_PLAYBACK_OK;
    }
    if (kind == TURBO_TTS_TERMINAL) {
      clear_staging(b);
      b->state = TURBO_TTS_PLAYBACK_SOURCE_DONE;
      return TURBO_TTS_PLAYBACK_OK;
    }
    if (kind != TURBO_TTS_PCM || !count || count > TURBO_TTS_SAMPLES_PER_FRAME) {
      clear_staging(b);
      (void)turbo_tts_playback_bridge_cancel(b);
      return TURBO_TTS_PLAYBACK_EPROVIDER;
    }
    b->pending_bytes = count * sizeof(int16_t);
    b->written_offset = 0u;
  }
}

int turbo_tts_playback_bridge_cancel(turbo_tts_playback_bridge_t *b) {
  if (!b) return TURBO_TTS_PLAYBACK_EINVAL;
  if (b->state == TURBO_TTS_PLAYBACK_CANCELED) return TURBO_TTS_PLAYBACK_OK;
  if (b->state == TURBO_TTS_PLAYBACK_FAILED) return TURBO_TTS_PLAYBACK_EBUSY;
  if (b->state != TURBO_TTS_PLAYBACK_CANCEL_PENDING) {
    /* Single threaded control owner means no concurrent bridge write here. */
    if (turbo_tts_state(b->source) == TURBO_TTS_ACTIVE &&
        turbo_tts_cancel(b->source) != TURBO_TTS_OK)
      return TURBO_TTS_PLAYBACK_EPROVIDER;
    clear_staging(b);
    b->clear_needed = 1;
    b->state = TURBO_TTS_PLAYBACK_CANCEL_PENDING;
  }
  /* clear() cannot race the single producer: this function IS that owner. */
  if (b->clear_needed) {
    if (salts_playback_clear(b->playback) != SALTS_PLAYBACK_OK)
      return TURBO_TTS_PLAYBACK_EDEVICE;
    b->clear_needed = 0;
  }
  return TURBO_TTS_PLAYBACK_OK;
}

int turbo_tts_playback_bridge_drain(turbo_tts_playback_bridge_t *b,
                                    uint32_t timeout_ms) {
  if (!b || !timeout_ms) return TURBO_TTS_PLAYBACK_EINVAL;
  if (b->state != TURBO_TTS_PLAYBACK_SOURCE_DONE ||
      b->pending_bytes != 0u)
    return TURBO_TTS_PLAYBACK_EBUSY;
  int rc = salts_playback_drain(b->playback, timeout_ms);
  if (rc == SALTS_PLAYBACK_OK) {
    b->state = TURBO_TTS_PLAYBACK_DRAINED;
    return TURBO_TTS_PLAYBACK_OK;
  }
  if (rc == SALTS_PLAYBACK_ERR_TIMEOUT || rc == SALTS_PLAYBACK_ERR_BUSY)
    return TURBO_TTS_PLAYBACK_EBUSY;
  return TURBO_TTS_PLAYBACK_EDEVICE;
}

turbo_tts_playback_bridge_state_t
turbo_tts_playback_bridge_get_state(const turbo_tts_playback_bridge_t *b) {
  return b ? b->state : TURBO_TTS_PLAYBACK_FAILED;
}

int turbo_tts_playback_bridge_destroy(turbo_tts_playback_bridge_t **handle) {
  if (!handle || !*handle) return TURBO_TTS_PLAYBACK_EINVAL;
  turbo_tts_playback_bridge_t *b = *handle;
  if ((b->state != TURBO_TTS_PLAYBACK_DRAINED &&
       b->state != TURBO_TTS_PLAYBACK_CANCELED &&
       b->state != TURBO_TTS_PLAYBACK_FAILED) ||
      b->clear_needed || !source_is_settled(b->source))
    return TURBO_TTS_PLAYBACK_EBUSY;
  clear_staging(b);
  memset(b, 0, sizeof(*b));
  free(b);
  *handle = NULL;
  return TURBO_TTS_PLAYBACK_OK;
}
