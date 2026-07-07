#ifndef TURBO_MODEL_H
#define TURBO_MODEL_H

#include <platform.h>
#include <turbo_parser.h>

#include "turbo_runtime_data_bind.h"
#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*turbo_model_user_data_free_fn)(void *user_data);

typedef struct turbo_model_result_s {
  const char *output_text;
  const char *tool_name;
  const char *tool_arguments_json;
} turbo_model_result_t;

typedef struct turbo_model_bind_result_s {
  const char *output_text;
  const char *tool_name;
  turbo_runtime_data_bind_value_t *tool_arguments;
} turbo_model_bind_result_t;

typedef int (*turbo_model_invoke_fn)(void *user_data, const json_value_t *messages,
                                     const turbo_tool_registry_t *tools,
                                     turbo_model_result_t *out_result);
typedef int (*turbo_model_invoke_bind_fn)(void *user_data,
                                          const turbo_runtime_data_bind_value_t *messages,
                                          const turbo_tool_registry_t *tools,
                                          turbo_model_bind_result_t *out_result);

typedef struct turbo_model_s {
  const char *name;
  turbo_model_invoke_fn invoke;
  turbo_model_invoke_bind_fn invoke_bind;
  void *user_data;
  turbo_model_user_data_free_fn user_data_free;
} turbo_model_t;

#ifdef __cplusplus
}
#endif

#endif
