#ifndef TURBO_TOOL_H
#define TURBO_TOOL_H

#include <stddef.h>
#include <stdint.h>

#include <cmeta/cmeta.h>
#include <cmeta/function.h>
#include <platform.h>

#include "turbo_runtime_json.h"
#include "turbo_runtime_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_tool_registry_s turbo_tool_registry_t;

typedef enum {
  TURBO_TOOL_OK = 0,
  TURBO_TOOL_ERROR = -1,
  TURBO_TOOL_INVALID_ARGUMENT = -2,
  TURBO_TOOL_DUPLICATE = -3,
  TURBO_TOOL_NOT_FOUND = -4,
  TURBO_TOOL_OUT_OF_MEMORY = -5,
  TURBO_TOOL_CANCELLED = -6,
  TURBO_TOOL_DEADLINE_EXCEEDED = -7,
  TURBO_TOOL_OUTPUT_LIMIT = -8,
  TURBO_TOOL_UNKNOWN_SIDE_EFFECT = -9,
  TURBO_TOOL_BACKPRESSURE = -10,
  TURBO_TOOL_FUEL_EXHAUSTED = -11,
  TURBO_TOOL_TRAPPED = -12
} turbo_tool_status_t;

typedef enum turbo_tool_execution_mode_e {
  TURBO_TOOL_EXECUTION_SEQUENTIAL = 0,
  TURBO_TOOL_EXECUTION_PARALLEL_SAFE = 1,
  TURBO_TOOL_EXECUTION_EXCLUSIVE = 2
} turbo_tool_execution_mode_t;

typedef enum turbo_tool_idempotency_e {
  TURBO_TOOL_IDEMPOTENCY_NONE = 0,
  TURBO_TOOL_IDEMPOTENCY_KEYED = 1,
  TURBO_TOOL_IDEMPOTENCY_READ_ONLY = 2
} turbo_tool_idempotency_t;

typedef struct turbo_tool_execution_policy_s {
  turbo_tool_execution_mode_t mode;
  turbo_tool_idempotency_t idempotency;
} turbo_tool_execution_policy_t;

#define TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION_V1 1u
#define TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION 2u

/**
 * Borrowed execution context for one tool invocation.
 *
 * The cancellation token and identity strings remain owned by the caller and
 * are valid only for the duration of the synchronous callback. A zero
 * deadline disables deadline control. Backends may cooperatively check the
 * token and may translate the absolute monotonic deadline into their native
 * timeout representation.
 */
typedef void (*turbo_tool_observation_sink_json_value_fn)(
    const json_value_t *value, void *user_data);

typedef struct turbo_tool_execution_context_s {
  uint32_t struct_size;
  uint32_t abi_version;
  const turbo_cancel_token_t *cancel_token;
  uint64_t deadline_mono_ms;
  const char *thread_id;
  const char *run_id;
  const char *turn_id;
  const char *tool_call_id;

  /**
   * Optional canonical Turbo event sink.
   *
   * Backends may emit progress/trace events while the synchronous tool callback
   * is active. The value is borrowed for the callback duration and the sink
   * must clone it if retention is required.
   */
  turbo_tool_observation_sink_json_value_fn event_sink;
  void *event_sink_user_data;

  /**
   * Optional full-detail sink.
   *
   * A backend that returns a compact model-facing result may publish one
   * structured full-detail object here. Agent execution journals the latest
   * detail independently from the model-facing tool output.
   */
  turbo_tool_observation_sink_json_value_fn detail_sink;
  void *detail_sink_user_data;
} turbo_tool_execution_context_t;

#define TURBO_TOOL_EXECUTION_CONTEXT_V1_SIZE \
  offsetof(turbo_tool_execution_context_t, event_sink)

typedef int (*turbo_tool_handler_fn)(const char *arguments_json, char **out_output,
                                     void *user_data);
typedef int (*turbo_tool_json_value_handler_fn)(const json_value_t *arguments,
                                                json_value_t **out_result, void *user_data);
typedef turbo_tool_status_t (*turbo_tool_context_handler_fn)(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data);
typedef turbo_tool_status_t (*turbo_tool_json_value_context_handler_fn)(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data);
typedef void (*turbo_tool_user_data_free_fn)(void *user_data);

typedef struct turbo_tool_definition_s {
  const char *name;
  const char *description;
  const char *parameters_json;
  const json_value_t *parameters_schema;
  int strict;
  turbo_tool_handler_fn handler;
  turbo_tool_json_value_handler_fn json_value_handler;
  void *user_data;
  turbo_tool_user_data_free_fn user_data_free;
} turbo_tool_definition_t;

#define TURBO_TOOL_DEFINITION_V2_ABI_VERSION 2u

/**
 * Versioned tool metadata. The embedded v1 definition preserves callback and
 * schema ownership rules; legacy registrations behave as sequential,
 * non-idempotent tools.
 */
