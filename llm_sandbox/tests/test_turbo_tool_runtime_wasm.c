#include "tinytest.h"

#include <salts/clock.h>

#include "turbo_tool_runtime_wasm.h"

#include <stdlib.h>
#include <string.h>

#ifndef LLM_SANDBOX_WASM_TOOL_WASM_PATH
  #error "LLM_SANDBOX_WASM_TOOL_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH
  #error "LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH
  #error "LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_MISSING_EXPORT_WASM_PATH
  #error "LLM_SANDBOX_WASM_MISSING_EXPORT_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM_PATH
  #error "LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH
  #error "LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_BAD_METADATA_WASM_PATH
  #error "LLM_SANDBOX_WASM_BAD_METADATA_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH
  #error "LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM_PATH
  #error "LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM_PATH
  #error "LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM_PATH must be defined"
#endif
#ifndef LLM_SANDBOX_WASM_CONTROL_WASM_PATH
  #error "LLM_SANDBOX_WASM_CONTROL_WASM_PATH must be defined"
#endif

static turbo_tool_runtime_t *create_runtime(const char *module_path,
                                            size_t max_input_bytes,
                                            size_t max_output_bytes,
                                            size_t linear_memory_bytes,
                                            uint64_t fuel_per_call) {
  turbo_tool_runtime_wasm_config_t config;

  turbo_tool_runtime_wasm_config_init(&config);
  config.module_path = module_path;
  if (max_input_bytes) config.max_input_bytes = max_input_bytes;
  if (max_output_bytes) config.max_output_bytes = max_output_bytes;
  if (linear_memory_bytes) config.max_linear_memory_bytes = linear_memory_bytes;
  if (fuel_per_call) config.fuel_per_call = fuel_per_call;
  return turbo_tool_runtime_wasm_create(&config);
}

