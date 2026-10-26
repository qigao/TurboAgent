#ifndef TURBO_AGENT_JOB_OBSERVATION_H
#define TURBO_AGENT_JOB_OBSERVATION_H

#include "turbo_agent_job_execution.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opt-in terminal observation over the existing asynchronous Job operation.
 * observation_id is a Host-stable 1..64 ASCII letters/digits/_/- identifier,
 * scoped to this Job. Copies all inputs and retains the same owners/reservation
 * as submit_job_inspect. Queue rejection still performs no Store access.
 *
 * After Inspect/pool cancellation and any explicit cancellation publication
 * settle, the worker attempts one create-if-absent in agent_job_observations,
 * keyed by job_id:observation_id, before announcing terminal. Requires a valid
 * bound Job at expected_revision. Duplicate IDs (even identical) return EALREADY
 * through observation_status, never overwrite. No retry, readback or fallback.
 * A failed acknowledgement may have committed; explicitly query and compare
 * supervisor identity and expected Job/binding before accepting old metadata.
 *
 * Status-v1 layout and legacy submission are unchanged. operation_status and
 * cancellation_status keep their original meanings. Observation publication
 * status is separate. take_result withholds output on an observation error
 * (returns it if Inspect otherwise succeeded), then retires normally.
 *
 * A record attests only a local operation-settled observation. It may commit
 * before live notification or pool finalization. It is not durable queue
 * admission, a remote stop receipt, execution/quality authority, a resumable
 * handle or proof that canonical STARTED/unknown attempts stopped. It stores
 * metadata/reference only, never a typed result, certificate or diagnostic text.
 */
CXX_C_API int turbo_agent_harness_submit_recorded_job_inspect(turbo_agent_harness_t *harness,
    const char *job_id, uint32_t expected_revision,
    const turbo_agent_compiler_config_t *compiler_config, uint64_t deadline_mono_ms,
    const char *observation_id, turbo_agent_job_execution_t **out_execution);

/** Borrowed copied ID, or NULL for a legacy/unrecorded handle. */
CXX_C_API const char *turbo_agent_job_execution_observation_id(const turbo_agent_job_execution_t *execution);
/** EBUSY before terminal (also sets *out_status=EBUSY). At terminal returns OK
 * and the original publication status, or ENOENT in *out_status for an
 * unrecorded handle. Does no Store access and is safe with status/wait/cancel.
 */
CXX_C_API int turbo_agent_job_execution_observation_status(const turbo_agent_job_execution_t *execution,
    int *out_status);

/** Read owned historical metadata, validating exact schema/fields/status joins
 * and the complete current Job binding. Clears *out_record on failure. Missing
 * data returns ENOENT, unsupported schemas ENOTSUP, corrupt joins EPROTO; backend
 * errors retain their status. No writes, authority check, source verification,
 * canonical result lookup, dispatch or result delivery occurs. The optional
 * execution_reference is structurally joined to the Job, not re-authorized here.
 * Host serializes this Runtime access after quiescence or in a fresh process.
 * Absence does not prove enqueue rejection or that no callback/effect occurred.
 */
CXX_C_API int turbo_agent_job_execution_observation_get(turbo_agent_runtime_t *runtime,
    const char *job_id, const char *observation_id, json_value_t **out_record);

#ifdef __cplusplus
}
#endif
#endif
