#ifndef TURBO_AGENT_VOICE_HARNESS_FINAL_GATE_H
#define TURBO_AGENT_VOICE_HARNESS_FINAL_GATE_H

/* Opt-in HOST policy boundary (not a second Harness/Session owner).
 * Compile against the original TurboAgent public Harness/App APIs. */
#include "turbo_agent_harness.h"
#include "turbo_agent_voice_coordinator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Host calls only after checked ASR FINAL + Capture callback join have put
 * this generation in WAIT_APPROVAL. "host_approved" must be asserted by a
 * trusted app policy after independently reviewing the user-visible result.
 * It cannot be inferred from a model output, an SSE event, or a tool JSON.
 *
 * Admission requires:
 *   Harness execution COMPLETED, result code SALTS_OK, stable execution id;
 *   app's current run id exactly equals expected_run_id;
 *   one-shot take_result summary.status == completed, matching run_id;
 *   canonical turbo_agent_app_result_text(state), not raw provider/SSE text;
 *   M4.0 strict plain UTF-8/voice/capacity validation.
 *
 * Once the terminal result is taken, a bad summary/text is NOT retried or
 * replaced by any other run or fallback. All returned JSON and malloc text
 * are promptly released; M4.0 copies the approved text it needs.
 * Does not run Harness or tools, create devices, or store any authority. */
int turbo_voice_admit_harness_completed_final(
    turbo_voice_coordinator_t *voice,
    uint64_t generation,
    turbo_agent_harness_t *harness,
    turbo_agent_harness_execution_t *execution,
    const char *expected_run_id,
    int host_approved,
    const char *voice_id,
    const char *language);

#ifdef __cplusplus
}
#endif
#endif