typedef struct turbo_tool_definition_v2_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
} turbo_tool_definition_v2_t;

#define TURBO_TOOL_DEFINITION_V3_ABI_VERSION 3u

/**
 * Versioned tool metadata with host-policy capability requirements.
 *
 * Capability names are copied by the registry. RuntimeTools deliberately does
 * not interpret them; the policy-aware Agent boundary is responsible for
 * rejecting unknown or denied capabilities before invoking the callback.
 */
typedef struct turbo_tool_definition_v3_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
  const char *const *required_capabilities;
  size_t required_capability_count;
} turbo_tool_definition_v3_t;

#define TURBO_TOOL_DEFINITION_V4_ABI_VERSION 4u

/**
 * Versioned tool definition with context-aware callbacks.
 *
 * The legacy callbacks remain available for direct callers that do not carry
 * execution context. Context-aware execution prefers the v4 callbacks and
 * falls back to the legacy callbacks when they are absent.
 */
typedef struct turbo_tool_definition_v4_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
  const char *const *required_capabilities;
  size_t required_capability_count;
  turbo_tool_context_handler_fn context_handler;
  turbo_tool_json_value_context_handler_fn json_value_context_handler;
} turbo_tool_definition_v4_t;

#define TURBO_TOOL_DEFINITION_V5_ABI_VERSION 5u

/**
 * Additive canonical result contract.
 *
 * result_schema_json is borrowed only for registration; the registry copies it.
 * NULL means the tool result is opaque/unknown. strict_result is descriptive
 * compiler metadata and does not silently enable runtime validation.
 */
typedef struct turbo_tool_definition_v5_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
  const char *const *required_capabilities;
  size_t required_capability_count;
  turbo_tool_context_handler_fn context_handler;
  turbo_tool_json_value_context_handler_fn json_value_context_handler;
  const char *result_schema_json;
  int strict_result;
} turbo_tool_definition_v5_t;

typedef uint64_t turbo_tool_effect_flags_t;

#define TURBO_TOOL_EFFECT_UNKNOWN            (UINT64_C(1) << 0)
#define TURBO_TOOL_EFFECT_PURE               (UINT64_C(1) << 1)
#define TURBO_TOOL_EFFECT_READ               (UINT64_C(1) << 2)
#define TURBO_TOOL_EFFECT_WRITE              (UINT64_C(1) << 3)
#define TURBO_TOOL_EFFECT_PROCESS            (UINT64_C(1) << 4)
#define TURBO_TOOL_EFFECT_NETWORK            (UINT64_C(1) << 5)
#define TURBO_TOOL_EFFECT_EXTERNAL_MUTATION  (UINT64_C(1) << 6)
#define TURBO_TOOL_EFFECT_KNOWN_MASK \
  (TURBO_TOOL_EFFECT_UNKNOWN | TURBO_TOOL_EFFECT_PURE | \
   TURBO_TOOL_EFFECT_READ | TURBO_TOOL_EFFECT_WRITE | \
   TURBO_TOOL_EFFECT_PROCESS | TURBO_TOOL_EFFECT_NETWORK | \
   TURBO_TOOL_EFFECT_EXTERNAL_MUTATION)

#define TURBO_TOOL_DEFINITION_V6_ABI_VERSION 6u

/**
 * Additive canonical semantic effect facts.
 *
 * effect_flags are compiler/optimizer facts and never grant authority.
 * Capabilities remain independently enforced. A zero flag set is normalized
 * by the registry to UNKNOWN. PURE and UNKNOWN are each mutually exclusive
 * with every other effect flag.
 */
typedef struct turbo_tool_definition_v6_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
  const char *const *required_capabilities;
  size_t required_capability_count;
  turbo_tool_context_handler_fn context_handler;
  turbo_tool_json_value_context_handler_fn json_value_context_handler;
  const char *result_schema_json;
  int strict_result;
  turbo_tool_effect_flags_t effect_flags;
} turbo_tool_definition_v6_t;

#define TURBO_TOOL_NATIVE_PROJECTION_ABI_VERSION 1u

/**
 * Optional canonical native execution authority for one RuntimeTool.
 *
 * FunctionDesc/FunctionAbi and every descriptor/provider/code pointer reachable
 * from them are borrowed. The callable value is copied by value; only its inline
 * capture bytes are owned by the receiving registry. Any transitive resource
 * referenced by the capture remains borrowed exactly as defined by CMeta.
 *
 * This projection does not define ownership/lifecycle semantics: FunctionDesc
 * result/parameter flags and DataDesc/type traits remain the canonical source.
 * A dynamic provider must be kept alive by an explicit outer owner/Plugin lease.
 */
typedef struct turbo_tool_native_projection_s {
  size_t struct_size;
  uint32_t abi_version;
  const cmeta_function_desc *function;
  const cmeta_function_abi_desc *abi;
  cmeta_callable callable;
} turbo_tool_native_projection_t;

#ifdef __cplusplus
}
#endif

#endif
