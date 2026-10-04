#ifndef TURBO_AGENT_PRAKTOR_LOWERING_H
#define TURBO_AGENT_PRAKTOR_LOWERING_H

#include "turbo_agent_templates.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_PRAKTOR_INLINE_SOURCE_ABI_VERSION 1u

typedef enum turbo_agent_praktor_lowering_status_e {
  TURBO_AGENT_PRAKTOR_LOWERING_OK = 0,
  TURBO_AGENT_PRAKTOR_LOWERING_INVALID_ARGUMENT = -1,
  TURBO_AGENT_PRAKTOR_LOWERING_INVALID_PLAN = -2,
  TURBO_AGENT_PRAKTOR_LOWERING_OUT_OF_MEMORY = -3,
  /**
   * Reserved for source compatibility with the pre-Praktor-0.4.5 lowering
   * slice. Current lowering no longer returns this status.
   */
  TURBO_AGENT_PRAKTOR_LOWERING_UNSUPPORTED_RETRY = -4
} turbo_agent_praktor_lowering_status_t;

typedef struct turbo_agent_praktor_lowering_diagnostic_s {
  turbo_agent_praktor_lowering_status_t status;
  char message[256];
} turbo_agent_praktor_lowering_diagnostic_t;

typedef struct turbo_agent_praktor_inline_source_s
    turbo_agent_praktor_inline_source_t;

/**
 * Lower one already-admitted Change/Repair TemplatePlan into deterministic,
 * in-memory Praktor YAML source.
 *
 * This API does not link or call Praktor. It owns only deterministic lowering
 * from frozen compiler facts. The returned source can later be passed to the
 * released Praktor inline WorkflowPlan ABI.
 *
 * Admitted finite retry_limit values are encoded as Praktor HostTool
 * retries.count. This layer does not link Praktor and therefore does not own
 * the backend hard cap; the released-ABI execution bridge must validate the
 * emitted count against PRAKTOR_HOST_TOOL_MAX_RETRIES before execution.
 */
CXX_C_API turbo_agent_praktor_lowering_status_t
turbo_agent_template_lower_praktor_inline(
    const turbo_agent_template_plan_t *plan,
    turbo_agent_praktor_inline_source_t **out_source,
    turbo_agent_praktor_lowering_diagnostic_t *diagnostic);

CXX_C_API void
turbo_agent_praktor_inline_source_destroy(
    turbo_agent_praktor_inline_source_t *source);

/** Borrowed logical source identity: turboagent:plan:<16-hex-plan-hash>. */
CXX_C_API const char *
turbo_agent_praktor_inline_source_id(
    const turbo_agent_praktor_inline_source_t *source);

/** Borrowed exact UTF-8 YAML bytes. Not NUL-dependent; use size accessor. */
CXX_C_API const char *
turbo_agent_praktor_inline_source_yaml(
    const turbo_agent_praktor_inline_source_t *source);

CXX_C_API size_t
turbo_agent_praktor_inline_source_yaml_size(
    const turbo_agent_praktor_inline_source_t *source);

CXX_C_API uint64_t
turbo_agent_praktor_inline_source_plan_hash(
    const turbo_agent_praktor_inline_source_t *source);

/**
 * Classify one canonical Praktor execution JSON for a Change/Repair plan.
 *
 * workflow_status != "success" is an execution failure.
 * A successful workflow must expose outputs.verify_result as an object with a
 * boolean "verified" field. Missing/malformed verify output fails closed as
 * VERIFY_FAILED and can never request replanning.
 */
CXX_C_API turbo_agent_template_outcome_t
turbo_agent_template_finish_praktor_result(
    const turbo_agent_template_plan_t *plan,
    const json_value_t *execution_result);

#ifdef __cplusplus
}
#endif

#endif
