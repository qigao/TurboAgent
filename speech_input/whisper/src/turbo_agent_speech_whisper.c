#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "turbo_agent_speech_whisper.h"

#include <cmeta_crypto.h>
#include <whisper.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define WHISPER_PCM_MAX_SAMPLES \
  ((size_t)TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES * \
   (size_t)TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME)

struct turbo_agent_speech_whisper_s {
  struct whisper_context *engine;
  uint8_t *verified_model; /* immutable exact hashed bytes, not a pathname reload */
  size_t model_bytes;
  float *pcm_f32; /* single owned, preallocated per-utterance model input */
  char *text; /* one result, borrowed only during synchronous finish() */
  size_t max_samples, used_samples, max_text;
  uint32_t cpu_threads;
  uint32_t max_decode_ms;
  uint64_t decode_deadline_ms;
  turbo_agent_speech_whisper_language_t language;
  uint64_t last_utterance, last_timestamp_us;
  int has_timestamp, active, has_nonzero_pcm, last_error;
  atomic_int abort_requested;
  atomic_int in_decode;
};

/* Monotonic deadline source, never a wall-clock or model-provided timestamp.
 * The device callback never invokes this (it remains pure bounded PCM). */
static uint64_t monotonic_milliseconds(void) {
#if defined(_WIN32)
  return (uint64_t)GetTickCount64();
#else
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t) != 0 || t.tv_sec < 0)
    return 0u;
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

static int absolute_model_path(const char *path) {
  size_t n = 0u;
  if (!path) return 0;
  while (path[n] && n <= 4096u) n++;
  if (n == 0u || n > 4096u) return 0;
#if defined(_WIN32)
  /* Deliberately reject relative/drive-relative and UNC paths in M3a. The
   * caller can pass an absolute local drive path after explicit approval. */
  return n >= 4u &&
         ((path[0] >= 'A' && path[0] <= 'Z') ||
          (path[0] >= 'a' && path[0] <= 'z')) &&
         path[1] == ':' && (path[2] == '\\' || path[2] == '/');
#else
  return path[0] == '/';
#endif
}

static int valid_language(turbo_agent_speech_whisper_language_t v) {
  return v == TURBO_AGENT_WHISPER_LANGUAGE_EN ||
         v == TURBO_AGENT_WHISPER_LANGUAGE_ZH ||
         v == TURBO_AGENT_WHISPER_LANGUAGE_JA ||
         v == TURBO_AGENT_WHISPER_LANGUAGE_AUTO;
}

static const char *language_name(turbo_agent_speech_whisper_language_t v) {
  switch (v) {
    case TURBO_AGENT_WHISPER_LANGUAGE_EN: return "en";
    case TURBO_AGENT_WHISPER_LANGUAGE_ZH: return "zh";
    case TURBO_AGENT_WHISPER_LANGUAGE_JA: return "ja";
    case TURBO_AGENT_WHISPER_LANGUAGE_AUTO: return "auto";
    default: return NULL;
  }
}

static void wipe_utterance(turbo_agent_speech_whisper_t *owner) {
  if (owner->pcm_f32) {
    cmeta_crypto_clear(owner->pcm_f32, owner->max_samples * sizeof(float));
  }
  if (owner->text) {
    cmeta_crypto_clear(owner->text, owner->max_text + 1u);
  }
  owner->used_samples = 0u;
  owner->has_nonzero_pcm = 0;
  owner->has_timestamp = 0;
  owner->last_timestamp_us = 0u;
  owner->decode_deadline_ms = 0u;
}

static void destroy_storage(turbo_agent_speech_whisper_t *owner) {
  if (!owner) return;
  if (owner->engine) {
    whisper_free(owner->engine);
    owner->engine = NULL;
  }
  wipe_utterance(owner);
  if (owner->verified_model) {
    cmeta_crypto_clear(owner->verified_model, owner->model_bytes);
  }
  free(owner->verified_model);
  free(owner->pcm_f32);
  free(owner->text);
  cmeta_crypto_clear(owner, sizeof(*owner));
  free(owner);
}

