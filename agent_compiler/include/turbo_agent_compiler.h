#ifndef TURBO_AGENT_COMPILER_H
#define TURBO_AGENT_COMPILER_H

#include <stddef.h>
#include <stdint.h>

#include <turbo_agent_api.h>
#include <json_parser.h>

#include "turbo_tool.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_TYPED_PLAN_ABI_VERSION 1u
#define TURBO_AGENT_TEMPLATE_DESCRIPTOR_ABI_VERSION 1u
#define TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION 1u
#define TURBO_AGENT_PLAN_CERTIFICATE_VERSION 1u

typedef enum turbo_agent_template_kind_e {
  TURBO_AGENT_TEMPLATE_INVALID = 0,
  TURBO_AGENT_TEMPLATE_INSPECT = 1
} turbo_agent_template_kind_t;

typedef enum turbo_agent_template_property_e {
  TURBO_AGENT_TEMPLATE_PROPERTY_NONE = 0,
  TURBO_AGENT_TEMPLATE_PROPERTY_READ_ONLY = 1u << 0
} turbo_agent_template_property_t;

typedef struct turbo_agent_template_descriptor_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_agent_template_kind_t kind;
  uint32_t version;
  const char *name;
  const char *input_contract;
  uint32_t properties;
} turbo_agent_template_descriptor_t;

/** Return the immutable built-in descriptor for one supported template. */
CXX_C_API const turbo_agent_template_descriptor_t *
turbo_agent_template_descriptor(turbo_agent_template_kind_t kind);

typedef enum turbo_agent_compile_status_e {
  TURBO_AGENT_COMPILE_OK = 0,
  TURBO_AGENT_COMPILE_INVALID_ARGUMENT = -1,
  TURBO_AGENT_COMPILE_UNSUPPORTED_TEMPLATE = -2,
  TURBO_AGENT_COMPILE_UNRESOLVED_TOOL = -3,
  TURBO_AGENT_COMPILE_CAPABILITY_DENIED = -4,
  TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION = -5,
  TURBO_AGENT_COMPILE_OUT_OF_MEMORY = -6,
  TURBO_AGENT_COMPILE_SOURCE_INVALID = -7,
  TURBO_AGENT_COMPILE_TOOL_ARGUMENTS_INVALID = -8,
  TURBO_AGENT_COMPILE_TOOL_SCHEMA_INVALID = -9,
  TURBO_AGENT_COMPILE_PLAN_LIMIT = -10,
  TURBO_AGENT_COMPILE_DUPLICATE_STEP = -11,
  TURBO_AGENT_COMPILE_MISSING_DEPENDENCY = -12,
  TURBO_AGENT_COMPILE_CYCLE = -13,
  TURBO_AGENT_COMPILE_RETRY_UNSAFE = -14
} turbo_agent_compile_status_t;

typedef struct turbo_agent_compile_diagnostic_s {
  turbo_agent_compile_status_t status;
  char message[256];
} turbo_agent_compile_diagnostic_t;

/**
 * Phase-1 normalized plan source.
 *
 * This is already-validated source data, not execution authority. Strings and
 * arguments are borrowed only for the compile call. The compiler must freeze
 * every semantic fact required by execution before returning success.
 */
typedef struct turbo_agent_typed_plan_s {
  uint32_t struct_size;
  uint32_t abi_version;
  turbo_agent_template_kind_t template_kind;
  const char *step_id;
  const char *tool_name;
  const json_value_t *arguments;
} turbo_agent_typed_plan_t;

typedef struct turbo_agent_compiler_config_s {
  uint32_t struct_size;
  uint32_t abi_version;

  /**
   * Host-admitted capability names.
   *
   * When deny_unlisted_capabilities is non-zero, every capability required by
   * the resolved tool must appear in this set. The compiler never grants a
   * capability that is absent from the tool metadata or this host admission.
   */
  const char *const *allowed_capabilities;
  size_t allowed_capability_count;
  int deny_unlisted_capabilities;

  /** Maximum bytes accepted by turbo_agent_compile_plan_json(). */
  size_t max_source_bytes;
  /** Maximum aggregate string/key/number bytes in one arguments tree. */
  size_t max_argument_bytes;
  /** Maximum JSON value nodes in one arguments tree. */
  size_t max_argument_nodes;
  /** Maximum nested object/array depth in one arguments tree. */
  size_t max_argument_depth;
} turbo_agent_compiler_config_t;

typedef struct turbo_agent_executable_plan_s turbo_agent_executable_plan_t;

CXX_C_API void turbo_agent_typed_plan_init(turbo_agent_typed_plan_t *plan);
CXX_C_API void turbo_agent_compiler_config_init(turbo_agent_compiler_config_t *config);

/**
 * Compile one Phase-1 plan.
 *
 * The source registry is borrowed for the lifetime of the returned executable
 * plan because the plan-owned projection borrows callback implementation state
 * from it. The source registry must remain alive and quiescent until the plan
 * is destroyed.
 *
 * On success, out_plan owns an opaque frozen execution authority. No tool
 * callback is invoked by compilation.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_plan(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_typed_plan_t *source,
    turbo_agent_executable_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

/**
 * Validate/bind a TurboAgent-owned Phase-1 source document through DataBind,
 * then compile it through the same admission path as turbo_agent_compile_plan().
 *
 * Expected Phase-1 source shape:
 * {
 *   "template_id": "inspect",
 *   "step_id": "...",
 *   "tool": "...",
 *   "arguments_json": "{...}"
 * }
 *
 * DataBind owns structural/type validation only. Template/tool/capability
 * semantics remain owned by this compiler.
 */
CXX_C_API turbo_agent_compile_status_t turbo_agent_compile_plan_json(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const char *source_json,
    size_t source_json_size,
    turbo_agent_executable_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic);

CXX_C_API void turbo_agent_executable_plan_destroy(
    turbo_agent_executable_plan_t *plan);

/**
 * Execute only an already-compiled plan.
 *
 * This API deliberately accepts no tool name, source JSON, TypedAgentPlan or
 * source registry. Execution consumes the frozen plan-owned projection.
 */
CXX_C_API turbo_tool_status_t turbo_agent_execute_compiled_plan(
    const turbo_agent_executable_plan_t *plan,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result);

CXX_C_API uint64_t turbo_agent_executable_plan_hash(
    const turbo_agent_executable_plan_t *plan);
CXX_C_API const char *turbo_agent_executable_plan_tool_name(
    const turbo_agent_executable_plan_t *plan);
CXX_C_API const char *turbo_agent_executable_plan_step_id(
    const turbo_agent_executable_plan_t *plan);
CXX_C_API turbo_agent_template_kind_t turbo_agent_executable_plan_template_kind(
    const turbo_agent_executable_plan_t *plan);
CXX_C_API turbo_tool_execution_policy_t turbo_agent_executable_plan_execution_policy(
    const turbo_agent_executable_plan_t *plan);

CXX_C_API turbo_agent_compile_status_t
turbo_agent_executable_plan_required_capabilities(
    const turbo_agent_executable_plan_t *plan,
    const char *const **out_capabilities,
    size_t *out_count);

/** Return one deterministic, caller-owned JSON certificate for the frozen plan. */
CXX_C_API json_value_t *turbo_agent_executable_plan_certificate_json_value(
    const turbo_agent_executable_plan_t *plan);

#ifdef __cplusplus
}
#endif

#endif
