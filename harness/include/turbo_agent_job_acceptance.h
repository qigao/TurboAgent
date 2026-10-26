#ifndef TURBO_AGENT_JOB_ACCEPTANCE_H
#define TURBO_AGENT_JOB_ACCEPTANCE_H

#include "turbo_agent_harness.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum turbo_agent_job_acceptance_decision_e {
  TURBO_AGENT_JOB_REJECTED = 0,
  TURBO_AGENT_JOB_ACCEPTED = 1
} turbo_agent_job_acceptance_decision_t;

/**
 * Record one immutable Host acceptance decision for a bound Job. This verifies
 * the execution reference against the frozen proposal, freshly compiled current
 * Host admission, committed canonical attempt and typed result, compiled journal,
 * completed Runtime Run and its persisted state_snapshot execution identity.
 * No tool/model is called, no Run is created and no result payload is copied.
 *
 * The Host explicitly decides whether the immutable acceptance criteria were
 * met. evidence_ref is a 1..1024-byte Host evidence identifier, not fetched or
 * automatically evaluated. Accepting does not grant execution or external
 * delivery authority. Only successful typed Inspect results can be reviewed by
 * this slice; rejected refers to Host quality/criteria, not a fabricated tool
 * failure. Source freshness and external evidence verification remain with Host;
 * create_verified_with_store enforces installed callbacks on record/read/delivery.
 * See turbo_agent_job_verifier.h for that constructor and borrowed verifier inputs.
 *
 * A single create-if-absent record in agent_job_acceptance is keyed by Job ID.
 * Even identical or contrary second decisions return EALREADY; no replacement,
 * retry or readback follows a failed acknowledgement. The original store error
 * is preserved and may mean the record committed. Reconcile explicitly through
 * get_job_acceptance_json. Job v1 proposal history/schema is unchanged.
 *
 * *out_record clears on failure; on success it owns independent JSON with only
 * decision, evidence reference and execution metadata references. Keep borrowed
 * inputs stable and distinct from the output slot. Harness exclusion serializes
 * Session operations; Host must additionally serialize Job access and keep
 * policy/registry/callback code stable through return. Current policy generation
 * must match the execution reference, even after same-grant replacement.
 */
CXX_C_API int turbo_agent_harness_record_job_acceptance_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    const json_value_t *execution_reference,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref,
    turbo_agent_compile_diagnostic_t *out_diagnostic, json_value_t **out_record);

/** Read an existing Host decision and revalidate all referenced records and
 * current admission. No writes, execution, implicit repair or downgrade occurs.
 * Unknown schema returns ENOTSUP; malformed/mismatched evidence EPROTO;
 * STARTED/unknown attempt EBUSY; missing/backend statuses remain distinct.
 * A revoked/stale policy rejects delivery while preserving the stored decision.
 * Outputs/ownership and synchronous Host requirements match record().
 */
CXX_C_API int turbo_agent_harness_get_job_acceptance_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    turbo_agent_compile_diagnostic_t *out_diagnostic, json_value_t **out_record);

#ifdef __cplusplus
}
#endif
#endif
