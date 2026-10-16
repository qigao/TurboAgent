#include "turbo_agent_speech_input.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct speech_slot_s {
  int16_t samples[TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME];
  uint64_t timestamp_us;
} speech_slot_t;

struct turbo_agent_speech_input_s {
  /* Queue and producer-observed state are protected by this one short lock.
   * The microphone callback does not spin or run Provider code. */
  atomic_flag queue_lock;
  turbo_agent_speech_state_t state;
  speech_slot_t *queue;
  size_t capacity, head, count, accepted;
  uint64_t utterance_id, last_timestamp_us;
  int has_timestamp;
  size_t max_frames, max_text;
  turbo_agent_speech_provider_v1_t provider;
  char *partial, *final;
  size_t partial_len, final_len;
  uint64_t revision;
  turbo_agent_speech_transcript_kind_t terminal_kind;
  int final_taken;
};

static int queue_try_lock(turbo_agent_speech_input_t *input) {
  return !atomic_flag_test_and_set_explicit(&input->queue_lock, memory_order_acquire);
}

static void control_lock(turbo_agent_speech_input_t *input) {
  while (!queue_try_lock(input)) { /* only a bounded PCM copy holds the lock */ }
}

static void queue_unlock(turbo_agent_speech_input_t *input) {
  atomic_flag_clear_explicit(&input->queue_lock, memory_order_release);
}

/* RFC 3629 scalar UTF-8; rejects embedded NUL, truncation, overlong
 * sequences, surrogate code points and values above U+10FFFF. */
static int valid_utf8(const char *s, size_t length) {
  size_t i = 0;
  const unsigned char *p = (const unsigned char *)s;
  if (!s && length) return 0;
  while (i < length) {
    unsigned char a = p[i++];
    unsigned extra;
    uint32_t cp;
    if (a == 0) return 0;
    if (a < 0x80) continue;
    if (a >= 0xc2 && a <= 0xdf) { extra = 1; cp = a & 0x1fu; }
    else if (a >= 0xe0 && a <= 0xef) { extra = 2; cp = a & 0x0fu; }
    else if (a >= 0xf0 && a <= 0xf4) { extra = 3; cp = a & 0x07u; }
    else return 0;
    if (length - i < extra) return 0;
    for (unsigned n = 0; n < extra; n++) {
      unsigned char b = p[i++];
      if ((b & 0xc0u) != 0x80u) return 0;
      cp = (cp << 6) | (b & 0x3fu);
    }
    if ((extra == 1 && cp < 0x80u) ||
        (extra == 2 && cp < 0x800u) ||
        (extra == 3 && cp < 0x10000u) ||
        (cp >= 0xd800u && cp <= 0xdfffu) || cp > 0x10ffffu) return 0;
  }
  return 1;
}

static int copy_transcript(turbo_agent_speech_input_t *input,
                           const turbo_agent_speech_transcript_t *value,
                           char *dst, size_t *out_len) {
  if (!value || (value->length != 0 && !value->utf8) ||
      value->length > input->max_text ||
      !valid_utf8(value->utf8, value->length)) return TURBO_AGENT_SPEECH_ETEXT;
  if (value->length) memcpy(dst, value->utf8, value->length);
  dst[value->length] = '\0';
  *out_len = value->length;
  return TURBO_AGENT_SPEECH_OK;
}

static void clear_queue_locked(turbo_agent_speech_input_t *input) {
  /* Raw PCM is not durable state; discard it after consumption/cancellation. */
  memset(input->queue, 0, input->capacity * sizeof(*input->queue));
  input->head = 0;
  input->count = 0;
}

static int fail_provider(turbo_agent_speech_input_t *input, int why) {
  control_lock(input);
  input->state = TURBO_AGENT_SPEECH_FAILED;
  clear_queue_locked(input);
  memset(input->partial, 0, input->max_text + 1u);
  memset(input->final, 0, input->max_text + 1u);
  input->partial_len = input->final_len = 0;
  queue_unlock(input);
  input->provider.cancel(input->provider.context);
  return why;
}

