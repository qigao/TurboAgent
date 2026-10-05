#include "turbo_tool_runtime_wasm.h"

#include "turbo_runtime_json.h"
#include "turbo_wasm_invoke_gate.h"
#include "turbo_tool_schema.h"

#include <salts/clock.h>
#include <salts/thread.h>
#include <salts_fs.h>
#include <tstr.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TURBO_TOOL_WASM_DEFAULT_MAX_TOOLS = 64,
  TURBO_TOOL_WASM_DEFAULT_METADATA_BYTES = 64 * 1024,
  TURBO_TOOL_WASM_DEFAULT_INPUT_BYTES = 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_OUTPUT_BYTES = 4 * 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_MODULE_BYTES = 16 * 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_ALLOCATION_BYTES = 64 * 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_LINEAR_MEMORY_BYTES = 64 * 1024 * 1024,
  TURBO_TOOL_WASM_DEFAULT_TABLE_ELEMENTS = 65536
};

#define TURBO_TOOL_WASM_DEFAULT_FUEL UINT64_C(10000000)
#define TURBO_TOOL_WASM_FNV_OFFSET UINT64_C(14695981039346656037)
#define TURBO_TOOL_WASM_FNV_PRIME UINT64_C(1099511628211)

typedef struct turbo_tool_runtime_wasm_tool_s {
  char *name;
  char *description;
  char *parameters_json;
  json_value_t *parameters_schema;
  int strict;
  char *result_schema_json;
  json_value_t *result_schema;
  int strict_result;
  turbo_tool_effect_flags_t effect_flags;
} turbo_tool_runtime_wasm_tool_t;

typedef struct turbo_tool_runtime_wasm_io_s {
  const uint8_t *input;
  size_t input_size;
  char *output;
  size_t output_size;
  size_t output_limit;
  int output_overflow;
} turbo_tool_runtime_wasm_io_t;

typedef enum turbo_tool_runtime_wasm_interrupt_reason_e {
  TURBO_TOOL_WASM_INTERRUPT_NONE = 0,
  TURBO_TOOL_WASM_INTERRUPT_CANCELLED,
  TURBO_TOOL_WASM_INTERRUPT_DEADLINE
} turbo_tool_runtime_wasm_interrupt_reason_t;

typedef struct turbo_tool_runtime_wasm_control_s {
  const turbo_tool_execution_context_t *context;
  turbo_tool_runtime_wasm_interrupt_reason_t reason;
} turbo_tool_runtime_wasm_control_t;

typedef struct turbo_tool_runtime_wasm_impl_s {
  turbo_wasm_invoke_gate_t invoke_gate;
  salts_fs_buf_t module_bytes;
  turbowasm_module module;
  turbowasm_instance instance;

  uint32_t export_tool_count;
  uint32_t export_tool_describe;
  uint32_t export_tool_invoke;

  turbo_tool_runtime_wasm_tool_t *tools;
  size_t tool_count;

  size_t max_metadata_bytes;
  size_t max_input_bytes;
  size_t max_output_bytes;
  uint64_t fuel_per_call;

  turbo_tool_runtime_wasm_io_t *active_io;
} turbo_tool_runtime_wasm_impl_t;

static turbowasm_name turbo_tool_runtime_wasm_name(const char *text) {
  turbowasm_name name = {0};
  if (!text) return name;
  name.bytes = (const uint8_t *)text;
  name.size = (uint32_t)strlen(text);
  return name;
}

static int turbo_tool_runtime_wasm_name_equal(turbowasm_name name, const char *text) {
  size_t length;
  if (!text) return 0;
  length = strlen(text);
  return length == name.size &&
         (length == 0 || memcmp(name.bytes, text, length) == 0);
}

static uint64_t turbo_tool_runtime_wasm_module_hash(
    const void *data, size_t size) {
  const unsigned char *bytes = (const unsigned char *)data;
  uint64_t hash = TURBO_TOOL_WASM_FNV_OFFSET;
  size_t i;
  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)bytes[i];
    hash *= TURBO_TOOL_WASM_FNV_PRIME;
  }
  return hash;
}

static int turbo_tool_runtime_wasm_set_u64_string(
    json_value_t *object, const char *name, uint64_t value) {
  char text[32];
  json_value_t *field;
  snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
  field = json_create_string(text);
  if (!field) return 0;
  if (turbo_runtime_json_object_set(object, name, field) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return 0;
  }
  return 1;
}

