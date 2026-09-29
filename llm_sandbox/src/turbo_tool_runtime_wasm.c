#include "turbo_tool_runtime_wasm.h"

#include "turbo_runtime_json.h"
#include <tstr.h>
#include "turbo_tool_schema.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  TURBO_TOOL_WASM_DEFAULT_MAX_TOOLS = 64,
  TURBO_TOOL_WASM_DEFAULT_METADATA_BYTES = 64 * 1024,
  TURBO_TOOL_WASM_DEFAULT_INPUT_BYTES = 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_OUTPUT_BYTES = 4 * 1024 * 1024
};

typedef struct turbo_tool_runtime_wasm_tool_s {
  char *name;
  char *description;
  char *parameters_json;
  json_value_t *parameters_schema;
  int strict;
} turbo_tool_runtime_wasm_tool_t;

typedef struct turbo_tool_runtime_wasm_output_s {
  char *data;
  size_t size;
  size_t limit;
  int overflow;
} turbo_tool_runtime_wasm_output_t;

typedef struct turbo_tool_runtime_wasm_sink_s {
  uint64_t size;
  uint64_t limit;
  int overflow;
} turbo_tool_runtime_wasm_sink_t;

typedef struct turbo_tool_runtime_wasm_io_s {
  turbo_tool_runtime_wasm_output_t output;
  turbo_tool_runtime_wasm_sink_t error_output;
} turbo_tool_runtime_wasm_io_t;

typedef struct turbo_tool_runtime_wasm_impl_s {
  turbo_wasm_vm_t *vm;
  turbo_tool_runtime_wasm_tool_t *tools;
  size_t tool_count;
  size_t max_metadata_bytes;
  size_t max_input_bytes;
  size_t max_output_bytes;
} turbo_tool_runtime_wasm_impl_t;

static void turbo_tool_runtime_wasm_tool_clear(turbo_tool_runtime_wasm_tool_t *tool) {
  if (!tool) return;
  tstr_free(tool->name);
  tstr_free(tool->description);
  tstr_free(tool->parameters_json);
  turbo_runtime_json_destroy(tool->parameters_schema);
  memset(tool, 0, sizeof(*tool));
}

static void turbo_tool_runtime_wasm_destroy_impl(void *impl) {
  turbo_tool_runtime_wasm_impl_t *wasm_impl = (turbo_tool_runtime_wasm_impl_t *)impl;
  size_t i;

  if (!wasm_impl) return;
  for (i = 0; i < wasm_impl->tool_count; ++i)
    turbo_tool_runtime_wasm_tool_clear(&wasm_impl->tools[i]);
  free(wasm_impl->tools);
  turbo_wasm_vm_destroy(wasm_impl->vm);
  free(wasm_impl);
}

static int turbo_tool_runtime_wasm_collect(const uint8_t *data, size_t size, void *user_data) {
  turbo_tool_runtime_wasm_io_t *io = (turbo_tool_runtime_wasm_io_t *)user_data;
  turbo_tool_runtime_wasm_output_t *output = io ? &io->output : NULL;

  if (!output || (size && !data) || size > output->limit - output->size) {
    if (output) output->overflow = 1;
    return -1;
  }
  if (size) memcpy(output->data + output->size, data, size);
  output->size += size;
  output->data[output->size] = '\0';
  return 0;
}

static int turbo_tool_runtime_wasm_discard(const uint8_t *data, size_t size, void *user_data) {
  turbo_tool_runtime_wasm_io_t *io = (turbo_tool_runtime_wasm_io_t *)user_data;
  turbo_tool_runtime_wasm_sink_t *sink = io ? &io->error_output : NULL;
  (void)data;
  if (!sink || size > sink->limit - sink->size) {
    if (sink) sink->overflow = 1;
    return -1;
  }
  sink->size += size;
  return 0;
}

static int turbo_tool_runtime_wasm_call(turbo_tool_runtime_wasm_impl_t *impl,
                                        const char *export_name, int32_t index, int has_index,
                                        const char *input, size_t output_limit, char **out_output,
                                        int32_t *out_guest_status) {
  turbo_wasm_app_io_t app_io = {0};
  turbo_tool_runtime_wasm_io_t io = {0};
  turbo_wasm_value_t arg = {0};
  turbo_wasm_value_t result = {0};
  size_t input_size = input ? strlen(input) : 0;
  int rc;

  if (!impl || !export_name || !out_output || !out_guest_status || output_limit == 0 ||
      output_limit == SIZE_MAX)
    return -1;
  *out_output = NULL;
  *out_guest_status = -1;
  io.output.data = (char *)malloc(output_limit + 1);
  if (!io.output.data) return -1;
  io.output.limit = output_limit;
  io.output.data[0] = '\0';
  io.error_output.limit = output_limit;

  app_io.struct_size = sizeof(app_io);
  app_io.input = (const uint8_t *)input;
  app_io.input_size = input_size;
  app_io.stdout_bytes = output_limit;
  app_io.stderr_bytes = output_limit;
  app_io.write_stdout = turbo_tool_runtime_wasm_collect;
  app_io.write_stderr = turbo_tool_runtime_wasm_discard;
  app_io.user_data = &io;
  rc = turbo_wasm_vm_set_app_io(impl->vm, &app_io);
  if (rc != TURBO_WASM_OK) goto fail;

  arg.type = TURBO_WASM_VALUE_I32;
  arg.value.i32 = (uint32_t)index;
  result.type = TURBO_WASM_VALUE_I32;
  rc = turbo_wasm_vm_call(impl->vm, export_name, has_index ? &arg : NULL, has_index ? 1u : 0u,
                          &result, 1u);
  (void)turbo_wasm_vm_clear_app_io(impl->vm);
  if (rc != TURBO_WASM_OK || io.output.overflow || io.error_output.overflow) goto fail;

  *out_guest_status = (int32_t)result.value.i32;
  *out_output = io.output.data;
  return 0;

fail:
  (void)turbo_wasm_vm_clear_app_io(impl->vm);
  free(io.output.data);
  return -1;
}

