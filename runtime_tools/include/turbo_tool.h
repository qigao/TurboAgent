#ifndef TURBO_TOOL_H
#define TURBO_TOOL_H

#include <platform.h>

#include "turbo_runtime_data_bind.h"

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
  TURBO_TOOL_OUT_OF_MEMORY = -5
} turbo_tool_status_t;

typedef int (*turbo_tool_handler_fn)(const char *arguments_json, char **out_output,
                                     void *user_data);
typedef int (*turbo_tool_bind_handler_fn)(const turbo_runtime_data_bind_value_t *arguments,
                                          turbo_runtime_data_bind_value_t **out_result,
                                          void *user_data);
typedef void (*turbo_tool_user_data_free_fn)(void *user_data);

typedef struct turbo_tool_definition_s {
  const char *name;
  const char *description;
  const char *parameters_json;
  const turbo_runtime_data_bind_value_t *parameters_schema;
  int strict;
  turbo_tool_handler_fn handler;
  turbo_tool_bind_handler_fn bind_handler;
  void *user_data;
  turbo_tool_user_data_free_fn user_data_free;
} turbo_tool_definition_t;

#ifdef __cplusplus
}
#endif

#endif
