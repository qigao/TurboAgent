#ifndef TURBO_AGENT_TEMPLATES_H
#define TURBO_AGENT_TEMPLATES_H

#include "turbo_agent_dag_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_TEMPLATE_TOOL_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION 1u

/**
 * One caller-selected RuntimeTool for a template-owned DAG role.
 *
 * The template owns step identity/topology and approval/checkpoint flags.
 * Callers provide only the tool identity, concrete arguments and finite retry
 * bound. All pointers are borrowed for the compile call.
 */
typedef struct turbo_agent_template_tool_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *tool_name;
  const json_value_t *arguments;
  uint32_t retry_limit;
} turbo_agent_template_tool_source_t;

/** Canonical Change.v1 = inspect -> change -> verify. */
typedef struct turbo_agent_change_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_agent_template_tool_source_t inspect;
  turbo_agent_template_tool_source_t change;
  turbo_agent_template_tool_source_t verify;
  uint32_t plan_generation;
} turbo_agent_change_source_t;

/** Canonical Repair.v1 = diagnose -> change -> verify. */
typedef struct turbo_agent_repair_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_agent_template_tool_source_t diagnose;
  turbo_agent_template_tool_source_t change;
  turbo_agent_template_tool_source_t verify;
  uint32_t plan_generation;
  uint32_t replan_budget;
} turbo_agent_repair_source_t;

typedef enum turbo_agent_repair_outcome_e {
  TURBO_AGENT_REPAIR_OUTCOME_INVALID = -1,
  TURBO_AGENT_REPAIR_OUTCOME_COMPLETED = 0,
  TURBO_AGENT_REPAIR_OUTCOME_REPLAN_REQUIRED = 1
} turbo_agent_repair_outcome_t;

CXX_C_API void
turbo_agent_template_tool_source_init(
    turbo_agent_template_tool_source_t *source);

CXX_C_API void
turbo_agent_change_source_init(turbo_agent_change_source_t *source);

CXX_C_API void
turbo_agent_repair_source_init(turbo_agent_repair_source_t *source);

/**
 * Compile the fixed Change.v1 topology.
 *
 * inspect/verify tools must be READ_ONLY. change must not be READ_ONLY.
 * The change step always receives APPROVAL_BEFORE + CHECKPOINT_AFTER.
 */
CXX_C_API turbo_agent_compile_status_t
turbo_agent_compile_change_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_change_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

/**
 * Compile the fixed Repair.v1 topology.
 *
 * diagnose/verify tools must be READ_ONLY. change must not be READ_ONLY.
 * The change step always receives APPROVAL_BEFORE + CHECKPOINT_AFTER.
 * replan_budget is frozen into the DAG identity but never causes model
 * invocation from deterministic execution.
 */
CXX_C_API turbo_agent_compile_status_t
turbo_agent_compile_repair_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_repair_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

/**
 * Classify the Repair.v1 verify RuntimeTool result.
 *
 * Accepted postcondition:
 *   {"verified": true, ...}  -> COMPLETED
 *   {"verified": false, ...} -> REPLAN_REQUIRED
 *
 * Missing/non-boolean verified fails closed as INVALID.
 */
CXX_C_API turbo_agent_repair_outcome_t
turbo_agent_repair_classify_verify_result(
    const json_value_t *verify_result);

#ifdef __cplusplus
}
#endif

#endif