static json_value_t *turbo_tool_runtime_wasm_execution_metadata(
    const turbo_tool_runtime_wasm_impl_t *impl,
    const turbo_tool_runtime_wasm_config_t *config) {
  json_value_t *root = NULL;
  json_value_t *limits = NULL;
  json_value_t *field = NULL;
  char identity[32];
  uint64_t hash;

  if (!impl || !config || !impl->module_bytes.base) return NULL;
  hash = turbo_tool_runtime_wasm_module_hash(
      impl->module_bytes.base, impl->module_bytes.len);

  root = json_create_object();
  limits = json_create_object();
  if (!root || !limits) goto fail;

  field = json_create_string("turbowasm");
  if (!field ||
      turbo_runtime_json_object_set(root, "backend", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  snprintf(identity, sizeof(identity), "fnv1a64:%016llx",
           (unsigned long long)hash);
  /*
   * Descriptive content fingerprint only. This is not a cryptographic module
   * identity; TurboWasm does not currently expose its internal source SHA-256
   * through the installed Runtime API.
   */
  field = json_create_string(identity);
  if (!field ||
      turbo_runtime_json_object_set(root, "module_fingerprint", field) !=
          TURBO_RUNTIME_JSON_OK)
    goto fail;
  field = NULL;

  if (!turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_module_bytes", (uint64_t)config->max_module_bytes) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_allocation_bytes",
          (uint64_t)config->max_allocation_bytes) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_linear_memory_bytes",
          (uint64_t)config->max_linear_memory_bytes) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_table_elements",
          (uint64_t)config->max_table_elements) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_input_bytes", (uint64_t)config->max_input_bytes) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "max_output_bytes", (uint64_t)config->max_output_bytes) ||
      !turbo_tool_runtime_wasm_set_u64_string(
          limits, "fuel_per_call", config->fuel_per_call))
    goto fail;

  if (turbo_runtime_json_object_set(root, "limits", limits) !=
      TURBO_RUNTIME_JSON_OK)
    goto fail;
  limits = NULL;
  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(limits);
  turbo_runtime_json_destroy(root);
  return NULL;
}

static int turbo_tool_runtime_wasm_effect_flag(
    const char *name, turbo_tool_effect_flags_t *out) {
  if (!name || !out) return -1;
  if (strcmp(name, "pure") == 0) *out = TURBO_TOOL_EFFECT_PURE;
  else if (strcmp(name, "read") == 0) *out = TURBO_TOOL_EFFECT_READ;
  else if (strcmp(name, "write") == 0) *out = TURBO_TOOL_EFFECT_WRITE;
  else if (strcmp(name, "process") == 0) *out = TURBO_TOOL_EFFECT_PROCESS;
  else if (strcmp(name, "network") == 0) *out = TURBO_TOOL_EFFECT_NETWORK;
  else if (strcmp(name, "external_mutation") == 0)
    *out = TURBO_TOOL_EFFECT_EXTERNAL_MUTATION;
  else if (strcmp(name, "unknown") == 0) *out = TURBO_TOOL_EFFECT_UNKNOWN;
  else return -1;
  return 0;
}

static int turbo_tool_runtime_wasm_parse_effects(
    const json_value_t *metadata, turbo_tool_effect_flags_t *out) {
  const json_value_t *effects;
  turbo_tool_effect_flags_t flags = 0;
  size_t i;
  if (!metadata || !out) return -1;
  effects = json_object_get(metadata, "effects");
  if (!effects) {
    *out = TURBO_TOOL_EFFECT_UNKNOWN;
    return 0;
  }
  if (json_type(effects) != JSON_ARRAY) return -1;
  for (i = 0; i < json_array_size(effects); ++i) {
    const json_value_t *value = json_array_get(effects, i);
    const char *name =
        value && json_type(value) == JSON_STRING ? json_string(value) : NULL;
    turbo_tool_effect_flags_t flag = 0;
    if (!name || turbo_tool_runtime_wasm_effect_flag(name, &flag) != 0)
      return -1;
    flags |= flag;
  }
  if (flags == 0) flags = TURBO_TOOL_EFFECT_UNKNOWN;
  if ((flags & TURBO_TOOL_EFFECT_UNKNOWN) != 0 &&
      flags != TURBO_TOOL_EFFECT_UNKNOWN)
    return -1;
  if ((flags & TURBO_TOOL_EFFECT_PURE) != 0 &&
      flags != TURBO_TOOL_EFFECT_PURE)
    return -1;
  *out = flags;
  return 0;
}

static void turbo_tool_runtime_wasm_tool_clear(turbo_tool_runtime_wasm_tool_t *tool) {
  if (!tool) return;
  tstr_free(tool->name);
  tstr_free(tool->description);
  tstr_free(tool->parameters_json);
  turbo_runtime_json_destroy(tool->parameters_schema);
  tstr_free(tool->result_schema_json);
  turbo_runtime_json_destroy(tool->result_schema);
  memset(tool, 0, sizeof(*tool));
}

static void turbo_tool_runtime_wasm_destroy_impl(void *impl) {
  turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (turbo_tool_runtime_wasm_impl_t *)impl;
  size_t i;

  if (!wasm_impl) return;
  for (i = 0; i < wasm_impl->tool_count; ++i)
    turbo_tool_runtime_wasm_tool_clear(&wasm_impl->tools[i]);
  free(wasm_impl->tools);
  turbowasm_instance_destroy(&wasm_impl->instance);
  turbowasm_module_destroy(&wasm_impl->module);
  free(wasm_impl->module_bytes.base);
  wasm_impl->module_bytes.base = NULL;
  wasm_impl->module_bytes.len = 0u;
  turbo_wasm_invoke_gate_destroy(&wasm_impl->invoke_gate);
  free(wasm_impl);
}

