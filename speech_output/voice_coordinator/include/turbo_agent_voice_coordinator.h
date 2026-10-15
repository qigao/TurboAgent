#ifndef TURBO_AGENT_VOICE_COORDINATOR_H
#define TURBO_AGENT_VOICE_COORDINATOR_H

#include <stddef.h>
#include <stdint.h>
#include "turbo_agent_tts_playback_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M4.2 experimental HALF-DUPLEX host-control facade. Does NOT own Capture,
 * Playback, Agent, ASR, Provider context or Provider DSO. Call all methods
 * from ONE serialized host owner; callbacks must not reenter the facade.
 * A checked ASR FINAL is a host assertion, NOT validated by this module.
 * Approved response text may only come from a committed Harness/Session
 * user-visible FINAL. No speculative tokens, PARTIAL or tool output. */
typedef struct turbo_voice_coordinator turbo_voice_coordinator_t;

/* Prevent one event-loop tick or native drain from monopolizing its Owner. */
#define TURBO_VOICE_MAX_POLL_BUDGET 8u
#define TURBO_VOICE_MAX_DRAIN_MS 1000u

typedef enum turbo_voice_state {
  TURBO_VOICE_READY = 0,
  TURBO_VOICE_INPUT = 1,
  TURBO_VOICE_WAIT_APPROVAL = 2,
  TURBO_VOICE_SPEAKING = 3,
  TURBO_VOICE_DRAIN_PENDING = 4,
  TURBO_VOICE_CANCEL_PENDING = 5,
  TURBO_VOICE_COMPLETED = 6,
  TURBO_VOICE_INTERRUPTED = 7,
  TURBO_VOICE_FAILED = 8
} turbo_voice_state_t;

typedef enum turbo_voice_asr_kind {
  TURBO_VOICE_ASR_PARTIAL = 0,
  TURBO_VOICE_ASR_CHECKED_FINAL = 1
} turbo_voice_asr_kind_t;

typedef enum turbo_voice_result {
  TURBO_VOICE_OK = 0,
  TURBO_VOICE_IGNORED = 1,
  TURBO_VOICE_EINVAL = -1,
  TURBO_VOICE_ESTALE = -2,
  TURBO_VOICE_EBUSY = -3,
  TURBO_VOICE_EPROVIDER = -4,
  TURBO_VOICE_EDEVICE = -5
} turbo_voice_result_t;

/* C11 stable read-only snapshot of Host obligations. Caller owns storage,
 * initializes exact struct_size / abi_version and calls on the SAME Owner.
 * No PCM pointer or native handle escapes this snapshot. Bit flags represent
 * distinct reasons a generation may not be safely retired or restarted. */
#define TURBO_VOICE_SNAPSHOT_ABI_VERSION 1u
typedef enum turbo_voice_blocker {
  TURBO_VOICE_BLOCK_INPUT_QUIESCE = 1u << 0,
  TURBO_VOICE_BLOCK_DEVICE_CLEAR = 1u << 1,
  TURBO_VOICE_BLOCK_PROVIDER_SETTLE = 1u << 2,
  TURBO_VOICE_BLOCK_PLAYBACK_DRAIN = 1u << 3,
  TURBO_VOICE_BLOCK_HOST_APPROVAL = 1u << 4,
  TURBO_VOICE_BLOCK_FAULT = 1u << 5
} turbo_voice_blocker_t;

typedef struct turbo_voice_snapshot {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t generation;
  turbo_voice_state_t state;
  uint32_t blockers;
} turbo_voice_snapshot_t;

/* init fills ABI identity; get_snapshot never mutates the Coordinator.
 * A pending clear is an outstanding obligation, not evidence of a physical
 * audio callback or a guarantee that a faulty device can recover. */
void turbo_voice_snapshot_init(turbo_voice_snapshot_t *snapshot);
int turbo_voice_get_snapshot(const turbo_voice_coordinator_t *voice,
                             turbo_voice_snapshot_t *snapshot);

/* Borrow both handles; host confirms installed Salts device uses exactly
 * 24000Hz mono S16 before create. Never open a device here. Host owns ASR,
 * microphone stop/join, Session authorization, Playback and TTS destruction. */
int turbo_voice_create(turbo_tts_output_t *source, salts_playback_t *playback,
                       turbo_voice_coordinator_t **out_voice);

/* New strictly increasing generation; host starts ASR and owns capture.
 * No new capture generation while output/clear/Provider settle is pending. */
int turbo_voice_begin_input(turbo_voice_coordinator_t *voice,
                            uint64_t generation);

/* PARTIAL is discarded, including contents (none are accepted). Only the
 * trusted host may assert CHECKED_FINAL for the current input generation.
 * It never invokes Agent/tools or starts TTS by itself. */
int turbo_voice_asr_event(turbo_voice_coordinator_t *voice,
                          uint64_t generation, turbo_voice_asr_kind_t kind);

/* Host confirmation: input device stopped and all Capture callbacks joined.
 * Output cannot start until BOTH checked FINAL and this acknowledgement.
 * An interrupted input must also quiesce before accepting a new generation. */
int turbo_voice_input_quiesced(turbo_voice_coordinator_t *voice,
                               uint64_t generation);

/* Committed, approved *user-visible* Harness FINAL only; M4.0 copies bytes
 * into its bounded owned text before Provider begin. No raw SSE/tool/SSML.
 * No Agent invocation and no playback without input quiescence. */
int turbo_voice_approved_final(turbo_voice_coordinator_t *voice,
                               uint64_t generation,
                               const char *approved_utf8, size_t bytes,
                               const char *voice_id, const char *language);

/* One bounded owner-driven bridge pump. Returns bytes accepted into the
 * Salts ring, NOT heard; completion requires separate bounded drain(). */
int turbo_voice_pump(turbo_voice_coordinator_t *voice,
                     size_t poll_budget, size_t *out_written);
int turbo_voice_drain(turbo_voice_coordinator_t *voice, uint32_t timeout_ms);

/* UI/push-to-talk interruption (NOT microphone-VAD acoustic barge-in).
 * Quiesce single writer, cancel Provider, clear Salts ring. On clear failure
 * retain bridge, source and borrowed DSO until a subsequent pump succeeds.
 * Pending Provider settlement cannot be forced or reported as safe. */
int turbo_voice_interrupt(turbo_voice_coordinator_t *voice,
                          uint64_t generation);
turbo_voice_state_t turbo_voice_get_state(
    const turbo_voice_coordinator_t *voice);
uint64_t turbo_voice_get_generation(const turbo_voice_coordinator_t *voice);

/* EBUSY if any input callback may remain, PCM ring clear is incomplete,
 * or Provider settlement has not been observed. Never destroys borrowed
 * handles or unloads a DSO. */
int turbo_voice_destroy(turbo_voice_coordinator_t **voice);

#ifdef __cplusplus
}
#endif
#endif