/* Hash and load a SINGLE file snapshot; never pass the path back to the
 * engine after hash qualification (which would create a TOCTOU window).
 * Retain this bounded verified image until model/context destruction. */
static int load_model_snapshot(const char *path, uint64_t max_bytes,
                               const uint8_t expected[32],
                               uint8_t **out_bytes, size_t *out_size) {
  FILE *file = NULL;
  cmeta_sha256_stream *sha = NULL;
  uint8_t *buffer = NULL;
  uint8_t actual[32] = {0};
  size_t size = 0u, offset = 0u;
  long end;
  int equal = 0;
  int error = TURBO_AGENT_WHISPER_EIO;

  *out_bytes = NULL;
  *out_size = 0u;
  file = fopen(path, "rb");
  if (!file) return TURBO_AGENT_WHISPER_EMODEL;
  if (fseek(file, 0, SEEK_END) != 0 ||
      (end = ftell(file)) <= 0 ||
      (uint64_t)end > max_bytes ||
      fseek(file, 0, SEEK_SET) != 0) {
    error = TURBO_AGENT_WHISPER_EMODEL;
    goto cleanup;
  }
  size = (size_t)end;
  buffer = (uint8_t *)malloc(size);
  if (!buffer) {
    error = TURBO_AGENT_SPEECH_ENOMEM;
    goto cleanup;
  }
  if (cmeta_sha256_stream_create(&sha) != 0 || !sha) {
    error = TURBO_AGENT_WHISPER_EENGINE;
    goto cleanup;
  }
  while (offset < size) {
    const size_t request = size - offset < 65536u ? size - offset : 65536u;
    const size_t amount = fread(buffer + offset, 1u, request, file);
    if (amount == 0u ||
        cmeta_sha256_stream_update(sha, buffer + offset, amount) != 0) {
      error = TURBO_AGENT_WHISPER_EIO;
      goto cleanup;
    }
    offset += amount;
  }
  /* A writer may have appended bytes; reject rather than hash a prefix. */
  if (fgetc(file) != EOF || ferror(file)) goto cleanup;
  if (cmeta_sha256_stream_finish(sha, actual) != 0 ||
      cmeta_crypto_equal(actual, expected, sizeof(actual), &equal) != 0) {
    error = TURBO_AGENT_WHISPER_EENGINE;
    goto cleanup;
  }
  if (!equal) {
    error = TURBO_AGENT_WHISPER_EINTEGRITY;
    goto cleanup;
  }
  *out_bytes = buffer;
  *out_size = size;
  buffer = NULL;
  error = TURBO_AGENT_SPEECH_OK;
cleanup:
  cmeta_crypto_clear(actual, sizeof(actual));
  cmeta_sha256_stream_destroy(sha);
  if (file) fclose(file);
  if (buffer) {
    cmeta_crypto_clear(buffer, size);
    free(buffer);
  }
  return error;
}

void turbo_agent_speech_whisper_config_init(
    turbo_agent_speech_whisper_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_SPEECH_WHISPER_ABI_VERSION;
  config->max_model_bytes = 128u * 1024u * 1024u;
  config->max_utterance_frames = 1500u;
  config->max_text_bytes = 1024u;
  config->cpu_threads = 2u;
  config->max_decode_ms = 30000u;
  config->language = TURBO_AGENT_WHISPER_LANGUAGE_AUTO;
}

