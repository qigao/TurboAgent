#ifndef TURBO_AGENT_JOB_QUEUE_RECEIPT_H
#define TURBO_AGENT_JOB_QUEUE_RECEIPT_H
#include "turbo_agent_job_admission.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opt-in admission intent plus an immutable native queue disposition receipt.
 * All admitted submission preflight/ownership rules apply. On successful enqueue
 * the selected run/cancel callback writes its receipt BEFORE Inspect or pool
 * cancellation settlement. Only an acknowledged receipt permits Inspect.
 * Receipt errors settle locally as FAILED/EPROTO/TURBO_TOOL_ERROR, suppress the
 * Inspect/terminal-observation paths and remain separately available below.
 * Explicitly accepted Host cancellation is still published once before terminal.
 * take_result returns the original receipt error, clears outputs and retires.
 *
 * On native queue rejection no callback can run. The submitting thread attempts
 * the rejection receipt, returns the original queue status and clears the handle.
 * Required out_rejection_receipt_status receives its original publication status,
 * or ENOENT when no rejection receipt was attempted (including successful enqueue
 * and pre-queue failures). No overwrite, retry, error readback or fallback occurs.
 * An uncertain receipt may have committed; querying never promotes it to an ack.
 *
 * Accepted receipts are deferred until a callback is selected. A crash between
 * enqueue and selection can leave only the intent. Receipt presence attests a
 * historical native queue disposition, not a current queue, durable restartable
 * task, callback/Run result, finalizer completion or remote stop. The Runtime is
 * serialized: successful enqueue performs no further caller-side Store I/O.
 * Store callbacks must not synchronously wait/destroy this pool or retained owners.
 * Legacy, recorded and admitted entry points keep their original behavior.
 */
CXX_C_API int turbo_agent_harness_submit_receipted_job_inspect(turbo_agent_harness_t *harness,
    const char *job_id, uint32_t expected_revision,
    const turbo_agent_compiler_config_t *compiler_config, uint64_t deadline_mono_ms,
    const char *admission_id, int *out_rejection_receipt_status,
    turbo_agent_job_execution_t **out_execution);

/** EBUSY before terminal, then OK with the original receipt publication status.
 * Unreceipted terminal handles report ENOENT in *out_status. Does no Store access;
 * safe concurrently with wait/status/cancel. Raw backend EBUSY is distinguishable
 * from a pending operation by this outer status and the live execution state.
 */
CXX_C_API int turbo_agent_job_execution_queue_receipt_status(
    const turbo_agent_job_execution_t *execution, int *out_status);

/** Owned historical receipt. Validate complete bound Job and immutable intent,
 * exact schema/types, supervisor/binding joins and queue-status/selection coherence.
 * Clears output on failure; ENOENT when intent/receipt missing, ENOTSUP for future
 * schemas, EPROTO for corruption; backend errors propagate. Read-only, no current
 * authority/source/cancellation/result check or dispatch. Host serializes Runtime
 * access after quiescence or in a fresh process. Missing receipts leave admission
 * unknown; neither queries nor a different ID authorize retry of an unknown attempt.
 */
CXX_C_API int turbo_agent_job_queue_receipt_get(turbo_agent_runtime_t *runtime,
    const char *job_id, const char *admission_id, json_value_t **out_record);

#ifdef __cplusplus
}
#endif
#endif