static int turbo_tool_runtime_wasm_host_result_i32(
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap,
    int32_t value) {
  if (!results || result_capacity < 1u || !result_count || !trap) return 0;
  results[0].kind = TURBOWASM_VALUE_I32;
  results[0].as.i32 = value;
  *result_count = 1u;
  *trap = TURBOWASM_TRAP_NONE;
  return 1;
}

static turbowasm_status turbo_tool_runtime_wasm_host_input_size(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
  turbo_tool_runtime_wasm_impl_t *impl =
      (turbo_tool_runtime_wasm_impl_t *)context;
  turbo_tool_runtime_wasm_io_t *io = impl ? impl->active_io : NULL;
  (void)call;
  (void)arguments;

  if (!io || argument_count != 0u || io->input_size > INT32_MAX ||
      !turbo_tool_runtime_wasm_host_result_i32(
          results, result_capacity, result_count, trap,
          (int32_t)io->input_size))
    return TURBOWASM_INVALID_ARGUMENT;
  return TURBOWASM_OK;
}

static turbowasm_status turbo_tool_runtime_wasm_host_input_read(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
  turbo_tool_runtime_wasm_impl_t *impl =
      (turbo_tool_runtime_wasm_impl_t *)context;
  turbo_tool_runtime_wasm_io_t *io = impl ? impl->active_io : NULL;
  turbowasm_host_memory_span span = {0};
  uint32_t destination;
  size_t offset;
  size_t capacity;
  size_t remaining;
  size_t to_copy;
  turbowasm_status status;

  if (!io || !call || !arguments || argument_count != 3u ||
      arguments[0].kind != TURBOWASM_VALUE_I32 ||
      arguments[1].kind != TURBOWASM_VALUE_I32 ||
      arguments[2].kind != TURBOWASM_VALUE_I32 ||
      arguments[0].as.i32 < 0 || arguments[2].as.i32 < 0)
    return TURBOWASM_INVALID_ARGUMENT;

  offset = (size_t)(uint32_t)arguments[0].as.i32;
  destination = (uint32_t)arguments[1].as.i32;
  capacity = (size_t)(uint32_t)arguments[2].as.i32;
  if (offset > io->input_size) {
    if (!turbo_tool_runtime_wasm_host_result_i32(
            results, result_capacity, result_count, trap, -1))
      return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_OK;
  }

  remaining = io->input_size - offset;
  to_copy = capacity < remaining ? capacity : remaining;
  if (to_copy != 0u) {
    status = turbowasm_host_call_memory_span(
        call, 0u, destination, to_copy, &span, trap);
    if (status != TURBOWASM_OK) return status;
    memcpy(span.data, io->input + offset, to_copy);
  }
  if (to_copy > INT32_MAX ||
      !turbo_tool_runtime_wasm_host_result_i32(
          results, result_capacity, result_count, trap, (int32_t)to_copy))
    return TURBOWASM_INVALID_ARGUMENT;
  return TURBOWASM_OK;
}

static turbowasm_status turbo_tool_runtime_wasm_host_output_write(
    void *context,
    turbowasm_host_call *call,
    const turbowasm_value *arguments,
    size_t argument_count,
    turbowasm_value *results,
    size_t result_capacity,
    size_t *result_count,
    turbowasm_trap *trap) {
  turbo_tool_runtime_wasm_impl_t *impl =
      (turbo_tool_runtime_wasm_impl_t *)context;
  turbo_tool_runtime_wasm_io_t *io = impl ? impl->active_io : NULL;
  turbowasm_host_memory_span span = {0};
  uint32_t source;
  size_t length;
  turbowasm_status status;

  if (!io || !call || !arguments || argument_count != 2u ||
      arguments[0].kind != TURBOWASM_VALUE_I32 ||
      arguments[1].kind != TURBOWASM_VALUE_I32 ||
      arguments[1].as.i32 < 0)
    return TURBOWASM_INVALID_ARGUMENT;

  source = (uint32_t)arguments[0].as.i32;
  length = (size_t)(uint32_t)arguments[1].as.i32;
  if (length > io->output_limit - io->output_size) {
    io->output_overflow = 1;
    if (!turbo_tool_runtime_wasm_host_result_i32(
            results, result_capacity, result_count, trap, -1))
      return TURBOWASM_INVALID_ARGUMENT;
    return TURBOWASM_OK;
  }

  if (length != 0u) {
    status = turbowasm_host_call_memory_span(
        call, 0u, source, length, &span, trap);
    if (status != TURBOWASM_OK) return status;
    memcpy(io->output + io->output_size, span.data, length);
    io->output_size += length;
  }
  io->output[io->output_size] = '\0';

  if (!turbo_tool_runtime_wasm_host_result_i32(
          results, result_capacity, result_count, trap, 0))
    return TURBOWASM_INVALID_ARGUMENT;
  return TURBOWASM_OK;
}