void turbo_agent_speech_config_init(turbo_agent_speech_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_SPEECH_INPUT_ABI_VERSION;
  config->sample_rate_hz = TURBO_AGENT_SPEECH_SAMPLE_RATE_HZ;
  config->channels = TURBO_AGENT_SPEECH_CHANNELS;
  config->bits_per_sample = 16u;
  config->samples_per_frame = TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME;
  config->queue_frames = 16u;
  config->max_utterance_frames = 1500u; /* 30 s */
  config->max_text_bytes = 1024u;
}

void turbo_agent_speech_provider_v1_init(
    turbo_agent_speech_provider_v1_t *provider) {
  if (!provider) return;
  memset(provider, 0, sizeof(*provider));
  provider->struct_size = sizeof(*provider);
  provider->abi_version = TURBO_AGENT_SPEECH_INPUT_ABI_VERSION;
}

int turbo_agent_speech_create(const turbo_agent_speech_config_t *config,
                              const turbo_agent_speech_provider_v1_t *provider,
                              turbo_agent_speech_input_t **out_input) {
  turbo_agent_speech_input_t *input;
  if (!out_input) return TURBO_AGENT_SPEECH_EINVAL;
  *out_input = NULL;
  if (!config || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_AGENT_SPEECH_INPUT_ABI_VERSION ||
      config->sample_rate_hz != TURBO_AGENT_SPEECH_SAMPLE_RATE_HZ ||
      config->channels != TURBO_AGENT_SPEECH_CHANNELS ||
      config->bits_per_sample != 16u ||
      config->samples_per_frame != TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME ||
      config->queue_frames < 1u ||
      config->queue_frames > TURBO_AGENT_SPEECH_MAX_QUEUED_FRAMES ||
      config->max_utterance_frames < 1u ||
      config->max_utterance_frames > TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES ||
      config->max_text_bytes < 1u ||
      config->max_text_bytes > TURBO_AGENT_SPEECH_MAX_TEXT_BYTES ||
      !provider || provider->struct_size != sizeof(*provider) ||
      provider->abi_version != TURBO_AGENT_SPEECH_INPUT_ABI_VERSION ||
      !provider->begin || !provider->accept_frame ||
      !provider->finish || !provider->cancel) return TURBO_AGENT_SPEECH_EINVAL;
  input = (turbo_agent_speech_input_t *)calloc(1, sizeof(*input));
  if (!input) return TURBO_AGENT_SPEECH_ENOMEM;
  input->capacity = config->queue_frames;
  input->max_frames = config->max_utterance_frames;
  input->max_text = config->max_text_bytes;
  input->provider = *provider; /* callback table copied, context borrowed */
  input->queue = (speech_slot_t *)calloc(input->capacity, sizeof(*input->queue));
  input->partial = (char *)calloc(input->max_text + 1u, 1u);
  input->final = (char *)calloc(input->max_text + 1u, 1u);
  if (!input->queue || !input->partial || !input->final) {
    free(input->queue);
    free(input->partial);
    free(input->final);
    free(input);
    return TURBO_AGENT_SPEECH_ENOMEM;
  }
  atomic_flag_clear(&input->queue_lock);
  input->state = TURBO_AGENT_SPEECH_IDLE;
  *out_input = input;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_begin(turbo_agent_speech_input_t *input,
                             uint64_t utterance_id) {
  if (!input || utterance_id == 0u) return TURBO_AGENT_SPEECH_EINVAL;
  control_lock(input);
  if (input->state == TURBO_AGENT_SPEECH_ACTIVE ||
      input->state == TURBO_AGENT_SPEECH_DRAINING) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_EBUSY;
  }
  /* Never reissue an ID: stale capture callbacks from N cannot enter N+1. */
  if (utterance_id <= input->utterance_id) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTALE;
  }
  clear_queue_locked(input);
  input->accepted = 0;
  input->utterance_id = utterance_id;
  input->has_timestamp = 0;
  input->partial_len = input->final_len = 0;
  input->partial[0] = input->final[0] = '\0';
  input->revision = 0;
  input->final_taken = 0;
  input->terminal_kind = TURBO_AGENT_SPEECH_NONE;
  input->state = TURBO_AGENT_SPEECH_IDLE;
  queue_unlock(input);
  if (input->provider.begin(input->provider.context, utterance_id) != 0)
    return fail_provider(input, TURBO_AGENT_SPEECH_EPROVIDER);
  control_lock(input);
  input->state = TURBO_AGENT_SPEECH_ACTIVE;
  queue_unlock(input);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_offer_frame(turbo_agent_speech_input_t *input,
                                   uint64_t utterance_id,
                                   const int16_t *pcm_s16,
                                   size_t samples,
                                   uint64_t timestamp_us) {
  speech_slot_t *slot;
  if (!input || !pcm_s16 ||
      samples != TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME ||
      utterance_id == 0u) return TURBO_AGENT_SPEECH_EINVAL;
  if (!queue_try_lock(input)) return TURBO_AGENT_SPEECH_EBUSY;
  if (utterance_id != input->utterance_id) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTALE;
  }
  if (input->state != TURBO_AGENT_SPEECH_ACTIVE) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTATE;
  }
  if (input->has_timestamp && timestamp_us <= input->last_timestamp_us) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_EINVAL;
  }
  if (input->count == input->capacity || input->accepted >= input->max_frames) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ECAPACITY;
  }
  slot = &input->queue[(input->head + input->count) % input->capacity];
  memcpy(slot->samples, pcm_s16, sizeof(slot->samples));
  slot->timestamp_us = timestamp_us;
  input->count++;
  input->accepted++;
  input->has_timestamp = 1;
  input->last_timestamp_us = timestamp_us;
  queue_unlock(input);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_request_finish(turbo_agent_speech_input_t *input) {
  if (!input) return TURBO_AGENT_SPEECH_EINVAL;
  control_lock(input);
  if (input->state != TURBO_AGENT_SPEECH_ACTIVE) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTATE;
  }
  input->state = TURBO_AGENT_SPEECH_DRAINING;
  queue_unlock(input);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_pump(turbo_agent_speech_input_t *input,
                            size_t frame_budget, size_t *out_pumped) {
  size_t pumped = 0;
  int ending;
  if (!input || !out_pumped || frame_budget == 0u)
    return TURBO_AGENT_SPEECH_EINVAL;
  *out_pumped = 0;
  for (;;) {
    speech_slot_t frame;
    turbo_agent_speech_transcript_t result = {TURBO_AGENT_SPEECH_NONE, NULL, 0};
    int has_frame;
    int rc;
    control_lock(input);
    if (input->state != TURBO_AGENT_SPEECH_ACTIVE &&
        input->state != TURBO_AGENT_SPEECH_DRAINING) {
      queue_unlock(input);
      return TURBO_AGENT_SPEECH_ESTATE;
    }
    has_frame = input->count != 0u && pumped < frame_budget;
    ending = input->state == TURBO_AGENT_SPEECH_DRAINING &&
             input->count == 0u;
    if (has_frame) {
      frame = input->queue[input->head];
      memset(&input->queue[input->head], 0, sizeof(*input->queue));
      input->head = (input->head + 1u) % input->capacity;
      input->count--;
    }
    queue_unlock(input);
    if (!has_frame) {
      if (ending) {
        rc = input->provider.finish(input->provider.context, &result);
        if (rc != 0) return fail_provider(input, TURBO_AGENT_SPEECH_EPROVIDER);
        if (result.kind != TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
            result.kind != TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH)
          return fail_provider(input, TURBO_AGENT_SPEECH_ETEXT);
        if (result.kind == TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH &&
            (result.utf8 != NULL || result.length != 0))
          return fail_provider(input, TURBO_AGENT_SPEECH_ETEXT);
        if (result.kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
            result.length == 0u)
          return fail_provider(input, TURBO_AGENT_SPEECH_ETEXT);
        rc = copy_transcript(input, &result, input->final, &input->final_len);
        if (rc != TURBO_AGENT_SPEECH_OK) return fail_provider(input, rc);
        control_lock(input);
        input->terminal_kind = result.kind;
        input->state = result.kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL
                           ? TURBO_AGENT_SPEECH_FINAL : TURBO_AGENT_SPEECH_NO_SPEECH;
        input->partial[0] = '\0';
        input->partial_len = 0;
        queue_unlock(input);
      }
      *out_pumped = pumped;
      return TURBO_AGENT_SPEECH_OK;
    }
    rc = input->provider.accept_frame(input->provider.context, frame.samples,
                                      TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME,
                                      frame.timestamp_us, &result);
    memset(&frame, 0, sizeof(frame));
    if (rc != 0) return fail_provider(input, TURBO_AGENT_SPEECH_EPROVIDER);
    if (result.kind == TURBO_AGENT_SPEECH_PARTIAL) {
      rc = copy_transcript(input, &result, input->partial, &input->partial_len);
      if (rc != TURBO_AGENT_SPEECH_OK) return fail_provider(input, rc);
      input->revision++;
    } else if (result.kind != TURBO_AGENT_SPEECH_NONE ||
               result.utf8 != NULL || result.length != 0u) {
      return fail_provider(input, TURBO_AGENT_SPEECH_ETEXT);
    }
    pumped++;
    /* The budget strictly bounds Provider work in one caller-driven pump. */
    if (pumped == frame_budget) {
      *out_pumped = pumped;
      return TURBO_AGENT_SPEECH_OK;
    }
  }
}

