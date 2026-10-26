#ifndef TURBO_AGENT_JOB_CANCEL_H
#define TURBO_AGENT_JOB_CANCEL_H

#include "turbo_agent_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Publish one immutable cancellation request at the exact observed Job head.
 * request_id: 1..64 ASCII letters/digits/_/-. reason_ref: 1..1024 non-NUL bytes,
 * a Host-owned reference, not a reason payload or execution result.
 * Uses one create-if-absent in agent_job_cancellations, keyed by Job ID.
 * Duplicate publication returns EALREADY, including identical requests. Errors
 * preserve their original status; no retry, readback or put fallback occurs.
 * An acknowledgement failure may have committed: explicitly read and reconcile
 * the request identity before taking further action. A stale head returns EBUSY.
 *
 * Host must serialize this call with ALL Job/Harness operations and external
 * delivery, and quiesce execution before publication. This is a durable fence
 * against future Job progress, not an interrupt, terminal-state acknowledgement,
 * distributed lease, rollback, or proof that an external effect did not occur.
 * To interrupt an active call, signal its existing cancellation source, wait for
 * the synchronous operation to return, then publish this request. No resume or
 * deletion API is provided; continuing work requires a new Job identity.
 */
CXX_C_API int turbo_agent_job_request_cancel(turbo_agent_job_t *job,
    uint32_t expected_revision, const char *request_id, const char *reason_ref);

/** Read and validate the request against the complete current Job history.
 * Returns independently owned JSON (free with json_free), ENOENT if absent,
 * ENOTSUP for future schemas, EPROTO for invalid records/joins, or the backend
 * error. Clears *out_request on failure. No execution or write occurs.
 * Job snapshots retain their existing proposal/binding state; a cancellation
 * request is separate control evidence, never a successful execution/receipt.
 */
CXX_C_API int turbo_agent_job_get_cancellation(const turbo_agent_job_t *job,
    json_value_t **out_request);

#ifdef __cplusplus
}
#endif
#endif
