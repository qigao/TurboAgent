#ifndef TURBO_AGENT_SPEECH_INPUT_H
#define TURBO_AGENT_SPEECH_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opt-in C11 input modality. No native audio, ASR inference, agent authority,
 * background worker or model dependency is created by this API. */
#define TURBO_AGENT_SPEECH_INPUT_ABI_VERSION 1u
#define TURBO_AGENT_SPEECH_SAMPLE_RATE_HZ 16000u
#define TURBO_AGENT_SPEECH_CHANNELS 1u
#define TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME 320u /* 20 ms S16 mono */
#define TURBO_AGENT_SPEECH_MAX_QUEUED_FRAMES 128u
#define TURBO_AGENT_SPEECH_MAX_TEXT_BYTES 4096u
#define TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES 3000u /* 60 s */

typedef struct turbo_agent_speech_input_s turbo_agent_speech_input_t;

typedef enum turbo_agent_speech_status_e {
  TURBO_AGENT_SPEECH_OK = 0,
  TURBO_AGENT_SPEECH_EINVAL = -1,
  TURBO_AGENT_SPEECH_ENOMEM = -2,
  TURBO_AGENT_SPEECH_EBUSY = -3,
  TURBO_AGENT_SPEECH_ECAPACITY = -4,
  TURBO_AGENT_SPEECH_ESTATE = -5,
  TURBO_AGENT_SPEECH_EPROVIDER = -6,
  TURBO_AGENT_SPEECH_ETEXT = -7,
  TURBO_AGENT_SPEECH_ESTALE = -8,
  TURBO_AGENT_SPEECH_EBUFFER = -9
} turbo_agent_speech_status_t;

typedef enum turbo_agent_speech_state_e {
  TURBO_AGENT_SPEECH_IDLE = 0,
  TURBO_AGENT_SPEECH_ACTIVE = 1,
  TURBO_AGENT_SPEECH_DRAINING = 2,
  TURBO_AGENT_SPEECH_FINAL = 3,
  TURBO_AGENT_SPEECH_NO_SPEECH = 4,
  TURBO_AGENT_SPEECH_CANCELED = 5,
  TURBO_AGENT_SPEECH_FAILED = 6
} turbo_agent_speech_state_t;

typedef enum turbo_agent_speech_transcript_kind_e {
  TURBO_AGENT_SPEECH_NONE = 0,
  TURBO_AGENT_SPEECH_PARTIAL = 1,
  TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL = 2,
  TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH = 3
} turbo_agent_speech_transcript_kind_t;

/* Provider text is borrowed only during its synchronous callback. The core
 * copies it after validating the exact UTF-8 byte count. */
typedef struct turbo_agent_speech_transcript_s {
  turbo_agent_speech_transcript_kind_t kind;
  const char *utf8;
  size_t length;
} turbo_agent_speech_transcript_t;

typedef struct turbo_agent_speech_provider_v1_s {
  size_t struct_size;
  uint32_t abi_version;
  void *context; /* borrowed: host retains until SpeechInput destroyed */
  int (*begin)(void *context, uint64_t utterance_id);
  int (*accept_frame)(void *context, const int16_t *pcm_s16,
                      size_t samples, uint64_t timestamp_us,
                      turbo_agent_speech_transcript_t *out_partial);
  int (*finish)(void *context, turbo_agent_speech_transcript_t *out_terminal);
  void (*cancel)(void *context); /* once per canceled/failed active generation */
} turbo_agent_speech_provider_v1_t;

typedef struct turbo_agent_speech_config_s {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t sample_rate_hz; /* 16000 only in M1 */
  uint32_t channels; /* 1 only in M1 */
  uint32_t bits_per_sample; /* signed 16-bit only in M1 */
  uint32_t samples_per_frame; /* 320 only in M1 */
  uint32_t queue_frames; /* 1..128, no dynamic growth */
  uint32_t max_utterance_frames; /* 1..3000, hard admission limit */
  uint32_t max_text_bytes; /* 1..4096 UTF-8 bytes */
} turbo_agent_speech_config_t;

void turbo_agent_speech_config_init(turbo_agent_speech_config_t *config);
void turbo_agent_speech_provider_v1_init(turbo_agent_speech_provider_v1_t *provider);

/* All allocation occurs at create. No allocation on microphone callbacks. */
int turbo_agent_speech_create(const turbo_agent_speech_config_t *config,
                              const turbo_agent_speech_provider_v1_t *provider,
                              turbo_agent_speech_input_t **out_input);

/* begin/pump/request_finish/cancel/read_partial/take_final/destroy are serialized
 * by the caller-owned control thread. Provider callbacks must not reenter.
 * Only offer_frame may execute concurrently on the capture producer thread. */
int turbo_agent_speech_begin(turbo_agent_speech_input_t *input,
                             uint64_t utterance_id);

/* For Salts::Capture's borrowed PCM callback: copies a full 20 ms frame
 * immediately under a try-lock, performs no ASR and never waits. EBUSY on
 * lock contention, ECAPACITY on a full queue, ESTALE for an old generation. */
int turbo_agent_speech_offer_frame(turbo_agent_speech_input_t *input,
                                   uint64_t utterance_id,
                                   const int16_t *pcm_s16,
                                   size_t samples,
                                   uint64_t timestamp_us);

/* Stop Salts::Capture and join its callback BEFORE this. No frames accepted
 * once finishing begins; otherwise no extra session/queue ownership is added. */
int turbo_agent_speech_request_finish(turbo_agent_speech_input_t *input);

/* Caller-driven Provider execution, entirely outside the native audio callback.
 * Processes at most frame_budget frames; finalizes when draining & empty.
 * Never submits text to AgentCompiler/Session/Harness. */
int turbo_agent_speech_pump(turbo_agent_speech_input_t *input,
                            size_t frame_budget, size_t *out_pumped);
int turbo_agent_speech_cancel(turbo_agent_speech_input_t *input);

/* PARTIAL is presentation-only, must never call tools or auto-submit a Session.
 * Copies one checked UTF-8 snapshot; revision increases on each update. */
int turbo_agent_speech_read_partial(turbo_agent_speech_input_t *input,
                                    char *dst, size_t dst_bytes,
                                    size_t *out_bytes, uint64_t *out_revision);

/* The sole FINAL/NO_SPEECH access. One successful take per utterance; too-small
 * destination leaves it unconsumed. Host explicitly decides whether finalized
 * untrusted text may enter existing Harness.start_text()/Session. */
int turbo_agent_speech_take_final(turbo_agent_speech_input_t *input,
                                  char *dst, size_t dst_bytes,
                                  size_t *out_bytes,
                                  turbo_agent_speech_transcript_kind_t *out_kind);

turbo_agent_speech_state_t turbo_agent_speech_get_state(
    turbo_agent_speech_input_t *input);

/* Never silently stop an active microphone/Provider. On EBUSY *input stays
 * valid; host must stop capture callbacks, cancel/drain and retry. */
int turbo_agent_speech_destroy(turbo_agent_speech_input_t **input);

#ifdef __cplusplus
}
#endif
#endif