static int turbo_tool_runtime_wasm_find_export(
    const turbowasm_module *module,
    const char *name,
    uint32_t expected_param_count,
    uint32_t expected_result_count,
    uint32_t *out_function_index) {
  const cmeta_type_desc *i32_type =
      turbowasm_value_type_descriptor(TURBOWASM_VALUE_I32);
  size_t i;

  if (!module || !name || !out_function_index || !i32_type) return 0;
  for (i = 0; i < turbowasm_module_export_count(module); ++i) {
    const turbowasm_export_desc *export_desc =
        turbowasm_module_export_at(module, i);
    turbowasm_function_signature signature = {0};
    uint32_t p;
    uint32_t r;
    if (!export_desc || export_desc->kind != TURBOWASM_EXTERN_FUNCTION ||
        !turbo_tool_runtime_wasm_name_equal(export_desc->name, name))
      continue;
    if (!turbowasm_module_function_signature_get(
            module, export_desc->item_index, &signature) ||
        signature.param_count != expected_param_count ||
        signature.result_count != expected_result_count)
      return 0;
    for (p = 0; p < signature.param_count; ++p)
      if (!cmeta_type_equal(
              turbowasm_module_function_param_type(
                  module, export_desc->item_index, p),
              i32_type))
        return 0;
    for (r = 0; r < signature.result_count; ++r)
      if (!cmeta_type_equal(
              turbowasm_module_function_result_type(
                  module, export_desc->item_index, r),
              i32_type))
        return 0;
    *out_function_index = export_desc->item_index;
    return 1;
  }
  return 0;
}

static int turbo_tool_runtime_wasm_link_tool_io(
    turbowasm_linker *linker,
    turbo_tool_runtime_wasm_impl_t *impl) {
  static const turbowasm_value_kind input_read_params[] = {
      TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32};
  static const turbowasm_value_kind output_write_params[] = {
      TURBOWASM_VALUE_I32, TURBOWASM_VALUE_I32};
  static const turbowasm_value_kind i32_result[] = {TURBOWASM_VALUE_I32};
  const turbowasm_host_function_type input_size_type = {
      NULL, 0u, i32_result, 1u};
  const turbowasm_host_function_type input_read_type = {
      input_read_params, 3u, i32_result, 1u};
  const turbowasm_host_function_type output_write_type = {
      output_write_params, 2u, i32_result, 1u};
  turbowasm_name module_name = turbo_tool_runtime_wasm_name("turbo_agent");

  return turbowasm_linker_define_host_function(
             linker, module_name,
             turbo_tool_runtime_wasm_name("tool_input_size"),
             &input_size_type, turbo_tool_runtime_wasm_host_input_size,
             impl) == TURBOWASM_OK &&
         turbowasm_linker_define_host_function(
             linker, module_name,
             turbo_tool_runtime_wasm_name("tool_input_read"),
             &input_read_type, turbo_tool_runtime_wasm_host_input_read,
             impl) == TURBOWASM_OK &&
         turbowasm_linker_define_host_function(
             linker, module_name,
             turbo_tool_runtime_wasm_name("tool_output_write"),
             &output_write_type, turbo_tool_runtime_wasm_host_output_write,
             impl) == TURBOWASM_OK;
}

