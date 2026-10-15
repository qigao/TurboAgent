#ifndef TURBO_AGENT_SPEECH_WHISPER_H
#define TURBO_AGENT_SPEECH_WHISPER_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_speech_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
  #if defined(TURBO_AGENT_SPEECH_WHISPER_BUILD)
    #define TURBO_AGENT_SPEECH_WHISPER_API __declspec(dllexport)
  #else
    #define TURBO_AGENT_SPEECH_WHISPER_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__)
  #define TURBO_AGENT_SPEECH_WHISPER_API __attribute__((visibility("default")))
#else
  #define TURBO_AGENT_SPEECH_WHISPER_API
#endif

/* The optional shared-provider interface stays C11 even though the separately
 * installed whisper.cpp + ggml CPU-only inference engine is implemented in C++. */
#define TURBO_AGENT_SPEECH_WHISPER_ABI_VERSION 1u
#define TURBO_AGENT_SPEECH_WHISPER_SHA256_BYTES 32u
#define TURBO_AGENT_SPEECH_WHISPER_MAX_MODEL_BYTES (1024u * 1024u * 1024u)
#define TURBO_AGENT_SPEECH_WHISPER_MAX_THREADS 16u

typedef struct turbo_agent_speech_whisper_s turbo_agent_speech_whisper_t;

typedef enum turbo_agent_speech_whisper_language_e {
  TURBO_AGENT_WHISPER_LANGUAGE_EN = 1,
  TURBO_AGENT_WHISPER_LANGUAGE_ZH = 2,
  TURBO_AGENT_WHISPER_LANGUAGE_JA = 3,
  TURBO_AGENT_WHISPER_LANGUAGE_AUTO = 4 /* caller explicitly selects AUTO */
} turbo_agent_speech_whisper_language_t;

typedef enum turbo_agent_speech_whisper_error_e {
  TURBO_AGENT_WHISPER_EMODEL = -201,
  TURBO_AGENT_WHISPER_EINTEGRITY = -202,
  TURBO_AGENT_WHISPER_EIO = -203,
  TURBO_AGENT_WHISPER_EENGINE = -204,
  TURBO_AGENT_WHISPER_EABORT = -205
} turbo_agent_speech_whisper_error_t;

typedef struct turbo_agent_speech_whisper_config_s {
  size_t struct_size;
  uint32_t abi_version;
  /* Absolute caller-authorized local file. The model's exact byte snapshot is
   * SHA256-verified before it is passed to the engine. No model download or
   * environment/default search is permitted. */
  const char *model_path;
  uint8_t expected_sha256[TURBO_AGENT_SPEECH_WHISPER_SHA256_BYTES];
  uint64_t max_model_bytes; /* 1..1 GiB; default 128 MiB, set explicitly for larger weights */
  uint32_t max_utterance_frames; /* 1..3000; <= M1 SpeechInput limit */
  uint32_t max_text_bytes; /* 1..4096; must fit M1 result copy contract */
  uint32_t cpu_threads; /* 1..16; no opportunistic GPU fallback */
  turbo_agent_speech_whisper_language_t language;
} turbo_agent_speech_whisper_config_t;

TURBO_AGENT_SPEECH_WHISPER_API void turbo_agent_speech_whisper_config_init(
    turbo_agent_speech_whisper_config_t *config);

/* Loads only an explicitly supplied local model with matching SHA256. One
 * preallocated float32 PCM buffer, one whisper context, one Provider Owner.
 * Model bytes are kept as an immutable verified snapshot to avoid a
 * hash-then-reopen TOCTOU; neither PCM nor transcripts are persisted. */
TURBO_AGENT_SPEECH_WHISPER_API int turbo_agent_speech_whisper_create(
    const turbo_agent_speech_whisper_config_t *config,
    turbo_agent_speech_whisper_t **out_whisper);

/* Copy the SpeechProviderV1 table and borrowed owner pointer. The host keeps
 * this Whisper Owner alive until its dependent SpeechInput and callback lease
 * have been completely drained. No second Session / execution authority. */
TURBO_AGENT_SPEECH_WHISPER_API int turbo_agent_speech_whisper_provider(
    turbo_agent_speech_whisper_t *owner,
    turbo_agent_speech_provider_v1_t *out_provider);

/* Only cancellation signal allowed from another thread. The owner remains
 * alive; it is forbidden to free a decoding Whisper context until the
 * synchronous Provider finish() returns, callback settled. */
TURBO_AGENT_SPEECH_WHISPER_API int turbo_agent_speech_whisper_request_abort(
    turbo_agent_speech_whisper_t *owner);

/* The last explicit error (including hash mismatch and abort). ProviderV1
 * surfaces generic EPROVIDER; this value is the optional model diagnostic.
 * Control-plane serialized with all Provider callbacks. */
TURBO_AGENT_SPEECH_WHISPER_API int turbo_agent_speech_whisper_last_error(
    const turbo_agent_speech_whisper_t *owner);

/* Fail-closed busy if a Provider utterance is still active or a whisper_full
 * callback is running; do not dlclose/LoadLibrary-unload the image before
 * dependent SpeechInput and any DSO lease have fully settled. */
TURBO_AGENT_SPEECH_WHISPER_API int turbo_agent_speech_whisper_destroy(
    turbo_agent_speech_whisper_t **owner);

#ifdef __cplusplus
}
#endif
#endif
