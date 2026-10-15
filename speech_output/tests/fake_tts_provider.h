#ifndef TURBO_AGENT_FAKE_TTS_PROVIDER_H
#define TURBO_AGENT_FAKE_TTS_PROVIDER_H

#include "turbo_agent_speech_output.h"
#include <stdint.h>

typedef struct fake_tts_s {
  uint64_t generation_id;
  uint32_t seed;
  uint32_t generated_frames;
  int active;
  int canceled;
  int finish_emitted;
  int begin_calls;
  int poll_calls;
  int cancel_calls;
  int would_block_count;
  int delayed_cancel_polls;
  int emit_late_pcm_after_cancel;
  int begin_error;
  int fail_poll_on_call;
  int invalid_pcm_on_call;
  int force_source_done;
  const char *borrowed_text;
  const char *borrowed_voice;
  const char *borrowed_language;
  size_t borrowed_length;
} fake_tts_t;

void fake_tts_provider_init(fake_tts_t *fake,
                            turbo_agent_tts_provider_v1_t *callbacks);

#endif
