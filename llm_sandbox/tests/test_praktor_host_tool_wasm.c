#include "tinytest.h"

#include "turbo_praktor_tool_pack.h"
#include "turbo_wasm_tool_pack.h"

#include <salts_fs.h>
#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

#ifndef LLM_SANDBOX_WASM_TOOL_WASM_PATH
  #error "LLM_SANDBOX_WASM_TOOL_WASM_PATH must be defined"
#endif

static unsigned int praktor_wasm_test_counter;

static char *praktor_wasm_strdup(const char *text) {
  size_t length = strlen(text) + 1u;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static char *praktor_wasm_workspace(void) {
  char temp[SALTS_FS_MAX_PATH];
  char path[SALTS_FS_MAX_PATH];
  unsigned int counter = ++praktor_wasm_test_counter;
  if (salts_fs_get_tmpdir(temp, sizeof(temp)) != 0) return NULL;
#ifdef _WIN32
  snprintf(path, sizeof(path), "%s/turbo_praktor_wasm_%lu_%u", temp,
           (unsigned long)GetCurrentProcessId(), counter);
#else
  snprintf(path, sizeof(path), "%s/turbo_praktor_wasm_%lu_%u", temp,
           (unsigned long)getpid(), counter);
#endif
  if (salts_fs_mkdir(path, 0700) != 0) return NULL;
  return praktor_wasm_strdup(path);
}

static int praktor_wasm_write_workflow(
    const char *workspace, char *out_path, size_t out_size) {
  static const char yaml[] =
      "input_policy: strict\n"
      "inputs:\n"
      "  message:\n"
      "    type: string\n"
      "    required: true\n"
      "outputs:\n"
      "  message:\n"
      "    type: string\n"
      "    required: true\n"
      "    value: \"{{ tasks.echo.outputs.result.message }}\"\n"
      "tasks:\n"
      "  - name: echo\n"
      "    tool: echo_json\n"
      "    with:\n"
      "      message: \"{{ variables.message }}\"\n";
  salts_fs_buf_t buffer = salts_fs_buf_init((void *)yaml, sizeof(yaml) - 1u);
  if (salts_fs_path_join(out_path, out_size, workspace, "wasm-host-tool.yml") != 0)
    return -1;
  return salts_fs_write_file(out_path, &buffer);
}

spec("Praktor HostTool TurboWasm bridge") {
  it("executes a reviewed WasmToolPack projection without backend knowledge in Praktor") {
    char *workspace = praktor_wasm_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_wasm_tool_pack_config_t wasm_pack_config;
    turbo_wasm_tool_pack_module_config_t module_config;
    turbo_wasm_tool_pack_t *wasm_pack = NULL;
    turbo_tool_registry_t *projection = NULL;
    const char *projection_names[] = {"echo_json"};
    turbo_praktor_tool_pack_config_t praktor_pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *praktor_pack = NULL;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    const json_value_t *outputs;

    check_not_null(workspace);
    check_equal(praktor_wasm_write_workflow(
                    workspace, workflow_path, sizeof(workflow_path)),
                0);

    turbo_wasm_tool_pack_config_init(&wasm_pack_config);
    wasm_pack = turbo_wasm_tool_pack_create(&wasm_pack_config);
    check_not_null(wasm_pack);
    turbo_wasm_tool_pack_module_config_init(&module_config);
    module_config.runtime.module_path = LLM_SANDBOX_WASM_TOOL_WASM_PATH;
    module_config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    module_config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
    check_equal(turbo_wasm_tool_pack_add_module(wasm_pack, &module_config),
                TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_project(
                    turbo_wasm_tool_pack_registry(wasm_pack),
                    projection_names, 1, &projection),
                TURBO_TOOL_OK);
    check_not_null(projection);

    turbo_praktor_tool_pack_config_init(&praktor_pack_config);
    praktor_pack = turbo_praktor_tool_pack_create(&praktor_pack_config);
    check_not_null(praktor_pack);
    check_true(turbo_praktor_tool_pack_supports_host_tools(praktor_pack));

    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_wasm_echo";
    workflow_config.description =
        "Reviewed Praktor workflow dispatching through RuntimeTools to TurboWasm.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.strict = 1;
    workflow_config.approved_host_tools = projection;
    check_equal(turbo_praktor_tool_pack_add_workflow(
                    praktor_pack, &workflow_config),
                TURBO_TOOL_OK);

    arguments = json_parse("{\"message\":\"phase3\"}",
                           strlen("{\"message\":\"phase3\"}"));
    check_not_null(arguments);
    check_equal(turbo_tool_registry_execute_json_value(
                    turbo_praktor_tool_pack_registry(praktor_pack),
                    "praktor_wasm_echo", arguments, &result),
                TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_get_string(result, "workflow_status"), "success");
    outputs = json_object_get(result, "outputs");
    check_not_null(outputs);
    check_equal(json_get_string(outputs, "message"), "phase3");

    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(arguments);
    turbo_praktor_tool_pack_destroy(praktor_pack);
    turbo_tool_registry_destroy(projection);
    turbo_wasm_tool_pack_destroy(wasm_pack);
    salts_fs_unlink(workflow_path);
    salts_fs_rmdir(workspace);
    free(workspace);
  }
}
