#ifndef TURBO_AGENT_TEMPLATES_H
#define TURBO_AGENT_TEMPLATES_H

#include "turbo_agent_dag_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_TEMPLATE_SLOT_ABI_VERSION 1u
#define TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_TEMPLATE_PLAN_CERTIFICATE_VERSION 1u

/**
 * One template slot binding.
 *
 * The template owns step identity/topology/approval/checkpoint semantics.
 * Callers provide only one admitted tool candidate, canonical JSON arguments,
 * and a finite retry limit.
 */
typedef struct turbo_agent_template_slot_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *tool_name;
  const json_value_t *arguments;
  uint32_t retry_limit;
} turbo_agent_template_slot_t;

typedef struct turbo_agent_change_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  uint32_t plan_version;
  turbo_agent_template_slot_t inspect;
  turbo_agent_template_slot_t change;
  turbo_agent_template_slot_t verify;
} turbo_agent_change_source_t;

typedef struct turbo_agent_repair_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** 1-based plan generation. Each replan must increment this value. */
  uint32_t plan_version;
  /** Maximum number of Harness/model replans permitted after version 1. */
  uint32_t max_replans;
  turbo_agent_template_slot_t diagnose;
  turbo_agent_template_slot_t change;
  turbo_agent_template_slot_t verify;
} turbo_agent_repair_source_t;

typedef enum turbo_agent_verify_status_e {
  TURBO_AGENT_VERIFY_PASSED = 0,
  TURBO_AGENT_VERIFY_SEMANTIC_FAILURE = 1,
  TURBO_AGENT_VERIFY_EXECUTION_FAILURE = 2
} turbo_agent_verify_status_t;

typedef enum turbo_agent_template_outcome_e {
  TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED = 0,
  TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_REQUIRED = 1,
  TURBO_AGENT_TEMPLATE_OUTCOME_VERIFY_FAILED = 2,
  TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_LIMIT_REACHED = 3,
  TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED = 4,
  TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT = -1
} turbo_agent_template_outcome_t;

typedef struct turbo_agent_template_plan_s turbo_agent_template_plan_t;

CXX_C_API void
turbo_agent_template_slot_init(turbo_agent_template_slot_t *slot);

CXX_C_API void
turbo_agent_change_source_init(turbo_agent_change_source_t *source);

CXX_C_API void
turbo_agent_repair_source_init(turbo_agent_repair_source_t *source);

/**
 * Compile canonical Change:
 *
 *   inspect -> change -> verify
 *
 * inspect/verify must resolve to READ_ONLY tools. change must not resolve to a
 * READ_ONLY tool and always receives APPROVAL_BEFORE + CHECKPOINT_AFTER.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_change_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_change_source_t *source,
    turbo_agent_template_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

/**
 * Compile canonical Repair:
 *
 *   diagnose -> change -> verify
 *
 * Replanning is never performed by this plan. The underlying DAG v2 freezes
 * Repair template identity, plan generation and replan budget into its
 * certificate/hash. turbo_agent_template_finish_verify() only classifies the
 * terminal verify result; it never advances the generation itself.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_repair_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_repair_source_t *source,
    turbo_agent_template_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

CXX_C_API void
turbo_agent_template_plan_destroy(turbo_agent_template_plan_t *plan);

CXX_C_API turbo_agent_template_kind_t
turbo_agent_template_plan_kind(const turbo_agent_template_plan_t *plan);

CXX_C_API uint32_t
turbo_agent_template_plan_version(const turbo_agent_template_plan_t *plan);

CXX_C_API uint32_t
turbo_agent_template_plan_max_replans(const turbo_agent_template_plan_t *plan);

CXX_C_API uint64_t
turbo_agent_template_plan_hash(const turbo_agent_template_plan_t *plan);

/** Borrow the immutable admitted DAG owned by the template plan. */
CXX_C_API const turbo_agent_executable_dag_t *
turbo_agent_template_plan_dag(const turbo_agent_template_plan_t *plan);

/** Borrow the exact RuntimeTools projection frozen by DAG admission. */
CXX_C_API const turbo_tool_registry_t *
turbo_agent_template_plan_approved_tools(
    const turbo_agent_template_plan_t *plan);

/**
 * Convert a verify terminal into a pure control outcome.
 *
 * This function never invokes a model, tool or Praktor. Repair semantic
 * failure returns REPLAN_REQUIRED only while the 1-based plan version still
 * has replan budget. A subsequent Harness/model/compiler turn must create the
 * next plan version.
 */
CXX_C_API turbo_agent_template_outcome_t
turbo_agent_template_finish_verify(
    const turbo_agent_template_plan_t *plan,
    turbo_agent_verify_status_t verify_status);

/**
 * Classify the canonical Praktor execution result for Change/Repair verify.
 *
 * Phase 3 uses one narrow template-level runtime contract rather than a
 * universal output type system:
 *
 *   workflow_status == "success"
 *   outputs.verify_result is an object
 *   outputs.verify_result.verified is a JSON boolean
 *
 * verified=true -> PASSED
 * verified=false -> SEMANTIC_FAILURE
 * anything missing/malformed/non-success -> EXECUTION_FAILURE
 *
 * The function is pure and never invokes Praktor, a model, or a tool.
 */
CXX_C_API turbo_agent_verify_status_t
turbo_agent_template_verify_status_from_praktor_result(
    const json_value_t *praktor_result);

/** Caller-owned deterministic certificate for template + underlying DAG. */
CXX_C_API json_value_t *
turbo_agent_template_plan_certificate_json_value(
    const turbo_agent_template_plan_t *plan);

#ifdef __cplusplus
}
#endif

#endif
