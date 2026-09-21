#ifndef TURBO_ACTION_TOOL_H
#define TURBO_ACTION_TOOL_H

#include <platform.h>
#include "turbo_tool_registry.h"
#include <json_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_action_tool_registry_s turbo_action_tool_registry_t;

typedef enum {
  TURBO_ACTION_TOOL_OK = 0,
  TURBO_ACTION_TOOL_ERROR = -1,
  TURBO_ACTION_TOOL_INVALID_ARGUMENT = -2,
  TURBO_ACTION_TOOL_DUPLICATE = -3,
  TURBO_ACTION_TOOL_NOT_FOUND = -4,
  TURBO_ACTION_TOOL_OUT_OF_MEMORY = -5,
  TURBO_ACTION_TOOL_POLICY_DENIED = -6,
  TURBO_ACTION_TOOL_APPROVAL_REQUIRED = -7
} turbo_action_tool_status_t;

typedef enum {
  TURBO_ACTION_OBSERVE = 0,
  TURBO_ACTION_MUTATE = 1,
  TURBO_ACTION_DANGEROUS = 2
} turbo_action_kind_t;

typedef int (*turbo_action_tool_handler_fn)(const json_value_t *args, json_value_t **out_result,
                                            void *user_data);

typedef struct turbo_action_tool_definition_s {
  const char *name;
  const char *description;
  const char *parameters_json;
  turbo_action_kind_t kind;
  int requires_approval;
  int idempotent;
  unsigned timeout_ms;
  turbo_action_tool_handler_fn handler;
  void *user_data;
} turbo_action_tool_definition_t;

CXX_C_API turbo_action_tool_registry_t *turbo_action_tool_registry_create(void);
CXX_C_API void turbo_action_tool_registry_destroy(turbo_action_tool_registry_t *registry);
CXX_C_API turbo_action_tool_status_t
turbo_action_tool_registry_add(turbo_action_tool_registry_t *registry,
                               const turbo_action_tool_definition_t *definition);
CXX_C_API size_t
turbo_action_tool_registry_count(const turbo_action_tool_registry_t *registry);
CXX_C_API const turbo_action_tool_definition_t *
turbo_action_tool_registry_find(const turbo_action_tool_registry_t *registry, const char *name);
CXX_C_API turbo_action_tool_status_t
turbo_action_tool_registry_execute(const turbo_action_tool_registry_t *registry, const char *name,
                                   const json_value_t *args, json_value_t **out_result);
CXX_C_API json_value_t *
turbo_action_tool_registry_build_openai_chat_tools(const turbo_action_tool_registry_t *registry);
/**
 * @brief Build a legacy tool registry bridge from the current action definitions.
 *
 * The bridge copies tool metadata and handler pointers. The source action
 * registry may be destroyed after this call; handler user_data remains
 * caller-owned just like it is for the action registry itself.
 */
CXX_C_API turbo_tool_registry_t *
turbo_action_tool_registry_build_tool_registry_bridge(
    const turbo_action_tool_registry_t *registry);

CXX_C_API json_value_t *turbo_action_result_create(int ok, const char *summary);
CXX_C_API int turbo_action_result_set_command_fields(json_value_t *result, int exit_code,
                                                     const char *stdout_text,
                                                     const char *stderr_text);
CXX_C_API int turbo_action_result_add_changed_file(json_value_t *result, const char *path);
CXX_C_API int turbo_action_result_set_retryable(json_value_t *result, int retryable);
CXX_C_API int turbo_action_result_validate(const json_value_t *result);

#ifdef __cplusplus
}
#endif

#endif
