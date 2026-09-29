#include "tinytest.h"
#include <salts_fs.h>
#include "turbo_tool_runtime_wasm.h"
#include "turbo_tool_runtime_wasm3.h"

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

static turbo_tool_runtime_t *create_runtime(const char *module_path, uint32_t capabilities,
                                            size_t max_input_bytes, size_t max_output_bytes,
                                            uint32_t linear_memory_bytes) {
  turbo_tool_runtime_wasm_config_t config;
  turbo_wasm_execution_limits_t limits;
  turbo_wasm_policy_t *policy = NULL;
  turbo_tool_runtime_t *runtime = NULL;
  char module_root[SALTS_FS_MAX_PATH];
  char module_name[SALTS_FS_MAX_PATH];

  if (salts_fs_path_dirname(module_path, module_root, sizeof(module_root)) != 0 ||
      salts_fs_path_basename(module_path, module_name, sizeof(module_name)) != 0)
    return NULL;
  policy = turbo_wasm_policy_create();
  if (!policy || turbo_wasm_policy_set_capabilities(policy, capabilities) != TURBO_WASM_OK ||
      turbo_wasm_policy_set_module_root(policy, module_root) != TURBO_WASM_OK ||
      turbo_wasm_policy_get_execution_limits(policy, &limits) != TURBO_WASM_OK)
    goto cleanup;
  if (linear_memory_bytes) limits.linear_memory_bytes = linear_memory_bytes;
  if (turbo_wasm_policy_set_execution_limits(policy, &limits) != TURBO_WASM_OK) goto cleanup;
  turbo_tool_runtime_wasm_config_init(&config);
  config.module_path = module_name;
  config.policy = policy;
  if (max_input_bytes) config.max_input_bytes = max_input_bytes;
  if (max_output_bytes) config.max_output_bytes = max_output_bytes;
  runtime = turbo_tool_runtime_wasm_create(&config);

cleanup:
  turbo_wasm_policy_destroy(policy);
  return runtime;
}

spec("TurboWasm tool sandbox") {
  it("loads descriptors and invokes JSON through bounded App I/O") {
    turbo_tool_runtime_t *runtime = create_runtime(
        LLM_SANDBOX_WASM_TOOL_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    turbo_tool_runtime_tool_t tool = {0};
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    char *output = NULL;

    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    check_int_eq(turbo_tool_runtime_get_tool(runtime, 0, &tool), TURBO_TOOL_OK);
    check_str_eq(tool.name, "echo_json");
    check_str_eq(
        turbo_runtime_json_value_as_string(turbo_json_object_get(tool.parameters_schema, "type")),
        "object");
    check_int_eq(turbo_tool_runtime_invoke(runtime, "echo_json", "{\"wasm\":true}", &output),
                 TURBO_TOOL_OK);
    check_str_eq(output, "{\"wasm\":true}");

    arguments = turbo_json_create_object();
    check_not_null(arguments);
    check_int_eq(turbo_runtime_json_object_set(arguments, "wasm", turbo_json_create_bool(1)),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_tool_runtime_invoke_json_value(runtime, "echo_json", arguments, &result),
                 TURBO_TOOL_OK);
    check_true(turbo_runtime_json_value_as_bool(turbo_json_object_get(result, "wasm"), 0));

    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(arguments);
    free(output);
    turbo_tool_runtime_destroy(runtime);
  }

  it("uses TurboWasm as the legacy default backend") {
    turbo_tool_runtime_wasm3_config_t config = {0};
    turbo_tool_runtime_t *runtime;
    config.module_path = LLM_SANDBOX_WASM_TOOL_WASM_PATH;
    runtime = turbo_tool_runtime_default_create(&config);
    check_not_null(runtime);
    check_size_eq(turbo_tool_runtime_count(runtime), 1);
    turbo_tool_runtime_destroy(runtime);
  }

  it("rejects guest modules without the App capability") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_TOOL_WASM_PATH, TURBO_WASM_CAP_CORE, 0, 0, 0);
    check_null(runtime);
  }

  it("rejects missing modules and required exports") {
    turbo_tool_runtime_t *missing =
        create_runtime("missing-tool.wasm", TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    turbo_tool_runtime_t *bad_exports =
        create_runtime(LLM_SANDBOX_WASM_MISSING_EXPORT_WASM_PATH,
                       TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    check_null(missing);
    check_null(bad_exports);
  }

  it("rejects invalid tool counts and accepts an empty catalog") {
    turbo_tool_runtime_t *negative =
        create_runtime(LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM_PATH,
                       TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    turbo_tool_runtime_t *empty = create_runtime(LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH,
                                                 TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    check_null(negative);
    check_not_null(empty);
    check_size_eq(turbo_tool_runtime_count(empty), 0);
    turbo_tool_runtime_destroy(empty);
  }

  it("rejects malformed metadata and non-object parameter schemas") {
    turbo_tool_runtime_t *bad_metadata = create_runtime(
        LLM_SANDBOX_WASM_BAD_METADATA_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    turbo_tool_runtime_t *bad_schema = create_runtime(
        LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    check_null(bad_metadata);
    check_null(bad_schema);
  }

  it("enforces host input and output byte limits") {
    turbo_tool_runtime_t *input_limited = create_runtime(
        LLM_SANDBOX_WASM_TOOL_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 8, 0, 0);
    turbo_tool_runtime_t *output_limited = create_runtime(
        LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 8, 0);
    char *output = NULL;

    check_not_null(input_limited);
    check_not_null(output_limited);
    check_int_eq(turbo_tool_runtime_invoke(input_limited, "echo_json", "{\"long\":true}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);
    check_int_eq(turbo_tool_runtime_invoke(output_limited, "overflow_output", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);
    check_int_eq(turbo_tool_runtime_invoke(output_limited, "fail_negative", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(output_limited);
    turbo_tool_runtime_destroy(input_limited);
  }

  it("rejects modules whose initial memory exceeds the TurboWasm quota") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH,
                       TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 64 * 1024);
    check_null(runtime);
  }

  it("rejects guest memory growth beyond the TurboWasm quota") {
    turbo_tool_runtime_t *runtime =
        create_runtime(LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH,
                       TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 256 * 1024);
    char *output = NULL;

    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "grow_echo_json", "{}", &output),
                 TURBO_TOOL_ERROR);
    check_null(output);

    turbo_tool_runtime_destroy(runtime);
  }

  it("does not expose unknown tools") {
    turbo_tool_runtime_t *runtime = create_runtime(
        LLM_SANDBOX_WASM_TOOL_WASM_PATH, TURBO_WASM_CAP_CORE | TURBO_WASM_CAP_APP, 0, 0, 0);
    char *output = NULL;
    check_not_null(runtime);
    check_int_eq(turbo_tool_runtime_invoke(runtime, "missing", "{}", &output),
                 TURBO_TOOL_NOT_FOUND);
    check_null(output);
    turbo_tool_runtime_destroy(runtime);
  }
}
