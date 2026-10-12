#ifndef TURBO_PRAKTOR_APPROVED_TEMPLATE_H
#define TURBO_PRAKTOR_APPROVED_TEMPLATE_H

#include "turbo_agent_templates.h"
#include "turbo_praktor_tool_pack.h"
#include "turbo_runtime_store.h"
#include "turbo_agent_session_execution.h"
#include <salts/thread_pool.h>

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

/**
 * Thin async ownership handle for an admitted Praktor TemplatePlan Session
 * Graph. The existing Session/Execution executor owns actual scheduling and
 * canonical Run/Checkpoint state. This handle only retains the graph and its
 * callback data until terminal publication; it is not another runtime.
 */
typedef struct turbo_praktor_template_session_run_s
    turbo_praktor_template_session_run_t;

/**
 * Submit one precompiled Change/Repair TemplatePlan to a bounded Session
 * executor. START (checkpoint_id=NULL) can interrupt immediately before any
 * HostTool. Host then reviews the same frozen certificate and explicitly
 * commits approval; RESUME names the exact checkpoint and runs only the
 * approved plan through the existing durable Praktor attempt journal.
 *
 * The plan, Session, provider callbacks and executor are BORROWED; their owners
 * must keep them live until accepted work and the terminal callback drain.
 * All identity strings and callback data are copied into the run handle.
 * This is one opaque Graph node wrapping an already-compiled three-step
 * Praktor plan, not a per-step Praktor transaction/checkpoint mechanism.
 *
 * Release may be called before completion: the worker's terminal reference
 * keeps the graph/callback data alive. The caller should await completion
 * before destroying the Session, compiler projection or backend providers.
 * The node cannot force-cancel an in-flight synchronous Praktor callback;
 * cancellation at Graph safe points is handled by Session.
 *
 * Returns SALTS_OK on accepted submission; no tool or approval is inferred
 * by this call. No automatic fallback to default Agent Loop or retries.
 */
CXX_C_API int turbo_praktor_template_session_submit(
    cmeta_threadpool_t *executor, turbo_agent_session_t *session,
    const turbo_agent_template_plan_t *frozen_plan,
    const char *attempt_id, const char *host_approval_id,
    const char *checkpoint_id, int interrupt_before_workflow,
    uint64_t deadline_mono_ms,
    turbo_praktor_template_session_run_t **out_run);

CXX_C_API void turbo_praktor_template_session_run_release(
    turbo_praktor_template_session_run_t *run);
CXX_C_API int turbo_praktor_template_session_run_wait(
    turbo_praktor_template_session_run_t *run, uint64_t timeout_ms);
CXX_C_API int turbo_praktor_template_session_run_cancel(
    turbo_praktor_template_session_run_t *run, turbo_cancel_reason_t reason);
CXX_C_API int turbo_praktor_template_session_run_status(
    const turbo_praktor_template_session_run_t *run,
    turbo_agent_execution_status_t *out_status);
CXX_C_API int turbo_praktor_template_session_run_take_result(
    turbo_praktor_template_session_run_t *run,
    json_value_t **out_summary, json_value_t **out_state);

#ifdef __cplusplus
}
#endif
#endif
