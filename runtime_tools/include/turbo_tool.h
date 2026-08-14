#ifndef TURBO_TOOL_H
#define TURBO_TOOL_H

#include <platform.h>

#include "turbo_runtime_json.h"

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
  TURBO_TOOL_BACKPRESSURE = -10
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

typedef int (*turbo_tool_handler_fn)(const char *arguments_json, char **out_output,
                                     void *user_data);
typedef int (*turbo_tool_json_value_handler_fn)(const json_value_t *arguments,
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

#ifdef __cplusplus
}
#endif

#endif
