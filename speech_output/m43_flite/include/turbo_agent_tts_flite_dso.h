#ifndef TURBO_AGENT_TTS_FLITE_DSO_H
#define TURBO_AGENT_TTS_FLITE_DSO_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_tts_flite_process.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M4.3 Linux-only borrowed Process Provider plugin ABI; NOT installed into
 * TurboAgent.Native and NEVER authorized by a model-free CI success.
 * The module is borrowed until ALL children are reaped, all associated
 * SpeechOutput and VoiceCoordinator handles are destroyed and only then
 * is the host permitted to dlclose the module. */
#define TURBO_M43_FLITE_DSO_ABI_VERSION 1u
typedef struct turbo_m43_flite_dso_api {
  size_t struct_size;
  uint32_t abi_version;
  void (*config_init)(turbo_m43_flite_config_t *);
  int (*create)(const turbo_m43_flite_config_t *,
                turbo_m43_flite_process_t **);
  int (*bind)(turbo_m43_flite_process_t *, turbo_tts_provider_t *);
  int (*destroy)(turbo_m43_flite_process_t **);
  /* Test-only synchronous dlclose witness. A production consumer must
   * pass NULL and must not use this for lifecycle synchronization. */
  void (*test_set_unload_counter)(unsigned *);
} turbo_m43_flite_dso_api_t;

typedef const turbo_m43_flite_dso_api_t *
(*turbo_m43_flite_dso_entry_fn)(void);

/* Exported by a separate, opt-in DSO. Not linked by the default application.
 * dlsym from C11 uses memcpy between identically sized pointer slots. */
#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const turbo_m43_flite_dso_api_t *turbo_m43_flite_dso_entry(void);

#ifdef __cplusplus
}
#endif
#endif
