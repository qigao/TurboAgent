#include "turbo_tool_runtime_wasm3.h"
#include "turbo_tool_schema.h"
#include "turbo_str.h"
#include "turbo_wasm3.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_tool_runtime_wasm3_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }
  len = strlen(src);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len + 1);
  return copy;
}

typedef struct turbo_tool_runtime_wasm3_tool_s {
  char *name;
  char *description;
  char *parameters_json;
  turbo_runtime_data_bind_value_t *parameters_schema;
  int strict;
} turbo_tool_runtime_wasm3_tool_t;

typedef struct turbo_tool_runtime_wasm3_impl_s {
  turbo_wasm3_vm_t *vm;
  turbo_tool_runtime_wasm3_tool_t *tools;
  size_t tool_count;
  IM3Function count_fn;
  IM3Function name_fn;
  IM3Function description_fn;
  IM3Function parameters_fn;
  IM3Function strict_fn;
  IM3Function input_ptr_fn;
  IM3Function input_capacity_fn;
  IM3Function output_ptr_fn;
  IM3Function output_capacity_fn;
  IM3Function invoke_fn;
} turbo_tool_runtime_wasm3_impl_t;



static void turbo_tool_runtime_wasm3_tool_free(turbo_tool_runtime_wasm3_tool_t *tool) {
  if (!tool) {
    return;
  }

  free(tool->name);
  free(tool->description);
  free(tool->parameters_json);
  turbo_runtime_data_bind_value_destroy(tool->parameters_schema);
  memset(tool, 0, sizeof(*tool));
}

static void turbo_tool_runtime_wasm3_destroy_impl(void *impl) {
  turbo_tool_runtime_wasm3_impl_t *wasm_impl =
      (turbo_tool_runtime_wasm3_impl_t *)impl;
  size_t i;

  if (!wasm_impl) {
    return;
  }

  for (i = 0; i < wasm_impl->tool_count; ++i) {
    turbo_tool_runtime_wasm3_tool_free(&wasm_impl->tools[i]);
  }

  free(wasm_impl->tools);
  turbo_wasm3_vm_destroy(wasm_impl->vm);
  free(wasm_impl);
}

static uint8_t *turbo_tool_runtime_wasm3_memory(turbo_tool_runtime_wasm3_impl_t *impl,
                                                uint32_t *out_size) {
  if (!impl || !impl->vm) {
    return NULL;
  }

  return m3_GetMemory(turbo_wasm3_vm_get_runtime(impl->vm), out_size, 0);
}

static int turbo_tool_runtime_wasm3_call_i32(IM3Function fn, int32_t arg0, int has_arg0,
                                             int32_t arg1, int has_arg1,
                                             int32_t *out_value) {
  M3Result result;
  int32_t value = 0;

  if (!fn || !out_value) {
    return -1;
  }

  if (has_arg0 && has_arg1) {
    result = m3_CallV(fn, arg0, arg1);
  } else if (has_arg0) {
    result = m3_CallV(fn, arg0);
  } else {
    result = m3_CallV(fn);
  }

  if (result) {
    return -1;
  }

  result = m3_GetResultsV(fn, &value);
  if (result) {
    return -1;
  }

  *out_value = value;
  return 0;
}

static int turbo_tool_runtime_wasm3_find_required(IM3Runtime runtime, const char *name,
                                                  IM3Function *out_fn) {
  return m3_FindFunction(out_fn, runtime, name) == NULL ? 0 : -1;
}

static char *turbo_tool_runtime_wasm3_read_string(turbo_tool_runtime_wasm3_impl_t *impl,
                                                  int32_t offset) {
  uint32_t memory_size = 0;
  uint8_t *memory;
  size_t len = 0;

  if (!impl || offset < 0) {
    return NULL;
  }

  memory = turbo_tool_runtime_wasm3_memory(impl, &memory_size);
  if (!memory || (uint32_t)offset >= memory_size) {
    return NULL;
  }

  while ((uint32_t)offset + len < memory_size && memory[offset + len] != '\0') {
    len++;
  }

  if ((uint32_t)offset + len >= memory_size) {
    return NULL;
  }

  return turbo_tool_runtime_wasm3_strdup((const char *)(memory + offset));
}

static size_t turbo_tool_runtime_wasm3_tool_count(const void *impl) {
  const turbo_tool_runtime_wasm3_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm3_impl_t *)impl;

  return wasm_impl ? wasm_impl->tool_count : 0;
}

static turbo_tool_status_t
turbo_tool_runtime_wasm3_get_tool(const void *impl, size_t index,
                                  turbo_tool_runtime_tool_t *out_tool) {
  const turbo_tool_runtime_wasm3_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm3_impl_t *)impl;
  const turbo_tool_runtime_wasm3_tool_t *tool;

  if (!wasm_impl || !out_tool) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  if (index >= wasm_impl->tool_count) {
    return TURBO_TOOL_NOT_FOUND;
  }

  tool = &wasm_impl->tools[index];
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->name = tool->name;
  out_tool->description = tool->description;
  out_tool->parameters_json = tool->parameters_json;
  out_tool->parameters_schema = tool->parameters_schema;
  out_tool->strict = tool->strict;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t