int turbo_agent_speech_whisper_create(
    const turbo_agent_speech_whisper_config_t *config,
    turbo_agent_speech_whisper_t **out_whisper) {
  turbo_agent_speech_whisper_t *owner = NULL;
  struct whisper_context_params params;
  size_t n, bytes;
  int error;
  uint8_t nonzero = 0u;
  if (!out_whisper) return TURBO_AGENT_SPEECH_EINVAL;
  *out_whisper = NULL;
  if (!config || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_AGENT_SPEECH_WHISPER_ABI_VERSION ||
      !absolute_model_path(config->model_path) ||
      config->max_model_bytes == 0u ||
      config->max_model_bytes > TURBO_AGENT_SPEECH_WHISPER_MAX_MODEL_BYTES ||
      config->max_utterance_frames == 0u ||
      config->max_utterance_frames > TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES ||
      config->max_text_bytes == 0u ||
      config->max_text_bytes > TURBO_AGENT_SPEECH_MAX_TEXT_BYTES ||
      config->cpu_threads == 0u ||
      config->cpu_threads > TURBO_AGENT_SPEECH_WHISPER_MAX_THREADS ||
      config->max_decode_ms == 0u ||
      config->max_decode_ms > TURBO_AGENT_SPEECH_WHISPER_MAX_DECODE_MS ||
      !valid_language(config->language))
    return TURBO_AGENT_SPEECH_EINVAL;
  for (size_t i = 0u; i < sizeof(config->expected_sha256); i++) {
    nonzero |= config->expected_sha256[i];
  }
  if (!nonzero) return TURBO_AGENT_SPEECH_EINVAL;
  n = (size_t)config->max_utterance_frames *
      TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME;
  if (n == 0u || n > WHISPER_PCM_MAX_SAMPLES || n > (size_t)INT_MAX)
    return TURBO_AGENT_SPEECH_EINVAL;
  bytes = n * sizeof(float);
  owner = (turbo_agent_speech_whisper_t *)calloc(1u, sizeof(*owner));
  if (!owner) return TURBO_AGENT_SPEECH_ENOMEM;
  owner->max_samples = n;
  owner->max_text = (size_t)config->max_text_bytes;
  owner->cpu_threads = config->cpu_threads;
  owner->max_decode_ms = config->max_decode_ms;
  owner->language = config->language;
  atomic_init(&owner->abort_requested, 0);
  atomic_init(&owner->in_decode, 0);
  owner->pcm_f32 = (float *)calloc(1u, bytes);
  owner->text = (char *)calloc(owner->max_text + 1u, 1u);
  if (!owner->pcm_f32 || !owner->text) {
    error = TURBO_AGENT_SPEECH_ENOMEM;
    goto failure;
  }
  error = load_model_snapshot(config->model_path, config->max_model_bytes,
                              config->expected_sha256,
                              &owner->verified_model, &owner->model_bytes);
  if (error != TURBO_AGENT_SPEECH_OK) goto failure;
  params = whisper_context_default_params();
  params.use_gpu = false;
  params.flash_attn = false;
  params.dtw_token_timestamps = false;
  owner->engine = whisper_init_from_buffer_with_params(
      owner->verified_model, owner->model_bytes, params);
  if (!owner->engine) {
    error = TURBO_AGENT_WHISPER_EMODEL;
    goto failure;
  }
  *out_whisper = owner;
  return TURBO_AGENT_SPEECH_OK;
failure:
  destroy_storage(owner);
  return error;
}

static int provider_begin(void *context, uint64_t utterance_id) {
  turbo_agent_speech_whisper_t *owner = (turbo_agent_speech_whisper_t *)context;
  if (!owner || utterance_id == 0u || owner->active ||
      atomic_load_explicit(&owner->in_decode, memory_order_acquire))
    return -1;
  if (utterance_id <= owner->last_utterance) {
    owner->last_error = TURBO_AGENT_SPEECH_ESTALE;
    return -1;
  }
  wipe_utterance(owner);
  owner->last_utterance = utterance_id;
  owner->active = 1;
  owner->last_error = TURBO_AGENT_SPEECH_OK;
  atomic_store_explicit(&owner->abort_requested, 0, memory_order_release);
  return 0;
}