static size_t turbo_tool_runtime_wasm_tool_count(const void *impl) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl = (const turbo_tool_runtime_wasm_impl_t *)impl;
  return wasm_impl ? wasm_impl->tool_count : 0;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_get_tool(const void *impl, size_t index,
                                                            turbo_tool_runtime_tool_t *out_tool) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl = (const turbo_tool_runtime_wasm_impl_t *)impl;
  const turbo_tool_runtime_wasm_tool_t *tool;

  if (!wasm_impl || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  if (index >= wasm_impl->tool_count) return TURBO_TOOL_NOT_FOUND;
  tool = &wasm_impl->tools[index];
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->name = tool->name;
  out_tool->description = tool->description;
  out_tool->parameters_json = tool->parameters_json;
  out_tool->parameters_schema = tool->parameters_schema;
  out_tool->strict = tool->strict;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke(void *impl, const char *name,
                                                          const char *arguments_json,
                                                          char **out_output) {
  turbo_tool_runtime_wasm_impl_t *wasm_impl = (turbo_tool_runtime_wasm_impl_t *)impl;
  const char *input = arguments_json ? arguments_json : "{}";
  int32_t guest_status;
  size_t i;

  if (!wasm_impl || !name || !out_output) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_output = NULL;
  if (strlen(input) > wasm_impl->max_input_bytes) return TURBO_TOOL_ERROR;
  for (i = 0; i < wasm_impl->tool_count; ++i)
    if (strcmp(wasm_impl->tools[i].name, name) == 0) break;
  if (i == wasm_impl->tool_count) return TURBO_TOOL_NOT_FOUND;
  if (turbo_tool_runtime_wasm_call(wasm_impl, "turbo_tool_invoke", (int32_t)i, 1, input,
                                   wasm_impl->max_output_bytes, out_output, &guest_status) != 0 ||
      guest_status != 0) {
    free(*out_output);
    *out_output = NULL;
    return TURBO_TOOL_ERROR;
  }
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke_json_value(void *impl, const char *name,
                                                                     const json_value_t *arguments,
                                                                     json_value_t **out_result) {
  char *arguments_json = NULL;
  char *output_json = NULL;
  json_value_t *parsed = NULL;
  turbo_tool_status_t status;

  if (!out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_result = NULL;
  if (arguments) {
    arguments_json = turbo_json_serialize(arguments, NULL);
    if (!arguments_json) return TURBO_TOOL_ERROR;
  }
  status = turbo_tool_runtime_wasm_invoke(impl, name, arguments_json, &output_json);
  turbo_json_serialize_free(arguments_json);
  if (status != TURBO_TOOL_OK) return status;
  if (turbo_parse_json((const uint8_t *)output_json, strlen(output_json), &parsed) != 0) {
    free(output_json);
    return TURBO_TOOL_ERROR;
  }
  free(output_json);
  *out_result = parsed;
  return TURBO_TOOL_OK;
}

static const turbo_tool_runtime_vtable_t turbo_tool_runtime_wasm_vtable = {
    turbo_tool_runtime_wasm_destroy_impl, turbo_tool_runtime_wasm_tool_count,
    turbo_tool_runtime_wasm_get_tool, turbo_tool_runtime_wasm_invoke,
    turbo_tool_runtime_wasm_invoke_json_value};

static int turbo_tool_runtime_wasm_load_tool(turbo_tool_runtime_wasm_impl_t *impl, size_t index) {
  turbo_tool_runtime_wasm_tool_t *tool = &impl->tools[index];
  json_value_t *metadata = NULL;
  json_value_t *parameters;
  json_value_t *strict;
  const char *name;
  const char *description;
  char *metadata_json = NULL;
  char *parameters_json = NULL;
  int32_t guest_status;
  size_t prior_index;
  int rc = -1;

  if (turbo_tool_runtime_wasm_call(impl, "turbo_tool_describe", (int32_t)index, 1, NULL,
                                   impl->max_metadata_bytes, &metadata_json, &guest_status) != 0 ||
      guest_status != 0 || !metadata_json[0])
    goto cleanup;
  if (turbo_parse_json((const uint8_t *)metadata_json, strlen(metadata_json), &metadata) != 0 ||
      turbo_json_type(metadata) != TURBO_JSON_OBJECT)
    goto cleanup;
  name = turbo_json_get_string(metadata, "name");
  description = turbo_json_get_string(metadata, "description");
  parameters = turbo_json_object_get(metadata, "parameters");
  strict = turbo_json_object_get(metadata, "strict");
  if (!name || !name[0] || !description || !parameters ||
      turbo_json_type(parameters) != TURBO_JSON_OBJECT || !strict ||
      turbo_json_type(strict) != TURBO_JSON_BOOL)
    goto cleanup;
  parameters_json = turbo_json_serialize(parameters, NULL);
  if (!parameters_json) goto cleanup;
  tool->name = (char *)tstr_dup(name);
  tool->description = (char *)tstr_dup(description);
  tool->parameters_json = (char *)tstr_dup(parameters_json);
  tool->parameters_schema = turbo_tool_schema_parse_parameters_json_value(parameters_json, 0);
  tool->strict = turbo_json_bool(strict) ? 1 : 0;
  if (!tool->name || !tool->description || !tool->parameters_json || !tool->parameters_schema)
    goto cleanup;
  for (prior_index = 0; prior_index < index; ++prior_index)
    if (strcmp(impl->tools[prior_index].name, tool->name) == 0) goto cleanup;
  rc = 0;

cleanup:
  if (rc != 0) turbo_tool_runtime_wasm_tool_clear(tool);
  turbo_json_serialize_free(parameters_json);
  turbo_runtime_json_destroy(metadata);
  free(metadata_json);
  return rc;
}

void turbo_tool_runtime_wasm_config_init(turbo_tool_runtime_wasm_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_TOOL_RUNTIME_WASM_ABI_VERSION;
  config->max_tools = TURBO_TOOL_WASM_DEFAULT_MAX_TOOLS;
  config->max_metadata_bytes = TURBO_TOOL_WASM_DEFAULT_METADATA_BYTES;
  config->max_input_bytes = TURBO_TOOL_WASM_DEFAULT_INPUT_BYTES;
  config->max_output_bytes = TURBO_TOOL_WASM_DEFAULT_OUTPUT_BYTES;
}

turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create(const turbo_tool_runtime_wasm_config_t *config) {
  static const turbo_wasm_value_type_t i32_result[] = {TURBO_WASM_VALUE_I32};
  static const turbo_wasm_value_type_t i32_arg[] = {TURBO_WASM_VALUE_I32};
  turbo_tool_runtime_wasm_impl_t *impl = NULL;
  turbo_wasm_value_t count_result = {0};
  int32_t count;
  size_t i;

  if (!config || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_TOOL_RUNTIME_WASM_ABI_VERSION || !config->module_path ||
      !config->module_path[0] || !config->policy || !config->max_tools ||
      !config->max_metadata_bytes || !config->max_input_bytes || !config->max_output_bytes ||
      config->max_tools > INT32_MAX)
    return NULL;
  impl = (turbo_tool_runtime_wasm_impl_t *)calloc(1, sizeof(*impl));
  if (!impl) return NULL;
  impl->max_metadata_bytes = config->max_metadata_bytes;
  impl->max_input_bytes = config->max_input_bytes;
  impl->max_output_bytes = config->max_output_bytes;
  impl->vm = turbo_wasm_vm_create(config->policy, NULL);
  if (!impl->vm || turbo_wasm_vm_load_file(impl->vm, config->module_path) != TURBO_WASM_OK ||
      turbo_wasm_vm_check_export(impl->vm, "turbo_tool_count", NULL, 0, i32_result, 1) !=
          TURBO_WASM_OK ||
      turbo_wasm_vm_check_export(impl->vm, "turbo_tool_describe", i32_arg, 1, i32_result, 1) !=
          TURBO_WASM_OK ||
      turbo_wasm_vm_check_export(impl->vm, "turbo_tool_invoke", i32_arg, 1, i32_result, 1) !=
          TURBO_WASM_OK)
    goto fail;
  count_result.type = TURBO_WASM_VALUE_I32;
  if (turbo_wasm_vm_call(impl->vm, "turbo_tool_count", NULL, 0, &count_result, 1) != TURBO_WASM_OK)
    goto fail;
  count = (int32_t)count_result.value.i32;
  if (count < 0 || (size_t)count > config->max_tools) goto fail;
  impl->tool_count = (size_t)count;
  if (impl->tool_count) {
    impl->tools = (turbo_tool_runtime_wasm_tool_t *)calloc(impl->tool_count, sizeof(*impl->tools));
    if (!impl->tools) goto fail;
    for (i = 0; i < impl->tool_count; ++i)
      if (turbo_tool_runtime_wasm_load_tool(impl, i) != 0) goto fail;
  }
  return turbo_tool_runtime_create(&turbo_tool_runtime_wasm_vtable, impl);

fail:
  turbo_tool_runtime_wasm_destroy_impl(impl);
  return NULL;
}
