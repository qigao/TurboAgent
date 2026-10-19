#ifndef TURBO_AGENT_SPEECH_OUTPUT_H
#define TURBO_AGENT_SPEECH_OUTPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M4.0 opt-in C11 TTS OUTPUT. Not an ASR Provider or native device owner.
 * This header has no dependencies on Salts, Agent, the network or TTS models.
 * The host alone chooses approved displayable text and one voice/provider.
 * Every mutating function is caller-control-thread serialized. */
#define TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION 1u
#define TURBO_AGENT_TTS_RATE_HZ 24000u
#define TURBO_AGENT_TTS_CHANNELS 1u
#define TURBO_AGENT_TTS_BITS_PER_SAMPLE 16u
#define TURBO_AGENT_TTS_FRAME_MS 20u
#define TURBO_AGENT_TTS_SAMPLES_PER_FRAME 480u
#define TURBO_AGENT_TTS_MAX_TEXT_BYTES 4096u
#define TURBO_AGENT_TTS_MAX_VOICE_BYTES 63u
#define TURBO_AGENT_TTS_MAX_AUDIO_MS 30000u

typedef struct turbo_agent_speech_output_s turbo_agent_speech_output_t;

typedef enum turbo_agent_tts_result_e {
  TURBO_TTS_OK = 0,
  TURBO_TTS_EINVAL = -1,
  TURBO_TTS_ENOMEM = -2,
  TURBO_TTS_EBUSY = -3,
  TURBO_TTS_ECAPACITY = -4,
  TURBO_TTS_ESTATE = -5,
  TURBO_TTS_EPROVIDER = -6,
  TURBO_TTS_ETEXT = -7,
  TURBO_TTS_ESTALE = -8,
  TURBO_TTS_EBUFFER = -9,
  TURBO_TTS_EAGAIN = -10,
  TURBO_TTS_EUNSUPPORTED = -11
} turbo_agent_tts_result_t;

typedef enum turbo_agent_tts_state_e {
  TURBO_TTS_IDLE = 0,
  TURBO_TTS_SYNTHESIZING = 1,
  /* Final Provider PCM still staged for the host writer. NOT device playback. */
  TURBO_TTS_STAGING_DRAINING = 2,
  TURBO_TTS_CANCEL_PENDING = 3,
  TURBO_TTS_SETTLED = 4,
  TURBO_TTS_CANCELED = 5,
  TURBO_TTS_FAILED = 6
} turbo_agent_tts_state_t;

typedef enum turbo_agent_tts_poll_kind_e {
  TURBO_TTS_WOULD_BLOCK = 0,
  TURBO_TTS_PCM = 1,         /* exactly 480 output samples */
  TURBO_TTS_PCM_FINAL = 2,   /* 1..480 final complete S16 samples */
  TURBO_TTS_SOURCE_DONE = 3, /* no more PCM; host staging may still remain */
  TURBO_TTS_PROVIDER_CANCELED = 4, /* worker fully settled */
  TURBO_TTS_PROVIDER_FAILED = 5    /* worker fully settled */
} turbo_agent_tts_poll_kind_t;

typedef struct turbo_agent_tts_format_s {
  uint32_t sample_rate_hz;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint32_t frame_ms;
} turbo_agent_tts_format_t;

/* Provider callbacks may neither retain the dst buffer nor call Playback,
 * Agent, Session, network or tools. All callbacks occur on the host control
 * owner, never in the device real-time callback.
 *
 * begin's text/voice/language pointers are borrowed from one copied
 * generation-scoped host allocation; Provider must NOT retain beyond terminal.
 * A nonzero begin return guarantees no worker/callback was started, so that
 * host cleanup is safe. poll_pcm returns 0 to deliver a checked kind; a
 * nonzero return starts explicit cancellation/settlement before unload.
 *
 * request_cancel is idempotent and may initiate async settlement, but it
 * does NOT on its own prove the provider's worker has stopped. The host must
 * poll to PROVIDER_CANCELED/SOURCE_DONE/PROVIDER_FAILED before destruction. */
typedef struct turbo_agent_tts_provider_v1_s {
  size_t struct_size;
  uint32_t abi_version;
  void *context; /* borrowed; caller keeps alive through destroy */
  int (*begin)(void *context, uint64_t generation_id,
               const char *approved_utf8, size_t text_bytes,
               const char *voice_id, const char *language,
               const turbo_agent_tts_format_t *format);
  int (*poll_pcm)(void *context, uint64_t generation_id,
                  int16_t *dst_s16, size_t capacity_samples,
                  size_t *out_samples, turbo_agent_tts_poll_kind_t *out_kind);
  void (*request_cancel)(void *context, uint64_t generation_id);
} turbo_agent_tts_provider_v1_t;

typedef struct turbo_agent_tts_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_tts_format_t output;
  uint32_t max_text_bytes; /* 1..4096 */
  uint32_t max_audio_ms;   /* exact multiple of 20, 20..30000 */
} turbo_agent_tts_config_t;

void turbo_agent_tts_config_init(turbo_agent_tts_config_t *config);
void turbo_agent_tts_provider_v1_init(turbo_agent_tts_provider_v1_t *provider);

int turbo_agent_speech_output_create(
    const turbo_agent_tts_config_t *config,
    const turbo_agent_tts_provider_v1_t *provider,
    turbo_agent_speech_output_t **out_output);

/* Plain UTF-8 only: reject embedded NUL, malformed or unsafe controls/bidi,
 * and SSML-like angle markup. No automatic language/model/network fallback.
 * The host supplies only an approved user-visible assistant FINAL; this
 * API does not read an Agent result or give text any tool authority.
 * Generation IDs are strictly increasing; callbacks may not reenter. */
int turbo_agent_speech_output_begin(turbo_agent_speech_output_t *output,
                                    uint64_t generation_id,
                                    const char *approved_utf8,
                                    size_t text_bytes,
                                    const char *voice_id,
                                    const char *language);

/* Exactly one Provider poll per call: 0 for progress, EAGAIN for a temporary
 * empty provider, EBUSY if host still has pending unconsumed PCM.
 * No implicit busy-spin. The total generated PCM cannot exceed config cap. */
int turbo_agent_speech_output_pump(turbo_agent_speech_output_t *output,
                                   uint64_t generation_id);

/* Peek returns BORROWED output samples owned by this one host object. Valid
 * only until the next consume, cancel or destroy; the pointer must not be
 * retained by a device callback. Host is the sole Salts::Playback producer.
 * On a short playback_write, consume only its accepted sample count; retry
 * the remaining borrowed suffix, then poll the next block. */
int turbo_agent_speech_output_peek_pcm(
    turbo_agent_speech_output_t *output, uint64_t generation_id,
    const int16_t **out_samples, size_t *out_count);
int turbo_agent_speech_output_consume_pcm(
    turbo_agent_speech_output_t *output, uint64_t generation_id,
    size_t accepted_samples);

/* Cancellation is control-owner only. It discards host staging PCM and asks
 * Provider to settle. Repeated cancel in CANCEL_PENDING is idempotent.
 * destroy MUST return EBUSY while active / pending / unconsumed. Do not
 * dlclose a Provider DSO until destroy succeeds and borrowed callbacks cease. */
int turbo_agent_speech_output_cancel(turbo_agent_speech_output_t *output,
                                     uint64_t generation_id);
turbo_agent_tts_state_t turbo_agent_speech_output_get_state(
    const turbo_agent_speech_output_t *output);
int turbo_agent_speech_output_destroy(turbo_agent_speech_output_t **output);

#ifdef __cplusplus
}
#endif
#endif