static bool turbo_tool_runtime_wasm_should_interrupt(void *user_data) {
  turbo_tool_runtime_wasm_control_t *control =
      (turbo_tool_runtime_wasm_control_t *)user_data;
  const turbo_tool_execution_context_t *context;
  if (!control || !(context = control->context)) return 0;

  if (context->cancel_token && turbo_cancel_token_check(context->cancel_token) != 0) {
    control->reason =
        turbo_cancel_token_reason(context->cancel_token) == TURBO_CANCEL_DEADLINE
            ? TURBO_TOOL_WASM_INTERRUPT_DEADLINE
            : TURBO_TOOL_WASM_INTERRUPT_CANCELLED;
    return 1;
  }
  if (context->deadline_mono_ms &&
      salts_monotonic_ms() >= context->deadline_mono_ms) {
    control->reason = TURBO_TOOL_WASM_INTERRUPT_DEADLINE;
    return 1;
  }
  return 0;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_map_status(
    turbowasm_status status,
    const turbo_tool_runtime_wasm_control_t *control,
    int output_overflow) {
  if (output_overflow) return TURBO_TOOL_OUTPUT_LIMIT;
  if (status == TURBOWASM_OK) return TURBO_TOOL_OK;
  if (status == TURBOWASM_INTERRUPTED && control) {
    if (control->reason == TURBO_TOOL_WASM_INTERRUPT_DEADLINE)
      return TURBO_TOOL_DEADLINE_EXCEEDED;
    if (control->reason == TURBO_TOOL_WASM_INTERRUPT_CANCELLED)
      return TURBO_TOOL_CANCELLED;
  }
  if (status == TURBOWASM_OUT_OF_MEMORY) return TURBO_TOOL_OUT_OF_MEMORY;
  if (status == TURBOWASM_FUEL_EXHAUSTED) return TURBO_TOOL_FUEL_EXHAUSTED;
  if (status == TURBOWASM_TRAPPED) return TURBO_TOOL_TRAPPED;
  return TURBO_TOOL_ERROR;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_call(
    turbo_tool_runtime_wasm_impl_t *impl,
    uint32_t function_index,
    int32_t index,
    int has_index,
    const char *input,
    size_t output_limit,
    const turbo_tool_execution_context_t *context,
    char **out_output,
    int32_t *out_guest_status) {
  turbo_tool_runtime_wasm_io_t io = {0};
  turbo_tool_runtime_wasm_control_t control = {0};
  turbowasm_execution_options options = {0};
  turbowasm_value argument = {0};
  turbowasm_value result = {0};
  turbowasm_trap trap = TURBOWASM_TRAP_NONE;
  size_t result_count = 0u;
  size_t input_size = input ? strlen(input) : 0u;
  turbowasm_status wasm_status;
  turbo_tool_status_t status;

  if (!impl || !out_output || !out_guest_status || output_limit == 0u ||
      output_limit == SIZE_MAX || input_size > INT32_MAX)
    return TURBO_TOOL_INVALID_ARGUMENT;

  *out_output = NULL;
  *out_guest_status = -1;
  io.input = (const uint8_t *)input;
  io.input_size = input_size;
  io.output = (char *)malloc(output_limit + 1u);
  io.output_limit = output_limit;
  if (!io.output) return TURBO_TOOL_OUT_OF_MEMORY;
  io.output[0] = '\0';

  control.context = context;
  if (context && turbo_tool_runtime_wasm_should_interrupt(&control)) {
    status = turbo_tool_runtime_wasm_map_status(
        TURBOWASM_INTERRUPTED, &control, 0);
    free(io.output);
    return status;
  }

  options.fuel = impl->fuel_per_call;
  options.has_fuel_limit = impl->fuel_per_call != 0u;
  options.should_interrupt =
      context ? turbo_tool_runtime_wasm_should_interrupt : NULL;
  options.interrupt_context = context ? &control : NULL;

  argument.kind = TURBOWASM_VALUE_I32;
  argument.as.i32 = index;

  /*
   * TurboWasm instances are single-owner and RuntimeTools registry execution
   * does not enforce execution_policy locks. The backend gate serializes
   * ownership without holding its mutex across guest execution. Queue time is
   * part of the invocation deadline and cancellation remains observable while
   * waiting.
   */
  status = turbo_wasm_invoke_gate_acquire(&impl->invoke_gate, context);
  if (status != TURBO_TOOL_OK) {
    free(io.output);
    return status;
  }

  impl->active_io = &io;
  wasm_status = turbowasm_instance_invoke_with_options(
      &impl->instance, function_index,
      has_index ? &argument : NULL, has_index ? 1u : 0u,
      &result, 1u, &result_count, &trap, &options);
  impl->active_io = NULL;
  turbo_wasm_invoke_gate_release(&impl->invoke_gate);

  status = turbo_tool_runtime_wasm_map_status(
      wasm_status, &control, io.output_overflow);
  if (status != TURBO_TOOL_OK || trap != TURBOWASM_TRAP_NONE ||
      result_count != 1u || result.kind != TURBOWASM_VALUE_I32) {
    free(io.output);
    return status == TURBO_TOOL_OK ? TURBO_TOOL_ERROR : status;
  }
  if (io.output_size != 0u &&
      memchr(io.output, '\0', io.output_size) != NULL) {
    free(io.output);
    return TURBO_TOOL_ERROR;
  }

  *out_guest_status = result.as.i32;
  *out_output = io.output;
  return TURBO_TOOL_OK;
}

static size_t turbo_tool_runtime_wasm_tool_count(const void *impl) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm_impl_t *)impl;
  return wasm_impl ? wasm_impl->tool_count : 0u;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_get_tool(
    const void *impl, size_t index, turbo_tool_runtime_tool_t *out_tool) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm_impl_t *)impl;
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

