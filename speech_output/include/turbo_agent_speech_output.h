#ifndef TURBO_AGENT_SPEECH_OUTPUT_H
#define TURBO_AGENT_SPEECH_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental opt-in C11 SpeechOutput ABI. No model/device/Agent dependency.
 * All mutating calls use one serialized control owner; Provider callbacks must
 * not reenter. A Provider context is BORROWED and must outlive the output.
 * All user-facing speech must be explicitly approved by the host. */
#define TURBO_TTS_ABI_VERSION 1u
#define TURBO_TTS_SAMPLE_RATE_HZ 24000u
#define TURBO_TTS_SAMPLES_PER_FRAME 480u /* 20 ms S16 mono */
#define TURBO_TTS_MAX_TEXT_BYTES 4096u
#define TURBO_TTS_MAX_FRAMES 1500u      /* 30 s at 20 ms per PCM poll */
#define TURBO_TTS_MAX_VOICE_BYTES 64u
#define TURBO_TTS_MAX_LANGUAGE_BYTES 16u

typedef struct turbo_tts_output turbo_tts_output_t;

typedef enum turbo_tts_result_e {
  TURBO_TTS_OK = 0,
  TURBO_TTS_EINVAL = -1,
  TURBO_TTS_ENOMEM = -2,
  TURBO_TTS_ESTATE = -3,
  TURBO_TTS_ESTALE = -4,
  TURBO_TTS_EPROVIDER = -5,
  TURBO_TTS_ECAPACITY = -6,
  TURBO_TTS_EBUSY = -7
} turbo_tts_result_t;

typedef enum turbo_tts_state_e {
  TURBO_TTS_IDLE = 0,
  TURBO_TTS_ACTIVE = 1,
  TURBO_TTS_CANCEL_PENDING = 2,
  TURBO_TTS_SOURCE_DONE = 3, /* Synth completed; playback may still buffer */
  TURBO_TTS_CANCELED = 4,    /* Provider has explicitly settled */
  TURBO_TTS_FAILED = 5      /* Provider has explicitly settled after error */
} turbo_tts_state_t;

typedef enum turbo_tts_poll_kind_e {
  TURBO_TTS_WOULD_BLOCK = 0,
  TURBO_TTS_PCM = 1,
  TURBO_TTS_TERMINAL = 2,
  TURBO_TTS_POLL_CANCELED = 3
} turbo_tts_poll_t;

typedef struct turbo_tts_format_s {
  uint32_t sample_rate_hz;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t frame_samples;
} turbo_tts_format_t;

typedef struct turbo_tts_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_tts_format_t format; /* M4.0 only 24000/1/S16/480 */
  uint32_t max_text_bytes;   /* 1..4096 copied at create */
  uint32_t max_frames;       /* 1..1500 20ms blocks */
} turbo_tts_config_t;

/* begin text/voice/language/format are borrowed only for this Generation;
 * copied/retained by SpeechOutput until terminal/settle. Provider may borrow
 * them until the returned terminal condition, never beyond.
 * poll_pcm receives caller-owned EXACTLY one frame-capacity destination.
 * WOULD_BLOCK has no PCM, PCM has 1..480 samples (short tail allowed),
 * TERMINAL and POLL_CANCELED have no PCM and certify no in-flight callbacks.
 * request_cancel must be nonblocking/idempotent, but DOES NOT certify settle;
 * host must poll until CANCELED/FAILED before destroying or unloading DSO. */
typedef struct turbo_tts_provider_s {
  size_t struct_size;
  uint32_t abi_version;
  void *context;
  int (*begin)(void *context, uint64_t generation_id,
               const char *approved_utf8, size_t text_bytes,
               const char *voice_id, const char *language,
               const turbo_tts_format_t *format);
  int (*poll_pcm)(void *context, uint64_t generation_id,
                  int16_t *dst_s16, size_t capacity_samples,
                  size_t *out_samples, turbo_tts_poll_t *out_kind);
  void (*request_cancel)(void *context, uint64_t generation_id);
} turbo_tts_provider_t;

void turbo_tts_config_init(turbo_tts_config_t *config);
void turbo_tts_provider_init(turbo_tts_provider_t *provider);
int turbo_tts_create(const turbo_tts_config_t *config,
                     const turbo_tts_provider_t *provider,
                     turbo_tts_output_t **out_output);

/* One generation at a time. Input MUST already be approved user-visible
 * plain text, never a speculative model fragment or tool/Agent command.
 * Explicit voice/language labels are ASCII, length-limited and host-chosen.
 * No SSML/markup, network fetching or automatic voice/provider fallback. */
int turbo_tts_begin(turbo_tts_output_t *output, uint64_t generation_id,
                    const char *approved_utf8, size_t text_bytes,
                    const char *voice_id, const char *language);

/* Only call again after the previously produced PCM has been fully accepted
 * by the host's single writer. This call never interacts with Playback.
 * All outputs are cleared before invocation; failed/terminal PCM is zeroed.
 * A canceled generation may internally receive and DISCARD late PCM until
 * the Provider reports POLL_CANCELED or TERMINAL (settled). */
int turbo_tts_poll(turbo_tts_output_t *output,
                   int16_t *dst_s16, size_t capacity_samples,
                   size_t *out_samples, turbo_tts_poll_t *out_kind);

/* Does not force-stop noncooperative inference. Caller must pump until
 * CANCELED; while CANCEL_PENDING, destroy returns EBUSY and the borrowed
 * Provider/DSO must stay loaded. Repeated cancel is idempotent. */
int turbo_tts_cancel(turbo_tts_output_t *output);
turbo_tts_state_t turbo_tts_state(const turbo_tts_output_t *output);

/* Returns EBUSY and preserves the handle if active, pending or in callback.
 * The host must quiesce Playback's sole producer before its clear/destroy,
 * and must independently drain Playback before reporting output played. */
int turbo_tts_destroy(turbo_tts_output_t **output);

#ifdef __cplusplus
}
#endif
#endif
