#ifndef TURBO_AGENT_HARNESS_H
#define TURBO_AGENT_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#include <turbo_agent_api.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>

#include "turbo_agent_app.h"
#include "turbo_agent_execution.h"
#include "turbo_agent_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_HARNESS_ABI_VERSION 1U

typedef struct turbo_agent_harness_s turbo_agent_harness_t;
typedef struct turbo_agent_harness_execution_s turbo_agent_harness_execution_t;

/**
 * Configuration for one embedded TurboAgent harness.
 *
 * The harness creates and owns one app from `app_config`. `executor` is
 * borrowed and must outlive the harness and every execution submitted through
 * it. The executor must have an explicit bounded queue capacity.
 */
typedef struct turbo_agent_harness_config_s {
  size_t struct_size;
  uint32_t abi_version;
  const turbo_agent_app_config_t *app_config;
  cmeta_threadpool_t *executor;
} turbo_agent_harness_config_t;

/**
 * Options shared by start, resume, and fork submissions.
 *
 * `graph_options`, `session_options`, and their strings are borrowed only for
 * the submit call; the execution layer copies the values it needs. Event sink
 * user data is borrowed until the execution becomes terminal.
 */
typedef struct turbo_agent_harness_run_options_s {
  size_t struct_size;
  uint32_t abi_version;
  const turbo_graph_run_options_t *graph_options;
  const turbo_agent_session_exec_options_t *session_options;
  uint64_t deadline_mono_ms;
  turbo_event_sink_json_value_fn event_sink;
  void *event_sink_user_data;
} turbo_agent_harness_run_options_t;

CXX_C_API void turbo_agent_harness_config_init(turbo_agent_harness_config_t *config);
CXX_C_API void turbo_agent_harness_run_options_init(turbo_agent_harness_run_options_t *options);

CXX_C_API turbo_agent_harness_t *
turbo_agent_harness_create(const turbo_agent_harness_config_t *config);
/**
 * Construct with an explicit complete RuntimeStore, without memory fallback.
 * Invalid configuration clears *out_harness and consumes no stores. Allocation
 * before Session construction also consumes none. Once Session construction begins,
 * RuntimeStore is consumed even on failure; MemoryStore transfers only on
 * success. Executor is always borrowed. Callback completeness does not establish
 * backend durability. Returns SALTS_OK, SALTS_EINVAL, SALTS_ENOMEM or SALTS_EIO.
 */
CXX_C_API int turbo_agent_harness_create_with_store(
    const turbo_agent_harness_config_t *config, turbo_agent_harness_t **out_harness);
CXX_C_API turbo_agent_harness_t *turbo_agent_harness_retain(turbo_agent_harness_t *harness);
CXX_C_API void turbo_agent_harness_release(turbo_agent_harness_t *harness);

/** @brief Return the app owned by the harness. The pointer is borrowed. */
CXX_C_API turbo_agent_app_t *turbo_agent_harness_app(const turbo_agent_harness_t *harness);

CXX_C_API int turbo_agent_harness_get_capabilities(const turbo_agent_harness_t *harness,
                                                   json_value_t **out_capabilities_json);
CXX_C_API int turbo_agent_harness_get_tool_schemas(const turbo_agent_harness_t *harness,
                                                   json_value_t **out_tool_schemas_json);
CXX_C_API int turbo_agent_harness_get_startup_diagnostics(const turbo_agent_harness_t *harness,
                                                          json_value_t **out_diagnostics_json);

/**
 * Start the harness default workflow from a TurboParser JSON-native state.
 * Only one non-terminal execution may be active per harness.
 */
CXX_C_API int turbo_agent_harness_start_state(turbo_agent_harness_t *harness,
                                              const json_value_t *state,
                                              const turbo_agent_harness_run_options_t *options,
                                              turbo_agent_harness_execution_t **out_execution);

