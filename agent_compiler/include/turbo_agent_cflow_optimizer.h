#ifndef TURBO_AGENT_CFLOW_OPTIMIZER_H
#define TURBO_AGENT_CFLOW_OPTIMIZER_H

#include <stddef.h>
#include <stdint.h>

#include <turbo_agent_api.h>

#include "turbo_agent_cflow_region.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_CFLOW_VALUE_EDGE_ABI_VERSION 1u
#define TURBO_AGENT_CFLOW_COMPOSITION_ABI_VERSION 1u
#define TURBO_AGENT_CFLOW_OPTIMIZER_OPTIONS_ABI_VERSION 1u

typedef enum turbo_agent_cflow_optimizer_status_e {
  TURBO_AGENT_CFLOW_OPTIMIZER_OK = 0,
  TURBO_AGENT_CFLOW_OPTIMIZER_INVALID_ARGUMENT = -1,
  TURBO_AGENT_CFLOW_OPTIMIZER_OUT_OF_MEMORY = -2,
  TURBO_AGENT_CFLOW_OPTIMIZER_REGION_COMPILE_FAILED = -3
} turbo_agent_cflow_optimizer_status_t;

/**
 * One explicit typed value edge between already-admitted DAG steps.
 *
 * This is not a Phase-3 ordering edge. producer_step_index/consumer_step_index
 * name DAG steps only; consumer_property identifies the logical RuntimeTools
 * slot receiving the producer result. operator_kind remains explicit.
 */
typedef struct turbo_agent_cflow_value_edge_s {
  uint32_t struct_size;
  uint32_t abi_version;
  size_t producer_step_index;
  size_t consumer_step_index;
  turbo_agent_cflow_region_operator_t operator_kind;
  const char *consumer_property;
} turbo_agent_cflow_value_edge_t;

/**
 * Initial Phase-4E composition source.
 *
 * Edges must form one explicit linear chain:
 *
 *   edge[i].consumer == edge[i+1].producer
 *
 * No fan-in/fan-out or DAG depends_on inference is performed.
 */
typedef struct turbo_agent_cflow_composition_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_agent_cflow_value_edge_t *edges;
  size_t edge_count;
} turbo_agent_cflow_composition_source_t;

typedef struct turbo_agent_cflow_optimizer_options_s {
  uint32_t struct_size;
  uint32_t abi_version;
  int enabled;
} turbo_agent_cflow_optimizer_options_t;

typedef struct turbo_agent_cflow_optimizer_result_s
    turbo_agent_cflow_optimizer_result_t;

CXX_C_API void turbo_agent_cflow_value_edge_init(
    turbo_agent_cflow_value_edge_t *edge);

CXX_C_API void turbo_agent_cflow_composition_source_init(
    turbo_agent_cflow_composition_source_t *source);

CXX_C_API void turbo_agent_cflow_optimizer_options_init(
    turbo_agent_cflow_optimizer_options_t *options);

/**
 * Partition one explicit linear composition into maximal eligible CFlow regions.
 *
 * Eligibility is delegated to the canonical #63 region admission path.
 * Unsupported/UNKNOWN edges split regions; they never trigger fallback
 * inference. Only runs containing at least one eligible edge (two DAG steps)
 * become CFlow regions.
 *
 * The result owns compiled region plans but borrows the same provider/
 * descriptor/code lifetime as the input DAG.
 */
CXX_C_API turbo_agent_cflow_optimizer_status_t
turbo_agent_cflow_optimizer_discover(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_composition_source_t *source,
    const turbo_agent_cflow_optimizer_options_t *options,
    turbo_agent_cflow_optimizer_result_t **out_result);

CXX_C_API void turbo_agent_cflow_optimizer_result_destroy(
    turbo_agent_cflow_optimizer_result_t *result);

CXX_C_API int turbo_agent_cflow_optimizer_enabled(
    const turbo_agent_cflow_optimizer_result_t *result);

CXX_C_API size_t turbo_agent_cflow_optimizer_region_count(
    const turbo_agent_cflow_optimizer_result_t *result);

CXX_C_API size_t turbo_agent_cflow_optimizer_region_step_count(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index);

CXX_C_API size_t turbo_agent_cflow_optimizer_region_dag_step_index(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index,
    size_t position);

/** Borrow the canonical RuntimeTools identity frozen in the source DAG. */
CXX_C_API const char *turbo_agent_cflow_optimizer_region_tool_name(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index,
    size_t position);

/** Borrow one compiled #63 region plan owned by the optimizer result. */
CXX_C_API const turbo_agent_cflow_region_plan_t *
turbo_agent_cflow_optimizer_region_plan(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t region_index);

/**
 * Return the #63 admission result for one source edge.
 *
 * OK means the edge participates in an eligible native MAP chain. Any barrier
 * value is fail-closed diagnostic data only. When optimization is disabled,
 * no admission is performed and INVALID_ARGUMENT is returned.
 */
CXX_C_API turbo_agent_cflow_region_status_t
turbo_agent_cflow_optimizer_edge_status(
    const turbo_agent_cflow_optimizer_result_t *result,
    size_t edge_index);

CXX_C_API const char *turbo_agent_cflow_optimizer_status_string(
    turbo_agent_cflow_optimizer_status_t status);

#ifdef __cplusplus
}
#endif

#endif