spec("TurboWasm tool sandbox") {
  it("loads descriptors and invokes JSON through bounded TurboAgent host imports") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_TOOL_WASM_PATH, 0, 0, 0, 0);
    turbo_tool_runtime_tool_t tool = {0};
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    char *output = NULL;

    check_not_null(runtime);
    check_equal(turbo_tool_runtime_count(runtime), 1);
    check_equal(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_equal(tool.name, "echo_json");
    check_equal(
        turbo_runtime_json_value_as_string(json_object_get(tool.parameters_schema, "type")),
        "object");
    check_equal(turbo_tool_runtime_invoke(runtime, "echo_json", "{\"wasm\":true}", &output),
                TURBO_TOOL_OK);
    check_equal(output, "{\"wasm\":true}");

    arguments = json_create_object();
    check_not_null(arguments);
    check_equal(turbo_runtime_json_object_set(arguments, "wasm", json_create_bool(1)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_tool_runtime_invoke_json_value(runtime, "echo_json", arguments, &result),
                TURBO_TOOL_OK);
    check_true(turbo_runtime_json_value_as_bool(json_object_get(result, "wasm"), 0));

    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(arguments);
    free(output);
    turbo_tool_runtime_destroy(runtime);
  }

  it("rejects guest imports outside the TurboAgent tool I/O capability surface") {
    turbo_tool_runtime_wasm_config_t config;
    turbo_tool_runtime_t *runtime;

    turbo_tool_runtime_wasm_config_init(&config);
    config.module_path = LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM_PATH;
    runtime = turbo_tool_runtime_wasm_create(&config);
    check_null(runtime);
  }

  it("rejects missing modules and required exports") {
    turbo_tool_runtime_t *missing = create_runtime("missing-tool.wasm", 0, 0, 0, 0);
    turbo_tool_runtime_t *bad_exports =
        create_runtime(LLM_SANDBOX_WASM_MISSING_EXPORT_WASM_PATH, 0, 0, 0, 0);
    check_null(missing);
    check_null(bad_exports);
  }

  it("rejects invalid tool counts and accepts an empty catalog") {
    turbo_tool_runtime_t *negative =
        create_runtime(LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM_PATH, 0, 0, 0, 0);
    turbo_tool_runtime_t *empty =
        create_runtime(LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH, 0, 0, 0, 0);
    check_null(negative);
    check_not_null(empty);
    check_equal(turbo_tool_runtime_count(empty), 0);
    turbo_tool_runtime_destroy(empty);
  }

  it("rejects malformed metadata and non-object parameter schemas") {
    turbo_tool_runtime_t *bad_metadata =
        create_runtime(LLM_SANDBOX_WASM_BAD_METADATA_WASM_PATH, 0, 0, 0, 0);
    turbo_tool_runtime_t *bad_schema =
        create_runtime(LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH, 0, 0, 0, 0);
    check_null(bad_metadata);
    check_null(bad_schema);
  }

  it("rejects embedded NUL bytes in guest text output") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM_PATH, 0, 0, 0, 0);
    char *output = NULL;

    check_not_null(runtime);
    check_equal(turbo_tool_runtime_invoke(runtime, "nul_output", "{}", &output),
                TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("enforces host input and output byte limits") {
    turbo_tool_runtime_t *input_limited =
        create_runtime(LLM_SANDBOX_WASM_TOOL_WASM_PATH, 8, 0, 0, 0);
    turbo_tool_runtime_t *output_limited =
        create_runtime(LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH, 0, 8, 0, 0);
    char *output = NULL;

    check_not_null(input_limited);
    check_not_null(output_limited);
    check_equal(
        turbo_tool_runtime_invoke(input_limited, "echo_json", "{\"long\":true}", &output),
        TURBO_TOOL_ERROR);
    check_null(output);
    check_equal(
        turbo_tool_runtime_invoke(output_limited, "overflow_output", "{}", &output),
        TURBO_TOOL_OUTPUT_LIMIT);
    check_null(output);
    check_equal(
        turbo_tool_runtime_invoke(output_limited, "fail_negative", "{}", &output),
        TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(output_limited);
    turbo_tool_runtime_destroy(input_limited);
  }

  it("rejects module bytes beyond the host read limit") {
    turbo_tool_runtime_wasm_config_t config;
    turbo_tool_runtime_t *runtime;

    turbo_tool_runtime_wasm_config_init(&config);
    config.module_path = LLM_SANDBOX_WASM_TOOL_WASM_PATH;
    config.max_module_bytes = 8u;
    runtime = turbo_tool_runtime_wasm_create(&config);
    check_null(runtime);
  }

  it("rejects modules whose initial memory exceeds the Runtime quota") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH, 0, 0, 64 * 1024, 0);
    check_null(runtime);
  }

  it("rejects guest memory growth beyond the Runtime quota") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH, 0, 0, 256 * 1024, 0);
    char *output = NULL;

    check_not_null(runtime);
    check_equal(turbo_tool_runtime_invoke(runtime, "grow_echo_json", "{}", &output),
                TURBO_TOOL_ERROR);
    check_null(output);
    turbo_tool_runtime_destroy(runtime);
  }

  it("distinguishes cooperative cancellation from deadline expiry") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_CONTROL_WASM_PATH, 0, 0, 0, 0);
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    char *output = NULL;

    check_not_null(runtime);
    check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);

    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;
    check_equal(
        turbo_tool_runtime_invoke_with_context(runtime, "spin", "{}", &context, &output),
        TURBO_TOOL_CANCELLED);
    check_null(output);

    context.cancel_token = NULL;
    context.deadline_mono_ms = salts_monotonic_ms();
    check_equal(
        turbo_tool_runtime_invoke_with_context(runtime, "spin", "{}", &context, &output),
        TURBO_TOOL_DEADLINE_EXCEEDED);
    check_null(output);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    turbo_tool_runtime_destroy(runtime);
  }

  it("distinguishes fuel exhaustion and Wasm traps from guest tool errors") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_CONTROL_WASM_PATH, 0, 0, 0, 100000);
    turbo_tool_runtime_t *guest_error =
        create_runtime(LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH, 0, 0, 0, 0);
    char *output = NULL;

    check_not_null(runtime);
    check_not_null(guest_error);
    check_equal(turbo_tool_runtime_invoke(runtime, "spin", "{}", &output),
                TURBO_TOOL_FUEL_EXHAUSTED);
    check_null(output);
    check_equal(turbo_tool_runtime_invoke(runtime, "trap", "{}", &output),
                TURBO_TOOL_TRAPPED);
    check_null(output);
    check_equal(turbo_tool_runtime_invoke(guest_error, "fail_negative", "{}", &output),
                TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(guest_error);
    turbo_tool_runtime_destroy(runtime);
  }

  it("does not expose unknown tools") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_TOOL_WASM_PATH, 0, 0, 0, 0);
    char *output = NULL;
    check_not_null(runtime);
    check_equal(turbo_tool_runtime_invoke(runtime, "missing", "{}", &output),
                TURBO_TOOL_NOT_FOUND);
    check_null(output);
    turbo_tool_runtime_destroy(runtime);
  }
}