/** @brief Start the harness default workflow from one user message. */
CXX_C_API int turbo_agent_harness_start_text(turbo_agent_harness_t *harness, const char *user_text,
                                             const turbo_agent_harness_run_options_t *options,
                                             turbo_agent_harness_execution_t **out_execution);

/**
 * Synchronous, explicit compiled Inspect admission and execution.
 *
 * This narrow Phase-1 API compiles and executes exactly one typed, read-only
 * Inspect step in the attached Session's existing Run/Checkpoint runtime.
 * Only PURE or READ(+NETWORK) effects, a strict result contract, and Host-
 * granted capabilities intersected with the caller's compiler restrictions
 * are admitted. Side-effecting or UNKNOWN-effect tools are not admitted.
 *
 * The Harness persists a `compiled_inspect_attempts` record in the attached
 * Runtime Store before callback dispatch. This attempt record is the sole durable
 * authority for the typed result. Each `agent_compiled_execution_journal` row
 * contains only Run/authority metadata and a reference to the attempt, never a
 * second durable result payload. The callback is not entered if the STARTED
 * attempt or its Run-journal reference cannot be persisted.
 *
 * This convenience API creates a new attempt ID for each call. Callers that need
 * retry/recovery across a Session-finalization failure must use
 * `turbo_agent_harness_execute_compiled_inspect_attempt_json` with the same
 * caller-stable attempt ID. A committed attempt is replayed through the Session
 * Run; STARTED or unknown attempts fail closed and are never automatically
 * redispatched.
 *
 * These are individual Runtime Store writes, not a multi-record transaction;
 * crash guarantees depend on the selected backend. Runtime's generic graph
 * checkpoint hook is not a per-edge durable commit on ordinary completion/error.
 *
 * On successful execution, out_result and out_certificate are caller-owned JSON
 * values. After compile admission the certificate is still returned on callback
 * or result-validation failure. Return SALTS_OK only when the Run commits a valid
 * typed result; inspect out_tool_status and out_diagnostic for failure details.
 *
 * This does not expose async compiled DAG execution, approval transitions,
 * dynamic Plugin lease management or hard process sandboxing. The source registry
 * and callback implementations must remain alive and quiescent for the call.
 * The Host must keep the Agent tool policy stable until this synchronous call
 * returns, including store/tool/event callbacks and result delivery. Generation
 * checks do not permit concurrent or callback-reentrant policy replacement.
 */
CXX_C_API int turbo_agent_harness_execute_compiled_inspect_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate);

/**
 * Execute compiled Inspect under a caller-stable durable attempt identity.
 *
 * attempt_id is 1-64 ASCII letters/digits/underscores/hyphens and is scoped to
 * the Harness's creation-time Session thread identity. A committed attempt is
 * replayed through the Session Run so that Session finalization can be retried
 * without re-invoking the tool callback. Existing STARTED/unknown attempts fail
 * closed. The compiled_inspect_attempts record is the sole durable typed-result
 * authority; the per-Run journal stores only a reference to that attempt.
 * Replay requires matching plan/contract and Host policy generation, even if
 * replacement grants are identical. The same policy-stability requirement as
 * execute_compiled_inspect_json applies throughout this call.
 */
CXX_C_API int turbo_agent_harness_execute_compiled_inspect_attempt_json(
    turbo_agent_harness_t *harness,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *source_json, size_t source_json_size,
    const turbo_tool_execution_context_t *context,
    const char *attempt_id,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result,
    json_value_t **out_certificate);

