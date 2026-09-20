#ifndef TURBO_TOOL_REGISTRY_H
#define TURBO_TOOL_REGISTRY_H

#include "platform.h"
#include "turbo_runtime_json.h"

#include <stddef.h>

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

typedef enum {
  TURBO_TOOL_EXECUTION_SEQUENTIAL = 0,
  TURBO_TOOL_EXECUTION_PARALLEL_SAFE = 1,
  TURBO_TOOL_EXECUTION_EXCLUSIVE = 2
} turbo_tool_execution_mode_t;

typedef enum {
  TURBO_TOOL_IDEMPOTENCY_NONE = 0,
  TURBO_TOOL_IDEMPOTENCY_KEYED = 1,
  TURBO_TOOL_IDEMPOTENCY_READ_ONLY = 2
} turbo_tool_idempotency_t;

typedef struct turbo_tool_execution_policy_s {
  turbo_tool_execution_mode_t mode;
  turbo_tool_idempotency_t idempotency;
} turbo_tool_execution_policy_t;

typedef int (*turbo_tool_handler_fn)(const char *, char **, void *);
typedef int (*turbo_tool_json_value_handler_fn)(const json_value_t *, json_value_t **, void *);
typedef void (*turbo_tool_user_data_free_fn)(void *);

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

#define TURBO_TOOL_DEFINITION_V3_ABI_VERSION 3u
typedef struct turbo_tool_definition_v3_s {
  size_t struct_size;
  unsigned int abi_version;
  turbo_tool_definition_t definition;
  turbo_tool_execution_policy_t execution_policy;
  const char *const *required_capabilities;
  size_t required_capability_count;
} turbo_tool_definition_v3_t;

turbo_tool_registry_t *turbo_tool_registry_create(void);
void turbo_tool_registry_destroy(turbo_tool_registry_t *registry);
turbo_tool_status_t turbo_tool_registry_add_v3(
    turbo_tool_registry_t *registry, const turbo_tool_definition_v3_t *definition);
size_t turbo_tool_registry_count(const turbo_tool_registry_t *registry);
turbo_tool_status_t turbo_tool_registry_get_execution_policy(
    const turbo_tool_registry_t *registry, const char *name,
    turbo_tool_execution_policy_t *out_policy);
turbo_tool_status_t turbo_tool_registry_get_required_capabilities(
    const turbo_tool_registry_t *registry, const char *name,
    const char *const **out_capabilities, size_t *out_count);
turbo_tool_status_t turbo_tool_registry_execute(
    const turbo_tool_registry_t *registry, const char *name,
    const char *arguments_json, char **out_output);
turbo_tool_status_t turbo_tool_registry_execute_json_value(
    const turbo_tool_registry_t *registry, const char *name,
    const json_value_t *arguments, json_value_t **out_result);

#endif
