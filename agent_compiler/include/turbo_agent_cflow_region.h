#ifndef TURBO_AGENT_CFLOW_REGION_H
#define TURBO_AGENT_CFLOW_REGION_H

#include <stddef.h>
#include <stdint.h>

#include <cflow/adapters.h>

#include "turbo_agent_contracts.h"
#include "turbo_agent_dag_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_CFLOW_REGION_STEP_ABI_VERSION 1u
#define TURBO_AGENT_CFLOW_REGION_SOURCE_ABI_VERSION 1u
#define TURBO_AGENT_CFLOW_REGION_CERTIFICATE_VERSION 1u

typedef enum turbo_agent_cflow_region_op_e {
  TURBO_AGENT_CFLOW_REGION_OP_INVALID = 0,
  TURBO_AGENT_CFLOW_REGION_OP_MAP = 1
} turbo_agent_cflow_region_op_t;

/**
 * One explicit value/data edge stage inside a compiler-owned typed region.
 *
 * step_id identifies an already-compiled DAG step. input_property names the
 * logical RuntimeTools argument slot that receives the region value. The DAG's
 * depends_on topology remains ordering-only and is never interpreted as this
 * value edge.
 */
typedef struct turbo_agent_cflow_region_step_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const char *step_id;
  const char *input_property;
  turbo_agent_cflow_region_op_t op;
} turbo_agent_cflow_region_step_source_t;

/**
 * Explicit ordered native dataflow region.
 *
 * Phase 4D v1 is intentionally narrow: two or more linear MAP stages only.
 */
typedef struct turbo_agent_cflow_region_source_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_agent_cflow_region_step_source_t *steps;
  size_t step_count;
} turbo_agent_cflow_region_source_t;

typedef struct turbo_agent_cflow_region_s turbo_agent_cflow_region_t;

CXX_C_API void turbo_agent_cflow_region_step_source_init(
    turbo_agent_cflow_region_step_source_t *step);
CXX_C_API void turbo_agent_cflow_region_source_init(
    turbo_agent_cflow_region_source_t *source);

/**
 * Compile one explicit static-native MAP-only region from an already-admitted
 * executable DAG.
 *
 * Admission proves, independently:
 * - the referenced DAG step/control boundary is optimizer-safe;
 * - RuntimeTools logical input/result contracts are compatible;
 * - RuntimeTools effects/policy are PURE + READ_ONLY + PARALLEL_SAFE;
 * - canonical CMeta FunctionDesc/FunctionAbi/callable authority exists;
 * - native input/result types agree with the logical contracts;
 * - adjacent CMeta native value types are equal;
 * - CFlow Direct/Plan eligibility is satisfied.
 *
 * No tool callback executes during compilation. The resulting CFlow Plan stores
 * already-bound callables and performs no RuntimeTools/Reflection lookup during
 * evaluation.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_cflow_region(
    const turbo_agent_executable_dag_t *dag,
    const turbo_agent_cflow_region_source_t *source,
    turbo_agent_cflow_region_t **out_region,
    turbo_agent_compile_diagnostic_t *diagnostic);

CXX_C_API void turbo_agent_cflow_region_destroy(
    turbo_agent_cflow_region_t *region);

CXX_C_API size_t turbo_agent_cflow_region_step_count(
    const turbo_agent_cflow_region_t *region);
CXX_C_API uint64_t turbo_agent_cflow_region_hash(
    const turbo_agent_cflow_region_t *region);
CXX_C_API uint64_t turbo_agent_cflow_region_source_dag_hash(
    const turbo_agent_cflow_region_t *region);

/** Borrow canonical native input/output type descriptors. */
CXX_C_API const cmeta_type_desc *turbo_agent_cflow_region_input_type(
    const turbo_agent_cflow_region_t *region);
CXX_C_API const cmeta_type_desc *turbo_agent_cflow_region_output_type(
    const turbo_agent_cflow_region_t *region);

/**
 * Execute only the compiled bound CFlow plan.
 *
 * Successful out_result ownership is exactly cflow_result ownership and must be
 * released with cflow_result_destroy(). TurboAgent does not wrap or replace the
 * canonical CMeta/CFlow lifecycle.
 */
CXX_C_API bool turbo_agent_cflow_region_eval_array(
    const turbo_agent_cflow_region_t *region,
    const void *inputs,
    size_t input_count,
    cflow_result *out_result);

/** Deterministic caller-owned certificate for backend/lowering identity. */
CXX_C_API json_value_t *turbo_agent_cflow_region_certificate_json_value(
    const turbo_agent_cflow_region_t *region);

#ifdef __cplusplus
}
#endif

#endif
