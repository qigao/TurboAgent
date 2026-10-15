#include "fake_tts_provider.h"

#include <stddef.h>
#include <string.h>

static uint32_t text_seed(const char *p, size_t n) {
  uint32_t h = UINT32_C(2166136261);
  for (size_t i = 0u; i < n; i++) {
    h ^= (unsigned char)p[i];
    h *= UINT32_C(16777619);
  }
  return h;
}

static int fake_begin(void *ctx, uint64_t generation_id,
                      const char *text, size_t text_len,
                      const char *voice, const char *lang,
                      const turbo_agent_tts_format_t *format) {
  fake_tts_t *f = (fake_tts_t *)ctx;
  f->begin_calls++;
  if (f->begin_error) return -1; /* Must start no worker on failure. */
  if (!text || !voice || !lang || !format || generation_id == 0u ||
      format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_ms != 20u)
    return -1;
  f->generation_id = generation_id;
  f->seed = text_seed(text, text_len);
  f->borrowed_text = text;
  f->borrowed_voice = voice;
  f->borrowed_language = lang;
  f->borrowed_length = text_len;
  f->generated_frames = 0u;
  f->canceled = 0;
  f->active = 1;
  f->finish_emitted = 0;
  f->poll_calls = 0;
  return 0;
}

static void render(fake_tts_t *f, int16_t *dst, size_t samples) {
  for (size_t i = 0u; i < samples; i++) {
    uint32_t t = f->generated_frames * TURBO_AGENT_TTS_SAMPLES_PER_FRAME
                 + (uint32_t)i;
    dst[i] = (int16_t)((int32_t)((t * UINT32_C(31) + f->seed)
                                % UINT32_C(4095)) - 2047);
  }
}

static int fake_poll(void *ctx, uint64_t generation_id,
                     int16_t *dst, size_t capacity, size_t *out_samples,
                     turbo_agent_tts_poll_kind_t *out_kind) {
  fake_tts_t *f = (fake_tts_t *)ctx;
  size_t samples;
  if (!f->active || !dst || !out_samples || !out_kind ||
      generation_id != f->generation_id ||
      capacity != TURBO_AGENT_TTS_SAMPLES_PER_FRAME)
    return -1;
  *out_samples = 0u;
  *out_kind = TURBO_TTS_WOULD_BLOCK;
  f->poll_calls++;
  if (f->canceled) {
    if (f->emit_late_pcm_after_cancel) {
      f->emit_late_pcm_after_cancel = 0;
      render(f, dst, capacity);
      *out_samples = capacity;
      *out_kind = TURBO_TTS_PCM;
      return 0;
    }
    if (f->delayed_cancel_polls > 0) {
      f->delayed_cancel_polls--;
      return 0;
    }
    f->active = 0;
    *out_kind = TURBO_TTS_PROVIDER_CANCELED;
    return 0;
  }
  if (f->fail_poll_on_call == f->poll_calls) {
    /* A failed poll may still have live generation work; the host must
     * request cancellation and observe PROVIDER_CANCELED later. */
    return -1;
  }
  if (f->would_block_count > 0) {
    f->would_block_count--;
    return 0;
  }
  if (f->force_source_done) {
    f->active = 0;
    *out_kind = TURBO_TTS_SOURCE_DONE;
    return 0;
  }
  if (f->generated_frames > 2u) {
    f->active = 0;
    *out_kind = TURBO_TTS_SOURCE_DONE;
    return 0;
  }
  samples = f->generated_frames == 2u ? 137u : capacity;
  if (f->invalid_pcm_on_call == f->poll_calls) {
    *out_samples = capacity + 1u;
    *out_kind = TURBO_TTS_PCM;
    return 0;
  }
  render(f, dst, samples);
  f->generated_frames++;
  *out_samples = samples;
  *out_kind = f->generated_frames == 3u ?
      TURBO_TTS_PCM_FINAL : TURBO_TTS_PCM;
  if (*out_kind == TURBO_TTS_PCM_FINAL) {
    f->finish_emitted = 1;
    f->active = 0; /* this callback is source-final and already quiescent */
  }
  return 0;
}

static void fake_cancel(void *ctx, uint64_t generation_id) {
  fake_tts_t *f = (fake_tts_t *)ctx;
  f->cancel_calls++;
  if (f->generation_id == generation_id) {
    f->canceled = 1;
    f->active = 1; /* even a finished engine must report settlement */
  }
}

void fake_tts_provider_init(fake_tts_t *fake,
                            turbo_agent_tts_provider_v1_t *callbacks) {
  memset(fake, 0, sizeof(*fake));
  turbo_agent_tts_provider_v1_init(callbacks);
  callbacks->context = fake;
  callbacks->begin = fake_begin;
  callbacks->poll_pcm = fake_poll;
  callbacks->request_cancel = fake_cancel;
}
