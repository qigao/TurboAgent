#ifndef TURBO_AGENT_TTS_PLAYBACK_BRIDGE_H
#define TURBO_AGENT_TTS_PLAYBACK_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_speech_output.h"
#include <salts_playback.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M4.1 experimental host-control bridge, NOT a playback owner. Its two
 * pointers are BORROWED and must remain alive until bridge_destroy returns.
 * One serialized control owner calls every API. The same owner is the sole
 * Salts::Playback producer: no native audio callback invokes TTS/Agent. */
typedef struct turbo_tts_playback_bridge turbo_tts_playback_bridge_t;
typedef enum turbo_tts_playback_bridge_state {
  TURBO_TTS_PLAYBACK_STREAMING = 1,
  TURBO_TTS_PLAYBACK_BACKPRESSURE = 2,
  TURBO_TTS_PLAYBACK_SOURCE_DONE = 3,
  TURBO_TTS_PLAYBACK_DRAINED = 4,
  TURBO_TTS_PLAYBACK_CANCEL_PENDING = 5,
  TURBO_TTS_PLAYBACK_CANCELED = 6,
  TURBO_TTS_PLAYBACK_FAILED = 7
} turbo_tts_playback_bridge_state_t;

typedef enum turbo_tts_playback_result {
  TURBO_TTS_PLAYBACK_OK = 0,
  TURBO_TTS_PLAYBACK_EINVAL = -1,
  TURBO_TTS_PLAYBACK_EBUSY = -2,
  TURBO_TTS_PLAYBACK_EPROVIDER = -3,
  TURBO_TTS_PLAYBACK_EDEVICE = -4
} turbo_tts_playback_result_t;

/* Source must already be ACTIVE; playback is a pre-existing host-owned
 * Salts::Playback configured exactly 24000Hz mono S16, NOT created here.
 * Device status and format are caller-verified at the device owner boundary.
 * This object only holds ONE 480-sample staging block and no second queue. */
int turbo_tts_playback_bridge_create(turbo_tts_output_t *source,
                                     salts_playback_t *playback,
                                     turbo_tts_playback_bridge_t **out_bridge);

/* Caller-driven, 1..poll_budget provider PCM polls. A pending short-write
 * suffix is always retried before polling ANY new PCM block. In-flight
 * bytes stay in one bounded 960-byte staging area, in strict FIFO order.
 * *out_written is the number of bytes accepted into Salts' bounded ring,
 * not necessarily played. Zero short-write reports BACKPRESSURE without spin. */
int turbo_tts_playback_bridge_pump(turbo_tts_playback_bridge_t *bridge,
                                   size_t poll_budget, size_t *out_written);

/* Only after SOURCE_DONE and every staged byte was submitted. Salts drain
 * waits for buffered audio consumption, NOT proof it was heard. On a timeout
 * the bridge remains SOURCE_DONE so the host can retry. */
int turbo_tts_playback_bridge_drain(turbo_tts_playback_bridge_t *bridge,
                                    uint32_t timeout_ms);

/* Host control plane: prohibit new writes, discard pending PCM, request TTS
 * cancel, then clear retained Salts ring only after this sole producer has
 * quiesced. Cancel request NEVER means Provider has settled. poll still needs
 * to observe its terminal. If clear fails, keep ownership and retry explicitly. */
int turbo_tts_playback_bridge_cancel(turbo_tts_playback_bridge_t *bridge);

turbo_tts_playback_bridge_state_t
turbo_tts_playback_bridge_get_state(const turbo_tts_playback_bridge_t *bridge);

/* Read-only staged-clear obligation, NOT a native device health query.
 * When nonzero, borrowed device and Provider must stay alive; caller must
 * finish/retry clear on its serialized sole producer control owner. */
int turbo_tts_playback_bridge_has_pending_clear(
    const turbo_tts_playback_bridge_t *bridge);

/* Must be terminal AND source settled; otherwise EBUSY preserves *bridge.
 * Does not destroy the source or playback native device. */
int turbo_tts_playback_bridge_destroy(turbo_tts_playback_bridge_t **bridge);

#ifdef __cplusplus
}
#endif
#endif