static int provider_accept(void *context, const int16_t *pcm,
                           size_t samples, uint64_t timestamp_us,
                           turbo_agent_speech_transcript_t *out_partial) {
  turbo_agent_speech_whisper_t *owner = (turbo_agent_speech_whisper_t *)context;
  if (!owner || !owner->active || !out_partial || !pcm ||
      samples != TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME ||
      atomic_load_explicit(&owner->abort_requested, memory_order_acquire)) {
    if (owner) owner->last_error = TURBO_AGENT_WHISPER_EABORT;
    return -1;
  }
  if ((owner->has_timestamp && timestamp_us <= owner->last_timestamp_us) ||
      owner->used_samples > owner->max_samples - samples) {
    owner->last_error = TURBO_AGENT_SPEECH_ECAPACITY;
    return -1;
  }
  for (size_t i = 0u; i < samples; i++) {
    owner->pcm_f32[owner->used_samples + i] =
        (float)pcm[i] * (1.0f / 32768.0f);
    if (pcm[i] != 0) owner->has_nonzero_pcm = 1;
  }
  owner->used_samples += samples;
  owner->last_timestamp_us = timestamp_us;
  owner->has_timestamp = 1;
  *out_partial = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_NONE, NULL, 0u};
  return 0;
}

static bool whisper_abort_callback(void *context) {
  turbo_agent_speech_whisper_t *owner =
      (turbo_agent_speech_whisper_t *)context;
  uint64_t now;
  if (atomic_load_explicit(&owner->abort_requested, memory_order_acquire))
    return true;
  now = monotonic_milliseconds();
  if (now == 0u || now >= owner->decode_deadline_ms) {
    atomic_store_explicit(&owner->abort_requested, 1, memory_order_release);
    return true; /* decoder must settle before any Provider image unload */
  }
  return false;
}

static int provider_finish(void *context,
                           turbo_agent_speech_transcript_t *out_final) {
  turbo_agent_speech_whisper_t *owner = (turbo_agent_speech_whisper_t *)context;
  struct whisper_full_params params;
  size_t length = 0u;
  int rc, segments;
  if (!owner || !owner->active || !out_final) return -1;
  if (atomic_load_explicit(&owner->abort_requested, memory_order_acquire)) {
    owner->last_error = TURBO_AGENT_WHISPER_EABORT;
    return -1;
  }
  /* Digital zero PCM is a deterministic silence-negative fixture, NOT
   * acoustic VAD. Background noise hallucinations require separate testing. */
  if (owner->used_samples == 0u || !owner->has_nonzero_pcm) {
    wipe_utterance(owner);
    owner->active = 0;
    *out_final = (turbo_agent_speech_transcript_t){
        TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH, NULL, 0u};
    return 0;
  }
  params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = (int)owner->cpu_threads;
  params.translate = false;
  params.no_context = true;
  params.no_timestamps = true;
  params.print_special = false;
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.token_timestamps = false;
  params.language = language_name(owner->language);
  params.detect_language = false; /* AUTO means auto-select then transcribe */
  params.temperature_inc = 0.0f; /* no alternate decoding strategy fallback */
  params.vad = false; /* no externally supplied VAD model */
  params.abort_callback = whisper_abort_callback;
  params.abort_callback_user_data = owner;
  {
    const uint64_t now = monotonic_milliseconds();
    if (now == 0u || now > UINT64_MAX - (uint64_t)owner->max_decode_ms) {
      owner->last_error = TURBO_AGENT_WHISPER_EENGINE;
      return -1;
    }
    owner->decode_deadline_ms = now + (uint64_t)owner->max_decode_ms;
  }
  atomic_store_explicit(&owner->in_decode, 1, memory_order_release);
  rc = whisper_full(owner->engine, params, owner->pcm_f32,
                    (int)owner->used_samples);
  atomic_store_explicit(&owner->in_decode, 0, memory_order_release);
  if (atomic_load_explicit(&owner->abort_requested, memory_order_acquire)) {
    owner->last_error = TURBO_AGENT_WHISPER_EABORT;
    return -1; /* M1 invokes Provider cancel; do not publish partial text */
  }
  if (rc != 0) {
    owner->last_error = TURBO_AGENT_WHISPER_EENGINE;
    return -1;
  }
  segments = whisper_full_n_segments(owner->engine);
  if (segments <= 0) {
    owner->last_error = TURBO_AGENT_WHISPER_EENGINE;
    return -1; /* no implicit inference-to-NO_SPEECH fallback */
  }
  for (int i = 0; i < segments; i++) {
    const char *segment = whisper_full_get_segment_text(owner->engine, i);
    size_t n = 0u;
    if (!segment) {
      owner->last_error = TURBO_AGENT_WHISPER_EENGINE;
      return -1;
    }
    /* Check the size before copy; never truncate a Whisper segment into an
     * apparently successful final (and never read past the bounded scan). */
    while (n <= owner->max_text - length && segment[n] != '\0') n++;
    if (n > owner->max_text - length) {
      owner->last_error = TURBO_AGENT_SPEECH_ETEXT;
      return -1;
    }
    if (n) memcpy(owner->text + length, segment, n);
    length += n;
  }
  if (length == 0u) {
    owner->last_error = TURBO_AGENT_WHISPER_EENGINE;
    return -1;
  }
  owner->text[length] = '\0';
  cmeta_crypto_clear(owner->pcm_f32, owner->max_samples * sizeof(float));
  owner->used_samples = 0u;
  owner->active = 0;
  *out_final = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, owner->text, length};
  return 0;
}

