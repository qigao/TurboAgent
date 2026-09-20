#include "tinytest.h"
#include "turbo_praktor_tool_pack.h"

#include <turbo_fs.h>
#include <turbo_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static unsigned int praktor_test_counter;

static char *praktor_test_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static char *praktor_test_workspace(void) {
  char temp[TURBO_FS_MAX_PATH];
  char path[TURBO_FS_MAX_PATH];
  unsigned int counter = ++praktor_test_counter;
  if (turbo_fs_get_tmpdir(temp, sizeof(temp)) != 0) return NULL;
#ifdef _WIN32
  snprintf(path, sizeof(path), "%s/turbo_praktor_%lu_%u", temp,
           (unsigned long)GetCurrentProcessId(), counter);
#else
  snprintf(path, sizeof(path), "%s/turbo_praktor_%lu_%u", temp,
           (unsigned long)getpid(), counter);
#endif
  if (turbo_fs_mkdir(path, 0700) != 0) return NULL;
  return praktor_test_strdup(path);
}

static int praktor_test_write_workflow(const char *workspace, char *out_path,
                                       size_t out_size) {
  static const char yaml[] =
      "tasks:\n"
      "  - name: inspect\n"
      "    script: |\n"
      "      if (ctx.get(\"payload.name\") != \"demo\") fail(\"bad name\");\n"
      "      if (ctx.get(\"payload.count\") != 7) fail(\"bad count\");\n"
      "      ctx.output(\"count\", ctx.get(\"payload.count\"));\n";
  turbo_fs_buf_t buffer = turbo_fs_buf_init((void *)yaml, sizeof(yaml) - 1);
  if (turbo_fs_path_join(out_path, out_size, workspace, "workflow.yml") != 0) return -1;
  return turbo_fs_write_file(out_path, &buffer);
}

static void praktor_test_cleanup(const char *workspace, const char *workflow_path) {
  if (workflow_path && workflow_path[0]) turbo_fs_unlink(workflow_path);
  if (workspace) turbo_fs_rmdir(workspace);
}

spec("Praktor workflow tool pack") {
  it("registers a reviewed workflow with conservative policy metadata") {
    char *workspace = praktor_test_workspace();
    char workflow_path[TURBO_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    turbo_tool_execution_policy_t policy = {0};
    const char *const *capabilities = NULL;
    size_t capability_count = 0;

    check_not_null(workspace);
    check_int_eq(praktor_test_write_workflow(workspace, workflow_path, sizeof(workflow_path)), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);

    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_inspect";
    workflow_config.description = "Run the reviewed inspect workflow.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.parameters_json =
        "{\"type\":\"object\",\"properties\":{\"payload\":{\"type\":\"object\"}},"
        "\"required\":[\"payload\"],\"additionalProperties\":false}";
    workflow_config.strict = 1;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_size_eq(turbo_praktor_tool_pack_workflow_count(pack), 1);
    check_int_eq(turbo_tool_registry_get_execution_policy(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect", &policy),
                 TURBO_TOOL_OK);
    check_int_eq(policy.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_int_eq(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    check_int_eq(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_size_eq(capability_count, 5);
    check_str_eq(capabilities[0], "runtime_tools");
    check_str_eq(capabilities[1], "network");
    check_str_eq(capabilities[2], "shell");
    check_str_eq(capabilities[3], "patch");
    check_str_eq(capabilities[4], "outside_workspace");

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("executes structured inputs and returns canonical Praktor JSON") {
    char *workspace = praktor_test_workspace();
    char workflow_path[TURBO_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    json_value_t *tasks;
    json_value_t *inspect;
    json_value_t *outputs;

    check_not_null(workspace);
    check_int_eq(praktor_test_write_workflow(workspace, workflow_path, sizeof(workflow_path)), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_inspect";
    workflow_config.description = "Run inspect.";
    workflow_config.workflow_path = workflow_path;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_int_eq(turbo_parse_json(
                     (const uint8_t *)"{\"payload\":{\"name\":\"demo\",\"count\":7}}",
                     strlen("{\"payload\":{\"name\":\"demo\",\"count\":7}}"),
                     &arguments),
                 0);
    check_int_eq(turbo_tool_registry_execute_json_value(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect",
                     arguments, &result),
                 TURBO_TOOL_OK);
    check_str_eq(turbo_json_get_string(result, "workflow_status"), "success");
    tasks = turbo_json_object_get(result, "tasks");
    inspect = turbo_json_object_get(tasks, "inspect");
    outputs = turbo_json_object_get(inspect, "outputs");
    check_int_eq((int)turbo_json_get_double(outputs, "count", -1.0), 7);

    turbo_free_json(&arguments);
    turbo_free_json(&result);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("returns workflow failure as structured tool output") {
    char *workspace = praktor_test_workspace();
    char missing_path[TURBO_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    json_value_t *arguments = turbo_json_create_object();
    json_value_t *result = NULL;

    check_not_null(workspace);
    check_int_eq(turbo_fs_path_join(missing_path, sizeof(missing_path), workspace, "missing.yml"),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_missing";
    workflow_config.description = "Run a missing workflow for failure-contract testing.";
    workflow_config.workflow_path = missing_path;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_execute_json_value(
                     turbo_praktor_tool_pack_registry(pack), "praktor_missing",
                     arguments, &result),
                 TURBO_TOOL_OK);
    check_str_eq(turbo_json_get_string(result, "workflow_status"), "failed");
    check_not_null(turbo_json_get_string(result, "error"));

    turbo_runtime_json_destroy(arguments);
    turbo_free_json(&result);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, NULL);
    free(workspace);
  }

  it("rejects relative paths, duplicate names, and workflow capacity overflow") {
    char *workspace = praktor_test_workspace();
    char first_path[TURBO_FS_MAX_PATH] = {0};
    char second_path[TURBO_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;

    check_not_null(workspace);
    check_int_eq(turbo_fs_path_join(first_path, sizeof(first_path), workspace, "one.yml"), 0);
    check_int_eq(turbo_fs_path_join(second_path, sizeof(second_path), workspace, "two.yml"), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack_config.max_workflows = 1;
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);

    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_one";
    workflow_config.description = "One.";
    workflow_config.workflow_path = "relative.yml";
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_INVALID_ARGUMENT);

    workflow_config.workflow_path = first_path;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    workflow_config.tool_name = "praktor_two";
    workflow_config.workflow_path = second_path;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_BACKPRESSURE);

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, NULL);
    free(workspace);

    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    workflow_config.tool_name = "praktor_one";
    workflow_config.workflow_path = first_path;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_DUPLICATE);
    turbo_praktor_tool_pack_destroy(pack);
  }

  it("allows an explicitly reviewed workflow to narrow default capabilities") {
    char *workspace = praktor_test_workspace();
    char workflow_path[TURBO_FS_MAX_PATH] = {0};
    const char *reviewed_capabilities[] = {"network"};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    const char *const *capabilities = NULL;
    size_t capability_count = 0;

    check_not_null(workspace);
    check_int_eq(turbo_fs_path_join(workflow_path, sizeof(workflow_path), workspace, "net.yml"), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_network";
    workflow_config.description = "Reviewed network-only workflow.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.required_capabilities = reviewed_capabilities;
    workflow_config.required_capability_count = 1;
    check_int_eq(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_network",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_size_eq(capability_count, 2);
    check_str_eq(capabilities[0], "runtime_tools");
    check_str_eq(capabilities[1], "network");

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, NULL);
    free(workspace);
  }
}