int turbo_agent_speech_cancel(turbo_agent_speech_input_t *input) {
  if (!input) return TURBO_AGENT_SPEECH_EINVAL;
  control_lock(input);
  if (input->state != TURBO_AGENT_SPEECH_ACTIVE &&
      input->state != TURBO_AGENT_SPEECH_DRAINING) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTATE;
  }
  input->state = TURBO_AGENT_SPEECH_CANCELED;
  clear_queue_locked(input);
  input->partial_len = input->final_len = 0;
  input->partial[0] = input->final[0] = '\0';
  queue_unlock(input);
  input->provider.cancel(input->provider.context);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_read_partial(turbo_agent_speech_input_t *input,
                                    char *dst, size_t dst_bytes,
                                    size_t *out_bytes, uint64_t *out_revision) {
  if (!input || !dst || !out_bytes || !out_revision)
    return TURBO_AGENT_SPEECH_EINVAL;
  control_lock(input);
  if (!input->revision ||
      (input->state != TURBO_AGENT_SPEECH_ACTIVE &&
       input->state != TURBO_AGENT_SPEECH_DRAINING)) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTATE;
  }
  *out_bytes = input->partial_len;
  *out_revision = input->revision;
  if (dst_bytes <= input->partial_len) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_EBUFFER;
  }
  memcpy(dst, input->partial, input->partial_len + 1u);
  queue_unlock(input);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_take_final(turbo_agent_speech_input_t *input,
                                  char *dst, size_t dst_bytes,
                                  size_t *out_bytes,
                                  turbo_agent_speech_transcript_kind_t *out_kind) {
  if (!input || !dst || !out_bytes || !out_kind)
    return TURBO_AGENT_SPEECH_EINVAL;
  control_lock(input);
  if ((input->state != TURBO_AGENT_SPEECH_FINAL &&
       input->state != TURBO_AGENT_SPEECH_NO_SPEECH) ||
      input->final_taken) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_ESTATE;
  }
  *out_bytes = input->final_len;
  *out_kind = input->terminal_kind;
  if (dst_bytes <= input->final_len) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_EBUFFER;
  }
  memcpy(dst, input->final, input->final_len + 1u);
  input->final_taken = 1;
  queue_unlock(input);
  return TURBO_AGENT_SPEECH_OK;
}

turbo_agent_speech_state_t turbo_agent_speech_get_state(
    turbo_agent_speech_input_t *input) {
  turbo_agent_speech_state_t result;
  if (!input) return TURBO_AGENT_SPEECH_FAILED;
  control_lock(input);
  result = input->state;
  queue_unlock(input);
  return result;
}

int turbo_agent_speech_destroy(turbo_agent_speech_input_t **handle) {
  turbo_agent_speech_input_t *input;
  if (!handle || !*handle) return TURBO_AGENT_SPEECH_EINVAL;
  input = *handle;
  control_lock(input);
  if (input->state == TURBO_AGENT_SPEECH_ACTIVE ||
      input->state == TURBO_AGENT_SPEECH_DRAINING) {
    queue_unlock(input);
    return TURBO_AGENT_SPEECH_EBUSY;
  }
  clear_queue_locked(input);
  memset(input->partial, 0, input->max_text + 1u);
  memset(input->final, 0, input->max_text + 1u);
  queue_unlock(input);
  free(input->queue);
  free(input->partial);
  free(input->final);
  free(input);
  *handle = NULL;
  return TURBO_AGENT_SPEECH_OK;
}