static void provider_cancel(void *context) {
  turbo_agent_speech_whisper_t *owner = (turbo_agent_speech_whisper_t *)context;
  if (!owner) return;
  atomic_store_explicit(&owner->abort_requested, 1, memory_order_release);
  /* The host is serialized with all Provider callbacks here; abort() is the
   * only signal allowed concurrently with a blocking whisper_full call. */
  if (atomic_load_explicit(&owner->in_decode, memory_order_acquire))
    return;
  wipe_utterance(owner);
  owner->active = 0;
  if (owner->last_error == TURBO_AGENT_SPEECH_OK)
    owner->last_error = TURBO_AGENT_WHISPER_EABORT;
}

int turbo_agent_speech_whisper_provider(
    turbo_agent_speech_whisper_t *owner,
    turbo_agent_speech_provider_v1_t *out_provider) {
  if (!owner || !out_provider) return TURBO_AGENT_SPEECH_EINVAL;
  turbo_agent_speech_provider_v1_init(out_provider);
  out_provider->context = owner;
  out_provider->begin = provider_begin;
  out_provider->accept_frame = provider_accept;
  out_provider->finish = provider_finish;
  out_provider->cancel = provider_cancel;
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_whisper_request_abort(
    turbo_agent_speech_whisper_t *owner) {
  if (!owner) return TURBO_AGENT_SPEECH_EINVAL;
  atomic_store_explicit(&owner->abort_requested, 1, memory_order_release);
  return TURBO_AGENT_SPEECH_OK;
}

int turbo_agent_speech_whisper_last_error(
    const turbo_agent_speech_whisper_t *owner) {
  return owner ? owner->last_error : TURBO_AGENT_SPEECH_EINVAL;
}

int turbo_agent_speech_whisper_destroy(
    turbo_agent_speech_whisper_t **handle) {
  turbo_agent_speech_whisper_t *owner;
  if (!handle || !*handle) return TURBO_AGENT_SPEECH_EINVAL;
  owner = *handle;
  if (owner->active ||
      atomic_load_explicit(&owner->in_decode, memory_order_acquire))
    return TURBO_AGENT_SPEECH_EBUSY;
  destroy_storage(owner);
  *handle = NULL;
  return TURBO_AGENT_SPEECH_OK;
}
