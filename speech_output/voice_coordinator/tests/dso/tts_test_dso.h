#ifndef TURBO_AGENT_M42_TEST_DSO_H
#define TURBO_AGENT_M42_TEST_DSO_H

/* Linux-only test fixture, not a public Provider interface or SDK export. */
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "turbo_agent_speech_output.h"

#define TURBO_TTS_TEST_DSO_ABI 1u

typedef struct turbo_tts_test_dso turbo_tts_test_dso_t;
typedef struct turbo_tts_test_dso_counts {
  uint32_t worker_started;
  uint32_t worker_done;
  uint32_t provider_polls;
  uint32_t provider_cancel_requests;
  uint32_t pcm_polls;
  uint32_t late_pcm_polls;
} turbo_tts_test_dso_counts_t;

/* These functions execute in the actual dlopened C11 shared library.
 * The host owns the atomic unload observer until after dlclose returns. */
typedef struct turbo_tts_test_dso_api {
  size_t struct_size;
  uint32_t abi_version;
  void (*set_unload_observer)(atomic_uint *unloads);
  int (*create)(turbo_tts_test_dso_t **out_context);
  int (*configure_provider)(turbo_tts_test_dso_t *context,
                            turbo_tts_provider_t *out_provider);
  int (*grant_pcm)(turbo_tts_test_dso_t *context);
  int (*wait_worker_started)(turbo_tts_test_dso_t *context);
  int (*wait_pcm_ready)(turbo_tts_test_dso_t *context);
  int (*grant_settle)(turbo_tts_test_dso_t *context);
  int (*wait_worker_done)(turbo_tts_test_dso_t *context);
  int (*snapshot)(turbo_tts_test_dso_t *context,
                  turbo_tts_test_dso_counts_t *counts);
  /* Refuses destruction while worker is still in-flight. Joins exactly
   * once after terminal settle before releasing C11 threading objects. */
  int (*destroy)(turbo_tts_test_dso_t **context);
} turbo_tts_test_dso_api_t;

typedef const turbo_tts_test_dso_api_t *(*turbo_tts_test_dso_entry_t)(void);

#endif
