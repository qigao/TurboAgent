#ifndef TURBO_AGENT_JOB_VERIFIER_H
#define TURBO_AGENT_JOB_VERIFIER_H

#include "turbo_agent_job_acceptance.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_JOB_VERIFIER_ABI_VERSION 1U

/** Host verification for bound Job APIs. Both callbacks are mandatory for the
 * verified constructor. Return SALTS_OK only for verified input; every nonzero
 * status (including missing, stale, unavailable or unknown evidence) is preserved
 * and refuses the operation. No fallback, retry or cached verification follows.
 *
 * source receives the actual immutable Job snapshot, including frozen proposal
 * bytes and source_revision. Verify those against Host-owned current sources;
 * the label alone is not proof. Host must pin/serialize that external source
 * view through the whole synchronous operation. Harness checks before Job
 * execution/review, and again before returning a committed execution result.
 *
 * evidence receives the actual validated canonical typed result, exact execution
 * reference, proposed/stored decision and evidence identifier. Verify external
 * evidence belongs to that Job/proposal/result and supports that explicit Host
 * decision. This callback does not choose or infer acceptance. It runs only after
 * canonical attempt/journal/Run validation, on both acceptance publication and
 * every acceptance/delivery read or publication. Source is checked again after
 * evidence verification, before returning evidence to the publication layer.
 *
 * Calls are synchronous under Harness submission exclusion, with no Harness
 * mutex held. Borrowed JSON/strings live only through callback return: do not
 * mutate, free or retain them. Do not reenter Session/Job/delivery operations,
 * replace policy/registry or destroy this Harness from a callback. Verification
 * must be bounded, read-only and must not send messages or produce effects.
 * Host owns cancellation/deadline enforcement for its synchronous verifier.
 * It must keep external data pinned, callback code and user_data alive/quiescent
 * until all Harness references/executions are released. No dynamic DSO lease,
 * network client, distributed fence or independent evidence trust is implied.
 */
typedef int (*turbo_agent_job_verify_source_fn)(const json_value_t *job_snapshot, void *user_data);
typedef int (*turbo_agent_job_verify_evidence_fn)(const json_value_t *job_snapshot,
    const json_value_t *execution_reference, const json_value_t *canonical_result,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref, void *user_data);

typedef struct turbo_agent_job_verifier_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_job_verify_source_fn source;
  turbo_agent_job_verify_evidence_fn evidence;
  void *user_data;
} turbo_agent_job_verifier_t;

CXX_C_API void turbo_agent_job_verifier_init(turbo_agent_job_verifier_t *verifier);

/** Construct an explicit-store Harness with an immutable, complete Host verifier.
 * The configuration is copied, callback code/user_data remain borrowed; changing
 * the original structure cannot disable verification. No setter disables it.
 * Bad/absent verifier clears output and consumes no stores. Store ownership and
 * failure transfer otherwise follow turbo_agent_harness_create_with_store().
 * No external verification is performed during construction itself.
 *
 * Only bound Job APIs use this verifier. Direct compiled-attempt and ordinary
 * Session APIs retain their own Host preconditions; this is not a process sandbox.
 * Existing constructors retain their documented manual-verification contract.
 * No Job, acceptance, attempt, Run or delivery schema is changed or cached.
 */
CXX_C_API int turbo_agent_harness_create_verified_with_store(
    const turbo_agent_harness_config_t *config, const turbo_agent_job_verifier_t *verifier,
    turbo_agent_harness_t **out_harness);

#ifdef __cplusplus
}
#endif
#endif
