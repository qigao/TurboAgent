#ifndef TURBO_AGENT_JOB_EXECUTION_H
#define TURBO_AGENT_JOB_EXECUTION_H

#include "turbo_agent_harness.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_JOB_EXECUTION_ABI_VERSION 1U
typedef struct turbo_agent_job_execution_s turbo_agent_job_execution_t;

typedef struct turbo_agent_job_execution_status_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_execution_status_t state;
  /** Inspect status, or ECANCELED for pool cancellation before dispatch;
   * EBUSY until terminal. Independent of cancellation publication status. */
  int operation_status;
  turbo_tool_status_t tool_status;
  int cancellation_requested;
  /** ENOENT when no request, EBUSY while pending, else original publication rc. */
  int cancellation_status;
  int result_taken;
  turbo_agent_compile_diagnostic_t diagnostic;
} turbo_agent_job_execution_status_t;

CXX_C_API void turbo_agent_job_execution_status_init(turbo_agent_job_execution_status_t *status);

/** Queue one bound Job Inspect on the Harness's existing bounded executor.
 * Copies job_id and compiler config, including at most 32 capability strings of
 * 1..64 bytes. deadline_mono_ms is absolute; zero disables it. Queue delay counts.
 * Admission returns the original pool status; rejection clears *out_execution
 * and performs no Store access, Run creation, tool call or persistence fallback.
 * Job/current authority/source admission occurs in the worker, not at enqueue.
 *
 * Retains Harness/App/Session through worker exit AND result retirement. The
 * executor, registry/callback code, verifier data and Store callbacks are borrowed
 * and must remain alive; Host serializes all Job/Store/policy access through this
 * interval. One reservation excludes all other Harness operations while queued,
 * running, and terminal-but-unretired. Retire by taking once or final release.
 * Release does not cancel or detach a running worker; its reference keeps owners
 * alive until completion. Host must drain the pool before destroying borrowers.
 */
CXX_C_API int turbo_agent_harness_submit_job_inspect(turbo_agent_harness_t *harness,
    const char *job_id, uint32_t expected_revision,
    const turbo_agent_compiler_config_t *compiler_config, uint64_t deadline_mono_ms,
    turbo_agent_job_execution_t **out_execution);
CXX_C_API turbo_agent_job_execution_t *turbo_agent_job_execution_retain(turbo_agent_job_execution_t *execution);
CXX_C_API void turbo_agent_job_execution_release(turbo_agent_job_execution_t *execution);
/** Ephemeral supervisor UUID, distinct from canonical attempt/Run/execution IDs. */
CXX_C_API const char *turbo_agent_job_execution_id(const turbo_agent_job_execution_t *execution);

/** Accept the first explicit cancellation request, copy bounded identity/reason,
 * and signal the existing cooperative token. Uses the same bounds as
 * turbo_agent_job_request_cancel. OK acknowledges in-memory acceptance only.
 * The worker closes cancellation admission after Inspect returns, then publishes
 * exactly once on its serialized Runtime before announcing terminal. EALREADY
 * means an earlier request or closed admission, never persistence success.
 * No force termination, retry or error readback occurs. A blocked callback keeps
 * the handle nonterminal. Deadline expiry alone creates no durable request.
 * Pool CANCEL_PENDING settles via its cancellation/finalize callbacks; it only
 * publishes a durable request when one was explicitly accepted by this API.
 */
CXX_C_API int turbo_agent_job_execution_cancel(turbo_agent_job_execution_t *execution,
    const char *request_id, const char *reason_ref);
/** Wait for operation settlement and any accepted cancellation publication.
 * Relative timeout; UINT64_MAX waits indefinitely. Wait expiry does not cancel.
 * Terminal is a local quiescence observation, not a persistent terminal record
 * or acknowledgement that remote effects stopped. Reservation remains retained.
 * Pool finalize/reference cleanup can follow this observation. Drain the pool
 * after release when retirement must be complete; take_result retires directly.
 */
CXX_C_API int turbo_agent_job_execution_wait(turbo_agent_job_execution_t *execution, uint64_t timeout_ms);
CXX_C_API int turbo_agent_job_execution_get_status(const turbo_agent_job_execution_t *execution,
    turbo_agent_job_execution_status_t *status);

/** Attempt result transfer once after terminal, then retire the reservation even
 * on failure. Before terminal returns EBUSY without consuming; later attempts
 * EALREADY. All three distinct output slots clear on error. On success returns
 * independently owned result/certificate/reference; free with json_free().
 * Recompiles current proof and revalidates canonical attempt/journal/Run, source,
 * current authority, durable cancellation and token at this actual boundary.
 * Creates no new Run/callback; no quality acceptance is inferred. Worker retains
 * only metadata, never a buffered typed result. operation_status/terminal state
 * describe worker completion; current transfer can fail after that observation.
 * Host exclusively serializes take/release with its policy/Store/control changes.
 */
CXX_C_API int turbo_agent_job_execution_take_result(turbo_agent_job_execution_t *execution,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out_result,
    json_value_t **out_certificate, json_value_t **out_execution_reference);

#ifdef __cplusplus
}
#endif
#endif
