#ifndef TURBO_AGENT_HARNESS_INTERNAL_H
#define TURBO_AGENT_HARNESS_INTERNAL_H

#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_job_verifier.h"

/* Internal DSO composition only; never installed as public ABI. */
typedef struct turbo_agent_inspect_admission_s {
  turbo_agent_executable_plan_t *plan;
  json_value_t *certificate;
  const turbo_tool_registry_t *approved;
  const char *name;
  const char *result_schema;
  turbo_tool_effect_flags_t effects;
} turbo_agent_inspect_admission_t;

typedef struct turbo_agent_job_proof_s {
  turbo_agent_harness_t *harness;
  turbo_agent_runtime_t *runtime;
  turbo_agent_t *agent;
  json_value_t *snapshot;
  turbo_agent_inspect_admission_t admission;
  turbo_agent_tool_authority_snapshot_t authority;
  char plan_hash[32];
} turbo_agent_job_proof_t;

int turbo_agent_harness_begin_submit(turbo_agent_harness_t *harness);
void turbo_agent_harness_end_submit(turbo_agent_harness_t *harness);
/* Reserved Job worker only: reuse the synchronous path without reacquiring or
 * releasing its caller's whole-handle submission reservation. */
cmeta_threadpool_t *turbo_agent_harness_executor(turbo_agent_harness_t *harness);
int turbo_agent_harness_execute_job_reserved(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    const turbo_tool_execution_context_t *context, turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_tool_status_t *status, json_value_t **result, json_value_t **certificate, json_value_t **reference);
/* Caller holds Harness submission exclusion and the documented quiescent policy
 * and Job ownership. This only compiles/validates, never dispatches or writes. */
int turbo_agent_harness_prepare_job_proof(turbo_agent_harness_t *harness,
    const char *id, uint32_t expected_revision, int require_active,
    const turbo_agent_compiler_config_t *config,
    turbo_agent_compile_diagnostic_t *diagnostic, turbo_agent_job_proof_t *out);
void turbo_agent_harness_job_proof_destroy(turbo_agent_job_proof_t *proof);
int turbo_agent_harness_require_job_active(turbo_agent_runtime_t *runtime,
    const json_value_t *snapshot, turbo_agent_compile_diagnostic_t *diagnostic);
/* Caller holds submission exclusion and owns a prepared proof. Revalidates the
 * immutable Host decision, including its canonical execution records. */
int turbo_agent_harness_load_job_acceptance(const turbo_agent_job_proof_t *proof,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out);
/* Same canonical joins as acceptance, without a quality decision/evidence call.
 * Returns a clone of the sole canonical typed result. No writes or execution. */
int turbo_agent_harness_load_job_result(const turbo_agent_job_proof_t *proof,
    const json_value_t *reference, turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out);
/* Shared metadata shape only; does not establish canonical/current authority. */
int turbo_agent_harness_job_reference_shape(const json_value_t *reference);
int turbo_agent_harness_verify_job_source(turbo_agent_harness_t *harness,
    const json_value_t *snapshot, turbo_agent_compile_diagnostic_t *diagnostic);
int turbo_agent_harness_verify_job_evidence(const turbo_agent_job_proof_t *proof,
    const json_value_t *reference, const json_value_t *result,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref,
    turbo_agent_compile_diagnostic_t *diagnostic);

#endif
