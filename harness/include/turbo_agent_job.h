#ifndef TURBO_AGENT_JOB_H
#define TURBO_AGENT_JOB_H

#include <stddef.h>
#include <stdint.h>
#include <turbo_runtime_store.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_JOB_SCHEMA_VERSION 1U
#define TURBO_AGENT_JOB_MAX_REVISIONS 64U
#define TURBO_AGENT_JOB_MAX_SOURCE_BYTES 16384U

typedef struct turbo_agent_job_s turbo_agent_job_t;

/**
 * Host-owned proposal coordination, not an execution/approval authority.
 * Create freezes goal and acceptance criteria (1..4096 UTF-8 bytes each).
 * job_id is 1..64 ASCII letters, digits, underscores or hyphens. The supplied
 * Runtime is borrowed; it must outlive all Job handles and operations. No
 * Session, Runtime or backend is created, consumed or substituted here.
 *
 * Creation first rejects surviving history to prevent orphaned identity reuse.
 * Publication uses one create-if-absent record. Unsupported backends return
 * SALTS_ENOTSUP; there is no get+put emulation. Duplicate IDs return EALREADY,
 * including identical content. Backend errors propagate, with no readback,
 * retry or success inference: a failed acknowledgement may have committed.
 * Reopen and inspect explicitly to reconcile. *out_job is NULL on failure.
 */
CXX_C_API int turbo_agent_job_create(turbo_agent_runtime_t *runtime,
    const char *job_id, const char *goal, const char *acceptance_criteria,
    turbo_agent_job_t **out_job);

/** Open and validate an existing descriptor and its complete bounded history.
 * Unsupported schemas return ENOTSUP; invalid records/joins/gaps return EPROTO.
 * Missing records and other backend statuses remain distinct. No writes occur.
 */
CXX_C_API int turbo_agent_job_open(turbo_agent_runtime_t *runtime,
    const char *job_id, turbo_agent_job_t **out_job);
CXX_C_API void turbo_agent_job_destroy(turbo_agent_job_t *job);

/** Return an independently owned JSON snapshot; free with json_free().
 * revision=0/state=created precedes proposals. A proposal's version is the
 * revision that published it. An attempt_bound snapshot retains that proposal
 * and only a compiled Inspect thread/attempt reference, never its result.
 * Binding means outcome unresolved, not callback started or Job accepted.
 * Clears *out_snapshot on failure. No tools, model, list query or writes run.
 */
CXX_C_API int turbo_agent_job_get_snapshot(const turbo_agent_job_t *job,
    json_value_t **out_snapshot);

/** Append an immutable proposal, only from created/proposed states.
 * source_json is 1..MAX_SOURCE_BYTES bytes (embedded NUL rejected; termination
 * not required), preserved exactly, not compiled or interpreted as authority.
 * source_revision is a Host-selected 1..128-byte snapshot label, not proof that
 * workspace sources remain fresh. Admission and source validation remain with
 * the execution Host. expected_revision must match the observed head.
 */
CXX_C_API int turbo_agent_job_propose(turbo_agent_job_t *job,
    uint32_t expected_revision, const char *source_json, size_t source_json_size,
    const char *source_revision);

/** Freeze one stable compiled Inspect identity before the Host dispatches it.
 * Only proposed -> attempt_bound is supported; bound Jobs cannot silently
 * replace a proposal or bind another attempt. thread_id accepts 1..80 and
 * attempt_id 1..64 of the same ASCII identifier alphabet as job_id.
 * This neither creates nor reads a compiled attempt, approves a plan, nor runs
 * a tool. The Host must use the existing Harness stable-attempt API and its
 * admission/recovery rules. No acceptance/delivery transition is exposed yet.
 */
CXX_C_API int turbo_agent_job_bind_compiled_inspect(turbo_agent_job_t *job,
    uint32_t expected_revision, const char *thread_id, const char *attempt_id);

/**
 * All operations are synchronous; the Host must serialize them with other Job
 * operations/record mutations and quiesce before destroy. No distributed owner
 * lease or cross-process scheduling is provided. Conditional insert protects
 * the next revision from overwrite; a racing claim may return EALREADY. A stale
 * expected revision or illegal state returns EBUSY; exhausted history ENOSPC.
 * Reads scan all 64 revision slots to reject gaps, with bounded retained data.
 * Backend atomicity/crash durability is only as documented by that backend.
 * The additive turbo_agent_job_cancel.h control API durably fences proposals
 * and binding with ECANCELED without altering this schema or snapshot state.
 */

#ifdef __cplusplus
}
#endif
#endif