static turbo_tool_status_t turbo_tool_runtime_wasm_get_tool_v2(
    const void *impl, size_t index, turbo_tool_runtime_tool_v2_t *out_tool) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm_impl_t *)impl;
  const turbo_tool_runtime_wasm_tool_t *tool;

  if (!wasm_impl || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  if (index >= wasm_impl->tool_count) return TURBO_TOOL_NOT_FOUND;
  tool = &wasm_impl->tools[index];
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V2_ABI_VERSION;
  out_tool->base.name = tool->name;
  out_tool->base.description = tool->description;
  out_tool->base.parameters_json = tool->parameters_json;
  out_tool->base.parameters_schema = tool->parameters_schema;
  out_tool->base.strict = tool->strict;
  out_tool->result_schema_json = tool->result_schema_json;
  out_tool->strict_result = tool->strict_result;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_get_tool_v3(
    const void *impl, size_t index, turbo_tool_runtime_tool_v3_t *out_tool) {
  const turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (const turbo_tool_runtime_wasm_impl_t *)impl;
  const turbo_tool_runtime_wasm_tool_t *tool;
  if (!wasm_impl || !out_tool) return TURBO_TOOL_INVALID_ARGUMENT;
  if (index >= wasm_impl->tool_count) return TURBO_TOOL_NOT_FOUND;
  tool = &wasm_impl->tools[index];
  memset(out_tool, 0, sizeof(*out_tool));
  out_tool->struct_size = sizeof(*out_tool);
  out_tool->abi_version = TURBO_TOOL_RUNTIME_TOOL_V3_ABI_VERSION;
  if (turbo_tool_runtime_wasm_get_tool_v2(
          impl, index, &out_tool->base) != TURBO_TOOL_OK)
    return TURBO_TOOL_ERROR;
  out_tool->effect_flags = tool->effect_flags;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke_with_context(
    void *impl,
    const char *name,
    const char *arguments_json,
    const turbo_tool_execution_context_t *context,
    char **out_output) {
  turbo_tool_runtime_wasm_impl_t *wasm_impl =
      (turbo_tool_runtime_wasm_impl_t *)impl;
  const char *input = arguments_json ? arguments_json : "{}";
  int32_t guest_status;
  turbo_tool_status_t status;
  size_t i;

  if (!wasm_impl || !name || !out_output) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_output = NULL;
  if (strlen(input) > wasm_impl->max_input_bytes) return TURBO_TOOL_ERROR;
  for (i = 0; i < wasm_impl->tool_count; ++i)
    if (strcmp(wasm_impl->tools[i].name, name) == 0) break;
  if (i == wasm_impl->tool_count) return TURBO_TOOL_NOT_FOUND;

  status = turbo_tool_runtime_wasm_call(
      wasm_impl, wasm_impl->export_tool_invoke, (int32_t)i, 1, input,
      wasm_impl->max_output_bytes, context, out_output, &guest_status);
  if (status != TURBO_TOOL_OK || guest_status != 0) {
    free(*out_output);
    *out_output = NULL;
    return status == TURBO_TOOL_OK ? TURBO_TOOL_ERROR : status;
  }
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke(
    void *impl, const char *name, const char *arguments_json, char **out_output) {
  return turbo_tool_runtime_wasm_invoke_with_context(
      impl, name, arguments_json, NULL, out_output);
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke_json_value_with_context(
    void *impl,
    const char *name,
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result) {
  char *arguments_json = NULL;
  char *output_json = NULL;
  json_value_t *parsed = NULL;
  turbo_tool_status_t status;

  if (!out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_result = NULL;
  if (arguments) {
    arguments_json = json_serialize(arguments, NULL);
    if (!arguments_json) return TURBO_TOOL_ERROR;
  }
  status = turbo_tool_runtime_wasm_invoke_with_context(
      impl, name, arguments_json, context, &output_json);
  json_serialize_free(arguments_json);
  if (status != TURBO_TOOL_OK) return status;

  parsed = json_parse(output_json, strlen(output_json));
  free(output_json);
  if (!parsed) return TURBO_TOOL_ERROR;
  *out_result = parsed;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t turbo_tool_runtime_wasm_invoke_json_value(
    void *impl,
    const char *name,
    const json_value_t *arguments,
    json_value_t **out_result) {
  return turbo_tool_runtime_wasm_invoke_json_value_with_context(
      impl, name, arguments, NULL, out_result);
}

static const turbo_tool_runtime_vtable_v4_t turbo_tool_runtime_wasm_vtable = {
    sizeof(turbo_tool_runtime_vtable_v4_t),
    TURBO_TOOL_RUNTIME_VTABLE_V4_ABI_VERSION,
    {
        sizeof(turbo_tool_runtime_vtable_v3_t),
        TURBO_TOOL_RUNTIME_VTABLE_V3_ABI_VERSION,
        {
            sizeof(turbo_tool_runtime_vtable_v2_t),
            TURBO_TOOL_RUNTIME_VTABLE_V2_ABI_VERSION,
            {
                turbo_tool_runtime_wasm_destroy_impl,
                turbo_tool_runtime_wasm_tool_count,
                turbo_tool_runtime_wasm_get_tool,
                turbo_tool_runtime_wasm_invoke,
                turbo_tool_runtime_wasm_invoke_json_value,
            },
            turbo_tool_runtime_wasm_invoke_with_context,
            turbo_tool_runtime_wasm_invoke_json_value_with_context,
        },
        turbo_tool_runtime_wasm_get_tool_v2,
    },
    turbo_tool_runtime_wasm_get_tool_v3,
};

static int turbo_tool_runtime_wasm_load_tool(
    turbo_tool_runtime_wasm_impl_t *impl, size_t index) {
  turbo_tool_runtime_wasm_tool_t *tool = &impl->tools[index];
  json_value_t *metadata = NULL;
  json_value_t *parameters;
  json_value_t *strict;
  json_value_t *result_schema;
  json_value_t *strict_result;
  const char *name;
  const char *description;
  char *metadata_json = NULL;
  char *parameters_json = NULL;
  char *result_schema_json = NULL;
  int32_t guest_status;
  size_t prior_index;
  int rc = -1;

  if (turbo_tool_runtime_wasm_call(
          impl, impl->export_tool_describe, (int32_t)index, 1, NULL,
          impl->max_metadata_bytes, NULL, &metadata_json,
          &guest_status) != TURBO_TOOL_OK ||
      guest_status != 0 || !metadata_json[0])
    goto cleanup;

  metadata = json_parse(metadata_json, strlen(metadata_json));
  if (!metadata || json_type(metadata) != JSON_OBJECT) goto cleanup;
  name = json_get_string(metadata, "name");
  description = json_get_string(metadata, "description");
  parameters = json_object_get(metadata, "parameters");
  strict = json_object_get(metadata, "strict");
  result_schema = json_object_get(metadata, "result");
  strict_result = json_object_get(metadata, "strict_result");
  if (!name || !name[0] || !description || !parameters ||
      json_type(parameters) != JSON_OBJECT || !strict ||
      json_type(strict) != JSON_BOOL)
    goto cleanup;
  if (result_schema && json_type(result_schema) != JSON_OBJECT) goto cleanup;
  if (strict_result && json_type(strict_result) != JSON_BOOL) goto cleanup;
  if (strict_result && !result_schema) goto cleanup;
  if (turbo_tool_runtime_wasm_parse_effects(
          metadata, &tool->effect_flags) != 0)
    goto cleanup;

  parameters_json = json_serialize(parameters, NULL);
  if (!parameters_json) goto cleanup;
  if (result_schema) {
    result_schema_json = json_serialize(result_schema, NULL);
    if (!result_schema_json) goto cleanup;
  }
  tool->name = (char *)tstr_dup(name);
  tool->description = (char *)tstr_dup(description);
  tool->parameters_json = (char *)tstr_dup(parameters_json);
  tool->parameters_schema =
      turbo_tool_schema_parse_parameters_json_value(parameters_json, 0);
  tool->strict = json_bool(strict) ? 1 : 0;
  if (result_schema_json) {
    tool->result_schema_json = (char *)tstr_dup(result_schema_json);
    tool->result_schema = json_clone(result_schema);
    tool->strict_result =
        strict_result && json_bool(strict_result) ? 1 : 0;
  }
  if (!tool->name || !tool->description || !tool->parameters_json ||
      !tool->parameters_schema ||
      (result_schema_json &&
       (!tool->result_schema_json || !tool->result_schema)))
    goto cleanup;
  for (prior_index = 0; prior_index < index; ++prior_index)
    if (strcmp(impl->tools[prior_index].name, tool->name) == 0)
      goto cleanup;
  rc = 0;

cleanup:
  if (rc != 0) turbo_tool_runtime_wasm_tool_clear(tool);
  json_serialize_free(result_schema_json);
  json_serialize_free(parameters_json);
  turbo_runtime_json_destroy(metadata);
  free(metadata_json);
  return rc;
}

static int turbo_tool_runtime_wasm_read_module_bounded(
    const char *path, size_t limit, salts_fs_buf_t *out) {
  salts_file_t file = SALTS_INVALID_FILE;
  char *buffer = NULL;
  size_t used = 0u;
  int read_status;
  char overflow_probe;

  if (!path || !path[0] || !limit || !out) return -1;
  out->base = NULL;
  out->len = 0u;

  file = salts_fs_open(path, SALTS_FS_O_RDONLY, 0);
  if (file == SALTS_INVALID_FILE) return -1;

  buffer = (char *)malloc(limit);
  if (!buffer) goto fail;

  while (used < limit) {
    size_t remaining = limit - used;
    size_t chunk = remaining > (size_t)INT_MAX ? (size_t)INT_MAX : remaining;
    read_status = salts_fs_read(file, buffer + used, chunk);
    if (read_status < 0) goto fail;
    if (read_status == 0) break;
    used += (size_t)read_status;
  }

  if (used == limit) {
    read_status = salts_fs_read(file, &overflow_probe, 1u);
    if (read_status != 0) goto fail;
  }

  if (salts_fs_close(file) != 0) {
    file = SALTS_INVALID_FILE;
    goto fail;
  }
  file = SALTS_INVALID_FILE;

  out->base = buffer;
  out->len = used;
  return 0;

fail:
  if (file != SALTS_INVALID_FILE) (void)salts_fs_close(file);
  free(buffer);
  return -1;
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
  config->max_module_bytes = TURBO_TOOL_WASM_DEFAULT_MODULE_BYTES;
  config->max_allocation_bytes = TURBO_TOOL_WASM_DEFAULT_ALLOCATION_BYTES;
  config->max_linear_memory_bytes =
      TURBO_TOOL_WASM_DEFAULT_LINEAR_MEMORY_BYTES;
  config->max_table_elements = TURBO_TOOL_WASM_DEFAULT_TABLE_ELEMENTS;
  config->fuel_per_call = TURBO_TOOL_WASM_DEFAULT_FUEL;
}

turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create_with_metadata(
    const turbo_tool_runtime_wasm_config_t *config,
    json_value_t **out_execution_metadata) {
  turbowasm_runtime_config runtime_config;
  turbowasm_linker linker = {0};
  turbo_tool_runtime_wasm_impl_t *impl = NULL;
  turbowasm_value count_result = {0};
  turbowasm_trap trap = TURBOWASM_TRAP_NONE;
  size_t result_count = 0u;
  int32_t count;
  size_t i;
  int linker_initialized = 0;
  turbo_tool_runtime_t *runtime = NULL;
  json_value_t *execution_metadata = NULL;

  if (out_execution_metadata) *out_execution_metadata = NULL;

  if (!config || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_TOOL_RUNTIME_WASM_ABI_VERSION ||
      !config->module_path || !config->module_path[0] || !config->max_tools ||
      !config->max_metadata_bytes || !config->max_input_bytes ||
      !config->max_output_bytes || !config->max_module_bytes ||
      !config->max_allocation_bytes || !config->max_linear_memory_bytes ||
      !config->max_table_elements || config->max_tools > INT32_MAX ||
      config->max_metadata_bytes > INT32_MAX ||
      config->max_input_bytes > INT32_MAX ||
      config->max_output_bytes > INT32_MAX)
    return NULL;

  impl = (turbo_tool_runtime_wasm_impl_t *)calloc(1, sizeof(*impl));
  if (!impl) return NULL;
  if (turbo_wasm_invoke_gate_init(&impl->invoke_gate) != 0) {
    free(impl);
    return NULL;
  }
  impl->max_metadata_bytes = config->max_metadata_bytes;
  impl->max_input_bytes = config->max_input_bytes;
  impl->max_output_bytes = config->max_output_bytes;
  impl->fuel_per_call = config->fuel_per_call;

  if (turbo_tool_runtime_wasm_read_module_bounded(
          config->module_path, config->max_module_bytes,
          &impl->module_bytes) != 0)
    goto fail;

  turbowasm_runtime_config_init(&runtime_config);
  runtime_config.limits.max_module_bytes = config->max_module_bytes;
  runtime_config.limits.max_allocation_bytes = config->max_allocation_bytes;
  runtime_config.limits.max_linear_memory_bytes = config->max_linear_memory_bytes;
  runtime_config.limits.max_table_elements = config->max_table_elements;

  if (turbowasm_module_load_borrowed_with_config(
          &impl->module, impl->module_bytes.base, impl->module_bytes.len,
          &runtime_config) != TURBOWASM_OK)
    goto fail;

  if (!turbo_tool_runtime_wasm_find_export(
          &impl->module, "turbo_tool_count", 0u, 1u,
          &impl->export_tool_count) ||
      !turbo_tool_runtime_wasm_find_export(
          &impl->module, "turbo_tool_describe", 1u, 1u,
          &impl->export_tool_describe) ||
      !turbo_tool_runtime_wasm_find_export(
          &impl->module, "turbo_tool_invoke", 1u, 1u,
          &impl->export_tool_invoke))
    goto fail;

  if (turbowasm_linker_init_with_config(&linker, &runtime_config) !=
          TURBOWASM_OK ||
      !turbo_tool_runtime_wasm_link_tool_io(&linker, impl))
    goto fail;
  linker_initialized = 1;

  if (turbowasm_instance_create_linked(
          &impl->instance, &impl->module, &linker) != TURBOWASM_OK)
    goto fail;
  turbowasm_linker_destroy(&linker);
  linker_initialized = 0;

  count_result.kind = TURBOWASM_VALUE_I32;
  {
    turbowasm_execution_options options = {0};
    options.fuel = impl->fuel_per_call;
    options.has_fuel_limit = impl->fuel_per_call != 0u;
    if (turbowasm_instance_invoke_with_options(
            &impl->instance, impl->export_tool_count,
            NULL, 0u, &count_result, 1u, &result_count, &trap,
            &options) != TURBOWASM_OK ||
        trap != TURBOWASM_TRAP_NONE || result_count != 1u ||
        count_result.kind != TURBOWASM_VALUE_I32)
      goto fail;
  }

  count = count_result.as.i32;
  if (count < 0 || (size_t)count > config->max_tools) goto fail;
  impl->tool_count = (size_t)count;
  if (impl->tool_count) {
    impl->tools = (turbo_tool_runtime_wasm_tool_t *)calloc(
        impl->tool_count, sizeof(*impl->tools));
    if (!impl->tools) goto fail;
    for (i = 0; i < impl->tool_count; ++i)
      if (turbo_tool_runtime_wasm_load_tool(impl, i) != 0) goto fail;
  }

  runtime = turbo_tool_runtime_create_v4(&turbo_tool_runtime_wasm_vtable, impl);
  if (!runtime) goto fail;

  if (out_execution_metadata) {
    execution_metadata =
        turbo_tool_runtime_wasm_execution_metadata(impl, config);
    if (!execution_metadata) {
      turbo_tool_runtime_destroy(runtime);
      return NULL;
    }
    *out_execution_metadata = execution_metadata;
  }
  return runtime;

fail:
  if (linker_initialized) turbowasm_linker_destroy(&linker);
  turbo_tool_runtime_wasm_destroy_impl(impl);
  return NULL;
}

turbo_tool_runtime_t *
turbo_tool_runtime_wasm_create(const turbo_tool_runtime_wasm_config_t *config) {
  return turbo_tool_runtime_wasm_create_with_metadata(config, NULL);
}
