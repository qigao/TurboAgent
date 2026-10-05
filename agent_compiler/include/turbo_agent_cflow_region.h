#ifndef TURBO_AGENT_CFLOW_REGION_H
#define TURBO_AGENT_CFLOW_REGION_H

#include <stddef.h>
#include <stdint.h>

#include <cflow/adapters.h>
#include <turbo_agent_api.h>

#include "turbo_agent_dag_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION 1u
#define TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION 1u

typedef enum turbo_agent_cflow_region_operator_e {
  TURBO_AGENT_CFLOW_REGION_OPERATOR_MAP = 1
} turbo_agent_cflow_region_operator_t;

typedef enum turbo_agent_cflow_region_status_e {
  TURBO_AGENT_CFLOW_REGION_OK = 0,
  TURBO_AGENT_CFLOW_REGION_INVALID_ARGUMENT = -1,
  TURBO_AGENT_CFLOW_REGION_NOT_APPROVED = -2,
  TURBO_AGENT_CFLOW_REGION_POLICY_BARRIER = -3,
  TURBO_AGENT_CFLOW_REGION_LOGICAL_CONTRACT_BARRIER = -4,
  TURBO_AGENT_CFLOW_REGION_NATIVE_PROJECTION_BARRIER = -5,
  TURBO_AGENT_CFLOW_REGION_OWNERSHIP_BARRIER = -6,
  TURBO_AGENT_CFLOW_REGION_TYPE_BARRIER = -7,
  TURBO_AGENT_CFLOW_REGION_CFLOW_REJECTED = -8,
  TURBO_AGENT_CFLOW_REGION_OUT_OF_MEMORY = -9,
  TURBO_AGENT_CFLOW_REGION_EXECUTION_FAILED = -10
} turbo_agent_cflow_region_status_t;

/**
 * Explicit value-flow step for the first Phase-4 CFlow slice.
 *
 * dag_step_index names an already admitted DAG step. operator is explicit and
 * must currently be MAP. consumer_property is NULL/empty for the first step;
 * every later step names the logical RuntimeTools input property that consumes
 * the previous step's result.
 *
 * Phase-3 depends_on edges are deliberately not consulted or reinterpreted.
 */
typedef struct turbo_agent_cflow_region_step_s {
  uint32_t struct_size;
  uint32_t abi_version;
  size_t dag_step_index;
  turbo_agent_cflow_region_operator_t operator_kind;
  const char *consumer_property;
} turbo_agent_cflow_region_step_t;

typedef struct turbo_agent_cflow_region_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_agent_cflow_region_step_t *steps;
  size_t step_count;
} turbo_agent_cflow_region_source_t;

typedef struct turbo_agent_cflow_region_plan_s turbo_agent_cflow_region_plan_t;

CXX_C_API void turbo_agent_cflow_region_step_init(
    turbo_agent_cflow_region_step_t *step);

CXX_C_API void turbo_agent_cflow_region_source_init(
    turbo_agent_cflow_region_source_t *source);

/**
 * Check one explicit linear MAP-only region without constructing a Graph/Plan.
 *
 * This is the canonical Phase-4 eligibility query used by optimizer-owned
 * region discovery. It applies the same RuntimeTools/CMeta contract as
 * turbo_agent_compile_cflow_region().
 */
CXX_C_API turbo_agent_cflow_region_status_t turbo_agent_cflow_region_admit(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source);

/**
 * Compile one explicit linear MAP-only region from an already admitted DAG.
 *
 * The returned plan borrows the same native provider/descriptor/code lifetime
 * as the DAG's approved RuntimeTools projection. It owns only the compiled
 * CFlow plan and copied callable capture bytes. CMeta remains the only native
 * ownership/lifecycle authority.
 */
CXX_C_API turbo_agent_cflow_region_status_t turbo_agent_compile_cflow_region(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source,
    turbo_agent_cflow_region_plan_t **out_plan);

CXX_C_API void turbo_agent_cflow_region_plan_destroy(
    turbo_agent_cflow_region_plan_t *plan);

CXX_C_API const cmeta_type_desc *turbo_agent_cflow_region_input_type(
    const turbo_agent_cflow_region_plan_t *plan);

CXX_C_API const cmeta_type_desc *turbo_agent_cflow_region_output_type(
    const turbo_agent_cflow_region_plan_t *plan);

CXX_C_API size_t turbo_agent_cflow_region_step_count(
    const turbo_agent_cflow_region_plan_t *plan);

/**
 * Execute the bound CFlow plan directly.
 *
 * RuntimeTools name lookup and Reflection lookup are not performed here.
 * A successful result is owned by CFlow and must be released with
 * cflow_result_destroy().
 */
CXX_C_API turbo_agent_cflow_region_status_t turbo_agent_cflow_region_eval_array(
    const turbo_agent_cflow_region_plan_t *plan,
    const void *inputs,
    size_t input_count,
    cflow_result *out_result);

CXX_C_API const char *turbo_agent_cflow_region_status_string(
    turbo_agent_cflow_region_status_t status);

#ifdef __cplusplus
}
#endif

#endif
