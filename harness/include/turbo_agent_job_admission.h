#ifndef TURBO_AGENT_JOB_ADMISSION_H
#define TURBO_AGENT_JOB_ADMISSION_H

#include "turbo_agent_job_observation.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Persist one immutable Inspect admission intent BEFORE attempting the bounded
 * queue. admission_id is a Host-stable 1..64 ASCII letters/digits/_/- identifier.
 * It is also the terminal observation ID; both records use the same supervisor
 * UUID and bound Job/proposal/thread/attempt identities.
 *
 * Copies/validates inputs, allocates owners and reserves the Harness before
 * Store access. Requires a complete bound Job at expected_revision and no durable
 * cancellation. One create-if-absent in agent_job_admissions must acknowledge OK
 * before queue submission. Write errors, uncertain acknowledgements and duplicate
 * IDs return their original status, clear *out_execution and never enqueue.
 * No overwrite, retry, readback or fallback occurs. Current source/authority and
 * canonical execution admission are still checked by the worker.
 *
 * Unlike legacy/recorded submission, queue rejection CAN leave an intent. Its
 * original ENOBUFS/ESHUTDOWN is preserved and no terminal observation is invented.
 * The record is not proof of queue acceptance, dispatch, callback entry or safe
 * retry. A crash can leave the same record before or after successful enqueue.
 * No recovery/resume API is provided. Reusing its ID returns EALREADY even if the
 * first operation never queued. A different ID is not permission to retry an
 * unknown canonical attempt. Durability follows the selected Runtime Store.
 *
 * This call performs synchronous Store I/O under the retained reservation, with
 * no execution mutex held. Host serializes Runtime/Store/policy access. After
 * success all recorded submission ownership, cancellation, observation and result
 * retirement rules apply, including withholding unacknowledged recorded results.
 */
CXX_C_API int turbo_agent_harness_submit_admitted_job_inspect(turbo_agent_harness_t *harness,
    const char *job_id, uint32_t expected_revision,
    const turbo_agent_compiler_config_t *compiler_config, uint64_t deadline_mono_ms,
    const char *admission_id, turbo_agent_job_execution_t **out_execution);

/** Read owned historical intent metadata, validating complete bound Job history
 * and exact schema/identity joins. Clears *out_record on error. ENOENT means no
 * record, ENOTSUP a future schema, EPROTO corrupt data; backend errors propagate.
 * Does not enqueue, write, check current authority/source/cancellation, load a
 * result or infer queue/terminal status. Compare any terminal observation's UUID
 * and all binding identities explicitly. Host serializes Runtime access after
 * quiescence or in a fresh process. Records contain no config, deadline or grant;
 * process-local monotonic deadlines cannot be restored from this metadata.
 */
CXX_C_API int turbo_agent_job_admission_get(turbo_agent_runtime_t *runtime,
    const char *job_id, const char *admission_id, json_value_t **out_record);

#ifdef __cplusplus
}
#endif
#endif