/**
 * Execute exactly the source/attempt frozen in a bound Job in this Harness's
 * Runtime. job_id accepts 1..64 ASCII letters/digits/underscores/hyphens.
 * expected_job_revision must match the complete current Job head (0..64).
 * Missing/invalid/unbound Jobs, stale revisions and mismatched Harness thread
 * scope reject before Run creation or tool dispatch. No source, thread or
 * attempt override is accepted. Job lookup occurs inside Harness submission
 * exclusion. The Host must also serialize all access/mutation to this Job.
 *
 * Compiler, current Host grants, result contract and stable-attempt recovery
 * use the existing compiled Inspect path. STARTED/unknown attempts still refuse
 * redispatch; committed attempts replay through Session finalization. Source
 * freshness remains a Host responsibility: source_revision is only a stored
 * label. The opt-in create_verified_with_store constructor additionally requires
 * and invokes Host source/evidence callbacks; see turbo_agent_job_verifier.h.
 *
 * On success, the three distinct output slots own independent JSON values:
 * typed result, certificate and execution reference. The reference contains
 * Job/proposal revision, thread/attempt identity, canonical attempt record ID,
 * this delivery Runtime Run ID/thread, compiled execution metadata ID, plan
 * hash and authority generation; it has no result
 * payload and is not persisted as another result authority. It does not assert
 * Job acceptance or external delivery. No Job record/schema is changed.
 *
 * Output JSON slots are cleared on rejection. After compilation a certificate
 * can still accompany an execution failure, as on the existing path. If the
 * result committed but reference allocation fails, ENOMEM is returned without
 * publishing result/reference; explicit retry uses the same bound attempt.
 * Policy stability, borrowed callback lifetimes and exclusive ownership follow
 * execute_compiled_inspect_attempt_json throughout this synchronous operation.
 */
CXX_C_API int turbo_agent_harness_execute_job_inspect_json(
    turbo_agent_harness_t *harness, const char *job_id,
    uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    const turbo_tool_execution_context_t *context,
    turbo_agent_compile_diagnostic_t *out_diagnostic,
    turbo_tool_status_t *out_tool_status,
    json_value_t **out_result, json_value_t **out_certificate,
    json_value_t **out_execution_reference);

/**
 * Resume the harness default workflow.
 * `options->session_options` is required and selects checkpoint/thread scope
 * and override/patch/command input semantics.
 */
CXX_C_API int turbo_agent_harness_resume(turbo_agent_harness_t *harness, const json_value_t *input,
                                         const turbo_agent_harness_run_options_t *options,
                                         turbo_agent_harness_execution_t **out_execution);

/** @brief Fork the harness default workflow from a checkpoint or thread head. */
CXX_C_API int turbo_agent_harness_fork(turbo_agent_harness_t *harness, const json_value_t *input,
                                       const turbo_agent_harness_run_options_t *options,
                                       turbo_agent_harness_execution_t **out_execution);

CXX_C_API turbo_agent_harness_execution_t *
turbo_agent_harness_execution_retain(turbo_agent_harness_execution_t *execution);
CXX_C_API void turbo_agent_harness_execution_release(turbo_agent_harness_execution_t *execution);

CXX_C_API const char *
turbo_agent_harness_execution_id(const turbo_agent_harness_execution_t *execution);
CXX_C_API int turbo_agent_harness_execution_cancel(turbo_agent_harness_execution_t *execution,
                                                   turbo_cancel_reason_t reason);
CXX_C_API int turbo_agent_harness_execution_wait(turbo_agent_harness_execution_t *execution,
                                                 uint64_t timeout_ms);
CXX_C_API int
turbo_agent_harness_execution_get_status(const turbo_agent_harness_execution_t *execution,
                                         turbo_agent_execution_status_t *out_status);
CXX_C_API int
turbo_agent_harness_execution_result_code(const turbo_agent_harness_execution_t *execution,
                                          int *out_result_code);

/**
 * Move the terminal summary/state to the caller exactly once.
 * The returned TurboParser JSON values are owned by the caller.
 */
CXX_C_API int turbo_agent_harness_execution_take_result(turbo_agent_harness_execution_t *execution,
                                                        json_value_t **out_summary,
                                                        json_value_t **out_state);

#ifdef __cplusplus
}
#endif

#endif
