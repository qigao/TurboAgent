#ifndef TURBO_AGENT_SPEECH_CAPTURE_H
#define TURBO_AGENT_SPEECH_CAPTURE_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_speech_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_SPEECH_CAPTURE_ABI_VERSION 1u
#define TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES 640u

/* In addition to the underlying SpeechInput error codes:
 * devices/permissions/unavailable Capture capability never fall back to a
 * different microphone or to cloud ASR. */
#define TURBO_AGENT_SPEECH_CAPTURE_EDEVICE (-101)
#define TURBO_AGENT_SPEECH_CAPTURE_EUNSUPPORTED (-102)

typedef struct turbo_agent_speech_capture_bridge_s
    turbo_agent_speech_capture_bridge_t;

/* A bridge does not own input or a microphone. It owns only its admission
 * gate and the first failure status, never raw PCM or a second frame queue.
 * Caller keeps input/Provider alive until bridge destruction. */
int turbo_agent_speech_capture_bridge_create(
    turbo_agent_speech_input_t *input, uint64_t utterance_id,
    turbo_agent_speech_capture_bridge_t **out_bridge);

/* Control-plane only. Must be called before starting native Capture. */
int turbo_agent_speech_capture_bridge_begin(
    turbo_agent_speech_capture_bridge_t *bridge);

/* Called solely by Salts::Capture's borrowed PCM callback (or a deterministic
 * synthetic callback in tests). Exactly one complete 20ms 16k/mono/S16LE
 * audio packet, no implicit truncation or resampling. Each byte is decoded
 * into an aligned C11 int16_t frame and copied into SpeechInput's existing
 * bounded queue. Callback uses no allocation, thread join, network or ASR.
 * On admission/format error the first negative code is recorded atomically
 * so the control thread can stop capture and cancel the utterance. */
int turbo_agent_speech_capture_bridge_offer(
    turbo_agent_speech_capture_bridge_t *bridge,
    const uint8_t *borrowed_pcm, size_t bytes, uint64_t timestamp_us);

/* Close callback admission before calling salts_capture_stop(); the caller
 * must then wait for the native Capture backend to drain all callbacks. */
int turbo_agent_speech_capture_bridge_stop_admission(
    turbo_agent_speech_capture_bridge_t *bridge);

/* Only after native capture callbacks have been drained:
 * fail-closed on any lost/invalid PCM, else request SpeechInput finish.
 * Provider pumping and text emission remain caller-driven. */
int turbo_agent_speech_capture_bridge_finish(
    turbo_agent_speech_capture_bridge_t *bridge);
int turbo_agent_speech_capture_bridge_cancel(
    turbo_agent_speech_capture_bridge_t *bridge);
int turbo_agent_speech_capture_bridge_first_error(
    const turbo_agent_speech_capture_bridge_t *bridge);

/* Only after native callback quiescence. On EBUSY the bridge is intact.
 * This does not destroy the borrowed SpeechInput or its ASR Provider. */
int turbo_agent_speech_capture_bridge_destroy(
    turbo_agent_speech_capture_bridge_t **bridge);

typedef struct turbo_agent_speech_salts_capture_s
    turbo_agent_speech_salts_capture_t;

/* The real native adapter exists only in the separately selected optional
 * TurboAgent::SpeechCaptureSalts target and requires an installed
 * Salts::Capture target. No hidden fallback or bundled miniaudio. */
int turbo_agent_speech_salts_capture_create(
    turbo_agent_speech_input_t *input, uint64_t utterance_id,
    const char *device_id, turbo_agent_speech_salts_capture_t **out_capture);
int turbo_agent_speech_salts_capture_start(
    turbo_agent_speech_salts_capture_t *capture);
int turbo_agent_speech_salts_capture_first_error(
    const turbo_agent_speech_salts_capture_t *capture);
int turbo_agent_speech_salts_capture_finish(
    turbo_agent_speech_salts_capture_t *capture);
int turbo_agent_speech_salts_capture_cancel(
    turbo_agent_speech_salts_capture_t *capture);
int turbo_agent_speech_salts_capture_destroy(
    turbo_agent_speech_salts_capture_t **capture);

#ifdef __cplusplus
}
#endif
#endif
