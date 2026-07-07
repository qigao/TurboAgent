#include "tinytest.h"
#include "turbo_tool_runtime_wasm3.h"

#include <stdlib.h>
#include <string.h>

#ifndef LLM_SANDBOX_WASM3_TOOL_WASM_PATH
#error "LLM_SANDBOX_WASM3_TOOL_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_BAD_INVOKE_WASM_PATH
#error "LLM_SANDBOX_WASM3_BAD_INVOKE_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM_PATH
#error "LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM_PATH
#error "LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM_PATH
#error "LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM_PATH
#error "LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM_PATH
#error "LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM_PATH
#error "LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM_PATH must be defined by CMake when this test is built"
#endif
#ifndef LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM_PATH
#error "LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM_PATH must be defined by CMake when this test is built"
#endif

spec("turbo tool runtime wasm3") {

  it("should load a wasm3 guest tool module through the runtime") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_runtime_tool_t tool = {0};
    char *output = NULL;
    turbo_runtime_data_bind_value_t *bind_args = NULL;
    turbo_runtime_data_bind_value_t *bind_result = NULL;

    config.module_path = LLM_SANDBOX_WASM3_TOOL_WASM_PATH;
    config.module_name = "llm_sandbox_tool_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_str_eq(tool.name, "echo_json");
    check_not_null(tool.parameters_schema);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(tool.parameters_schema, "type")),
                 "object");
    check_int_eq(
        turbo_tool_runtime_invoke(runtime, "echo_json", "{\"wasm\":true}", &output),
        TURBO_TOOL_OK);
    check_str_eq(output, "{\"wasm\":true}");

    bind_args = turbo_runtime_data_bind_value_create_object();
    check_not_null(bind_args);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     bind_args, "wasm",
                     turbo_runtime_data_bind_value_create_bool(1)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(
        turbo_tool_runtime_invoke_bind(runtime, "echo_json", bind_args, &bind_result),
        TURBO_TOOL_OK);
    check_not_null(bind_result);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(bind_result, "wasm"), 0));

    turbo_runtime_data_bind_value_destroy(bind_result);
    turbo_runtime_data_bind_value_destroy(bind_args);
    free(output);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should expose wasm3 as the default runtime backend") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LLM_SANDBOX_WASM3_TOOL_WASM_PATH;
    config.module_name = "llm_sandbox_tool_guest";

    runtime = turbo_tool_runtime_default_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject a missing wasm3 guest module") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = "missing_llm_sandbox_tool_guest.wasm";
    config.module_name = "missing_llm_sandbox_tool_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_null(runtime);
  }

  it("should reject wasm3 guest modules missing required exports") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM_PATH;
    config.module_name = "llm_sandbox_missing_export_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_null(runtime);
  }

  it("should reject negative wasm3 guest tool counts") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM_PATH;
    config.module_name = "llm_sandbox_negative_count_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_null(runtime);
  }

  it("should allow wasm3 guest modules with zero tools") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    turbo_tool_runtime_tool_t tool = {0};

    config.module_path = LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM_PATH;
    config.module_name = "llm_sandbox_zero_tools_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 0);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_NOT_FOUND);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject wasm3 guest metadata pointers outside memory") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM_PATH;
    config.module_name = "llm_sandbox_bad_metadata_ptr_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_null(runtime);
  }

  it("should reject invalid wasm3 guest parameter schemas") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM_PATH;
    config.module_name = "llm_sandbox_bad_schema_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_null(runtime);
  }

  it("should reject unknown wasm3 guest tools") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *output = NULL;

    config.module_path = LLM_SANDBOX_WASM3_TOOL_WASM_PATH;
    config.module_name = "llm_sandbox_tool_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "missing_tool", "{}", &output),
                 TURBO_TOOL_NOT_FOUND);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject wasm3 guest input larger than the exported input buffer") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *input = NULL;
    char *output = NULL;
    size_t input_len = 600;

    config.module_path = LLM_SANDBOX_WASM3_TOOL_WASM_PATH;
    config.module_name = "llm_sandbox_tool_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    input = (char *)malloc(input_len + 1);
    check_not_null(input);
    memset(input, 'x', input_len);
    input[input_len] = '\0';

    check_int_eq(turbo_tool_runtime_invoke(runtime, "echo_json", input, &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    free(input);
    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject wasm3 guest input buffers outside memory") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *output = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM_PATH;
    config.module_name = "llm_sandbox_bad_input_buffer_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "bad_input_buffer", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject wasm3 guest output buffers outside memory") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *output = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM_PATH;
    config.module_name = "llm_sandbox_bad_output_buffer_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "bad_output_buffer", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject negative wasm3 guest output lengths") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *output = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_INVOKE_WASM_PATH;
    config.module_name = "llm_sandbox_bad_invoke_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "fail_negative", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("should reject wasm3 guest output lengths past the exported buffer") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime = NULL;
    char *output = NULL;

    config.module_path = LLM_SANDBOX_WASM3_BAD_INVOKE_WASM_PATH;
    config.module_name = "llm_sandbox_bad_invoke_guest";

    runtime = turbo_tool_runtime_wasm3_create(&config);
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "overflow_output", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }
}
