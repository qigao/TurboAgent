#ifndef TURBO_PRAKTOR_APPROVED_TEMPLATE_H
#define TURBO_PRAKTOR_APPROVED_TEMPLATE_H

#include "turbo_agent_templates.h"
#include "turbo_praktor_tool_pack.h"
#include "turbo_runtime_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Host-only authorization of one frozen Change/Repair template, exactly one
 * Session thread and exactly one durable attempt. This operation never invokes
 * Praktor or a Tool and publishes no permission from Effect metadata.
 *
 * Approval identifiers are host-issued, not model-supplied. Calling this API is
 * itself a trusted host approval action, and MUST only follow review of the
 * identical compiled certificate. It persists a receipt in the caller-owned
 * RuntimeStore; the executor will reject absent or mismatched receipts.
 *
 * The RuntimeStore must provide durable, exclusively owned record storage
 * (bounded SQLite for crash recovery); the caller must serialize approval /
 * execution operations on one Session. No in-memory or file backend grants
 * cross-process atomicity. Approval cannot be revoked once tool side effects
 * are uncertain; handle revocation as a separate host policy.
 */
CXX_C_API turbo_tool_status_t turbo_praktor_template_approval_commit(
    turbo_agent_runtime_t *runtime,
    const turbo_agent_template_plan_t *frozen_plan,
    const char *thread_id, const char *attempt_id,
    const char *host_approval_id);

/**
 * Execute a Praktor ABI 2.6 inline WorkflowPlan only if a matching durable host
 * approval is present. The exact immutable plan is compiled with the frozen
 * RuntimeTools projection and must pass zero-retry/effect/approval preflight.
 *
 * This call commits STARTED before invoking Praktor, then COMMITTED on a known
 * terminal result. An existing COMMITTED attempt replays its bounded typed
 * result; STARTED returns UNKNOWN_SIDE_EFFECT and never automatically retries.
 * External effects are not atomic with the SQLite commit; no exactly-once claim.
 *
 * Input and Runtime-owned callbacks are borrowed for the synchronous call;
 * context controls cancellation/deadline inside the Praktor execution bridge.
 * Host must serialize calls on the same Session RuntimeStore. This API does
 * not start a TurboAgent model turn or manipulate Graph checkpoints.
 */
CXX_C_API turbo_tool_status_t turbo_praktor_template_execute_approved(
    turbo_agent_runtime_t *runtime,
    const turbo_agent_template_plan_t *frozen_plan,
    const char *thread_id, const char *attempt_id,
    const char *host_approval_id,
    const turbo_tool_execution_context_t *context,
    int *out_replayed,
    turbo_agent_template_outcome_t *out_outcome,
    json_value_t **out_result);

#ifdef __cplusplus
}
#endif
#endif