turbo_tool_runtime_wasm3_invoke(void *impl, const char *name,
                                const char *arguments_json, char **out_output) {
  turbo_tool_runtime_wasm3_impl_t *wasm_impl =
      (turbo_tool_runtime_wasm3_impl_t *)impl;
  size_t i;
  int32_t input_ptr;
  int32_t input_capacity;
  int32_t output_ptr;
  int32_t output_capacity;
  int32_t output_len;
  uint32_t memory_size = 0;
  uint8_t *memory;
  size_t input_len;
  char *output;

  if (!wasm_impl || !name || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_output = NULL;
  input_len = strlen(arguments_json ? arguments_json : "{}");

  for (i = 0; i < wasm_impl->tool_count; ++i) {
    if (strcmp(wasm_impl->tools[i].name, name) == 0) {
      break;
    }
  }

  if (i == wasm_impl->tool_count) {
    return TURBO_TOOL_NOT_FOUND;
  }

  if (turbo_tool_runtime_wasm3_call_i32(wasm_impl->input_ptr_fn, 0, 0, 0, 0,
                                        &input_ptr) != 0 ||
      turbo_tool_runtime_wasm3_call_i32(wasm_impl->input_capacity_fn, 0, 0, 0, 0,
                                        &input_capacity) != 0 ||
      turbo_tool_runtime_wasm3_call_i32(wasm_impl->output_ptr_fn, 0, 0, 0, 0,
                                        &output_ptr) != 0 ||
      turbo_tool_runtime_wasm3_call_i32(wasm_impl->output_capacity_fn, 0, 0, 0, 0,
                                        &output_capacity) != 0) {
    return TURBO_TOOL_ERROR;
  }

  if (input_ptr < 0 || input_capacity < 0 || output_ptr < 0 || output_capacity < 0) {
    return TURBO_TOOL_ERROR;
  }

  if (input_len > (size_t)input_capacity) {
    return TURBO_TOOL_ERROR;
  }

  memory = turbo_tool_runtime_wasm3_memory(wasm_impl, &memory_size);
  if (!memory) {
    return TURBO_TOOL_ERROR;
  }

  if ((uint32_t)input_ptr + input_len > memory_size ||
      (uint32_t)output_ptr + (uint32_t)output_capacity > memory_size) {
    return TURBO_TOOL_ERROR;
  }

  memcpy(memory + input_ptr, arguments_json ? arguments_json : "{}", input_len);
  if ((size_t)input_capacity > input_len) {
    memory[input_ptr + input_len] = '\0';
  }

  if (turbo_tool_runtime_wasm3_call_i32(wasm_impl->invoke_fn, (int32_t)i, 1,
                                        (int32_t)input_len, 1, &output_len) != 0) {
    return TURBO_TOOL_ERROR;
  }

  if (output_len < 0 || output_len > output_capacity ||
      (uint32_t)output_ptr + (uint32_t)output_len > memory_size) {
    return TURBO_TOOL_ERROR;
  }

  output = (char *)malloc((size_t)output_len + 1);
  if (!output) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  if (output_len > 0) {
    memcpy(output, memory + output_ptr, (size_t)output_len);
  }
  output[output_len] = '\0';

  *out_output = output;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t
turbo_tool_runtime_wasm3_invoke_bind(void *impl, const char *name,
                                     const turbo_runtime_data_bind_value_t *arguments,
                                     turbo_runtime_data_bind_value_t **out_result) {
  json_value_t *arguments_json_value = NULL;
  char *arguments_json = NULL;
  char *output_json = NULL;
  json_value_t *parsed = NULL;
  turbo_runtime_data_bind_value_t *bound = NULL;
  turbo_tool_status_t status;

  if (!out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_result = NULL;
  if (arguments) {
    arguments_json_value = turbo_runtime_data_bind_value_to_json(arguments);
    if (!arguments_json_value) {
      return TURBO_TOOL_ERROR;
    }
    arguments_json = turbo_json_serialize(arguments_json_value, NULL);
    turbo_free_json(&arguments_json_value);
    if (!arguments_json) {
      return TURBO_TOOL_ERROR;
    }
  }

  status = turbo_tool_runtime_wasm3_invoke(impl, name, arguments_json, &output_json);
  turbo_json_serialize_free(arguments_json);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

  if (turbo_parse_json((const uint8_t *)output_json, strlen(output_json), &parsed) != 0) {
    free(output_json);
    return TURBO_TOOL_ERROR;
  }
  free(output_json);

  bound = turbo_runtime_data_bind_value_from_json(parsed);
  turbo_free_json(&parsed);
  if (!bound) {
    return TURBO_TOOL_ERROR;
  }

  *out_result = bound;
  return TURBO_TOOL_OK;
}

static const turbo_tool_runtime_vtable_t turbo_tool_runtime_wasm3_vtable = {
    turbo_tool_runtime_wasm3_destroy_impl,
    turbo_tool_runtime_wasm3_tool_count,
    turbo_tool_runtime_wasm3_get_tool,
    turbo_tool_runtime_wasm3_invoke,
    turbo_tool_runtime_wasm3_invoke_bind};

static int turbo_tool_runtime_wasm3_load_metadata(turbo_tool_runtime_wasm3_impl_t *impl) {
  IM3Runtime runtime;
  int32_t tool_count;
  size_t i;

  runtime = turbo_wasm3_vm_get_runtime(impl->vm);
  if (turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_count",
                                             &impl->count_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_name",
                                             &impl->name_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_description",
                                             &impl->description_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_parameters",
                                             &impl->parameters_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_strict",
                                             &impl->strict_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_input_ptr",
                                             &impl->input_ptr_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_input_capacity",
                                             &impl->input_capacity_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_output_ptr",
                                             &impl->output_ptr_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_output_capacity",
                                             &impl->output_capacity_fn) != 0 ||
      turbo_tool_runtime_wasm3_find_required(runtime, "turbo_tool_invoke",
                                             &impl->invoke_fn) != 0) {
    return -1;
  }

  if (turbo_tool_runtime_wasm3_call_i32(impl->count_fn, 0, 0, 0, 0, &tool_count) != 0 ||
      tool_count < 0) {
    return -1;
  }

  impl->tool_count = (size_t)tool_count;
  if (impl->tool_count == 0) {
    return 0;
  }

  impl->tools = (turbo_tool_runtime_wasm3_tool_t *)calloc(impl->tool_count,
                                                          sizeof(*impl->tools));
  if (!impl->tools) {
    return -1;
  }

  for (i = 0; i < impl->tool_count; ++i) {
    int32_t ptr_value;

    if (turbo_tool_runtime_wasm3_call_i32(impl->name_fn, (int32_t)i, 1, 0, 0,
                                          &ptr_value) != 0) {
      return -1;
    }
    impl->tools[i].name = turbo_tool_runtime_wasm3_read_string(impl, ptr_value);

    if (turbo_tool_runtime_wasm3_call_i32(impl->description_fn, (int32_t)i, 1, 0, 0,
                                          &ptr_value) != 0) {
      return -1;
    }
    impl->tools[i].description = turbo_tool_runtime_wasm3_read_string(impl, ptr_value);

    if (turbo_tool_runtime_wasm3_call_i32(impl->parameters_fn, (int32_t)i, 1, 0, 0,
                                          &ptr_value) != 0) {
      return -1;
    }
    impl->tools[i].parameters_json =
        turbo_tool_runtime_wasm3_read_string(impl, ptr_value);
    if (impl->tools[i].parameters_json) {
      impl->tools[i].parameters_schema = turbo_tool_schema_parse_parameters_bind(
          impl->tools[i].parameters_json, 0);
    }

    if (turbo_tool_runtime_wasm3_call_i32(impl->strict_fn, (int32_t)i, 1, 0, 0,
                                          &ptr_value) != 0) {
      return -1;
    }
    impl->tools[i].strict = ptr_value ? 1 : 0;

    if (!impl->tools[i].name || !impl->tools[i].description ||
        !impl->tools[i].parameters_json || !impl->tools[i].parameters_schema) {
      return -1;
    }
  }

  return 0;
}

turbo_tool_runtime_t *
turbo_tool_runtime_wasm3_create(const turbo_tool_runtime_wasm3_config_t *config) {
  turbo_tool_runtime_wasm3_impl_t *impl;
  const char *module_name;
  uint32_t stack_size;
  size_t socket_capacity;

  if (!config || !config->module_path || config->module_path[0] == '\0') {
    return NULL;
  }

  impl = (turbo_tool_runtime_wasm3_impl_t *)calloc(1, sizeof(*impl));
  if (!impl) {
    return NULL;
  }

  stack_size = config->stack_size != 0 ? config->stack_size : 64 * 1024u;
  socket_capacity = config->socket_capacity != 0 ? config->socket_capacity : 8u;
  module_name = config->module_name && config->module_name[0] != '\0'
                    ? config->module_name
                    : "turbo_tool_guest";

  impl->vm = turbo_wasm3_vm_create(stack_size, NULL, socket_capacity);
  if (!impl->vm) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  if (config->enable_turbonet_host &&
      turbo_wasm3_vm_enable_host(impl->vm) != 0) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  if (config->enable_http_host &&
      turbo_wasm3_vm_enable_http_host(impl->vm) != 0) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  if (config->enable_redis_host &&
      turbo_wasm3_vm_enable_redis_host(impl->vm) != 0) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  if (turbo_wasm3_vm_load_module_file(impl->vm, config->module_path, module_name,
                                      NULL) != NULL) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  if (turbo_tool_runtime_wasm3_load_metadata(impl) != 0) {
    turbo_tool_runtime_wasm3_destroy_impl(impl);
    return NULL;
  }

  return turbo_tool_runtime_create(&turbo_tool_runtime_wasm3_vtable, impl);
}

turbo_tool_runtime_t *
turbo_tool_runtime_default_create(const turbo_tool_runtime_wasm3_config_t *config) {
  return turbo_tool_runtime_wasm3_create(config);
}
