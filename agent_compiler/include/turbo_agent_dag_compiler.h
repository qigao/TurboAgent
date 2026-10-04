#ifndef TURBO_AGENT_DAG_COMPILER_H
#define TURBO_AGENT_DAG_COMPILER_H

#include "turbo_agent_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_DAG_STEP_ABI_VERSION 1u
#define TURBO_AGENT_DAG_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_DAG_CERTIFICATE_VERSION 1u

typedef enum turbo_agent_dag_step_flag_e {
  TURBO_AGENT_DAG_STEP_NONE = 0,
  TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE = 1u << 0,
  TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER = 1u << 1
} turbo_agent_dag_step_flag_t;

/**
 * One already-bound source step for Phase-3 DAG admission.
 *
 * All pointers are borrowed for the compile call. tool_name may use a provider
 * compatible external alias; the compiled DAG freezes the canonical RuntimeTools
 * identity. depends_on contains source step IDs, not backend handles.
 */
typedef struct turbo_agent_dag_step_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *step_id;
  const char *tool_name;
  const json_value_t *arguments;
  const char *const *depends_on;
  size_t dependency_count;
  uint32_t retry_limit;
  uint32_t flags;
} turbo_agent_dag_step_source_t;

/**
 * Finite multi-step source graph.
 *
 * replan_budget is a Harness-level bound recorded into plan identity. It never
 * authorizes model invocation from inside deterministic execution.
 */
typedef struct turbo_agent_dag_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_agent_dag_step_source_t *steps;
  size_t step_count;
  uint32_t replan_budget;
} turbo_agent_dag_source_t;

typedef struct turbo_agent_executable_dag_s turbo_agent_executable_dag_t;

CXX_C_API void
turbo_agent_dag_step_source_init(turbo_agent_dag_step_source_t *step);

CXX_C_API void
turbo_agent_dag_source_init(turbo_agent_dag_source_t *source);

/**
 * Compile one finite DAG into immutable execution authority.
 *
 * The source registry is borrowed by the returned plan-owned RuntimeTools
 * projection. It and all projected callback dependencies must outlive the DAG.
 * Compilation performs no tool callbacks.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_dag(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_dag_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

CXX_C_API void
turbo_agent_executable_dag_destroy(turbo_agent_executable_dag_t *plan);

CXX_C_API uint64_t
turbo_agent_executable_dag_hash(const turbo_agent_executable_dag_t *plan);

CXX_C_API size_t
turbo_agent_executable_dag_step_count(const turbo_agent_executable_dag_t *plan);

/**
 * Borrow the exact compiler-approved RuntimeTools projection.
 *
 * The projection remains owned by the DAG and is valid until plan destruction.
 * Callers must not mutate or destroy it.
 */
CXX_C_API const turbo_tool_registry_t *
turbo_agent_executable_dag_approved_tools(
    const turbo_agent_executable_dag_t *plan);

/** Return deterministic caller-owned certificate JSON for the frozen DAG. */
CXX_C_API json_value_t *
turbo_agent_executable_dag_certificate_json_value(
    const turbo_agent_executable_dag_t *plan);

#ifdef __cplusplus
}
#endif

#endif
