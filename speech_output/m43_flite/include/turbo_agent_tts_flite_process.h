#ifndef TURBO_AGENT_TTS_FLITE_PROCESS_H
#define TURBO_AGENT_TTS_FLITE_PROCESS_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_speech_output.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M4.3 Linux/POSIX-only opt-in experiment. No vendor, model, microphone,
 * playback or cloud dependency in the host process. The host selects one
 * exact absolute executable path; there is no PATH lookup or fallback.
 * The helper is a separately reviewed Flite 2.2 + explicitly approved voice.
 * All methods remain serialized on a single owner, not an audio callback. */
#define TURBO_M43_FLITE_ABI_VERSION 1u
#define TURBO_M43_FLITE_MAX_TEXT_BYTES 128u
#define TURBO_M43_FLITE_VOICE_ID "cmu_us_kal16"
#define TURBO_M43_FLITE_LANGUAGE "en"

typedef struct turbo_m43_flite_process turbo_m43_flite_process_t;

typedef struct turbo_m43_flite_config {
  size_t struct_size;
  uint32_t abi_version;
  const char *helper_absolute_path; /* borrowed only during create */
  uint32_t max_text_bytes;          /* 1..128; must not exceed TTS config */
  uint32_t max_wall_ms;             /* 1..30000; one generation hard wall */
  uint32_t cancel_grace_ms;         /* 1..1000; then SIGKILL */
} turbo_m43_flite_config_t;

void turbo_m43_flite_config_init(turbo_m43_flite_config_t *config);
int turbo_m43_flite_create(const turbo_m43_flite_config_t *config,
                           turbo_m43_flite_process_t **out);
/* The borrowed provider context must outlive the returned SpeechOutput.
 * The provider is NOT an owner of Salts Playback or native audio devices. */
int turbo_m43_flite_bind(turbo_m43_flite_process_t *ctx,
                         turbo_tts_provider_t *provider);
/* EBUSY while a child is unreaped. Never closes/unloads a running engine. */
int turbo_m43_flite_destroy(turbo_m43_flite_process_t **ctx);

#ifdef __cplusplus
}
#endif
#endif
