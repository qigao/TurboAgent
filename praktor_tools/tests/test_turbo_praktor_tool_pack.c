#include "tinytest.h"
#include "turbo_praktor_tool_pack.h"
#include "turbo_runtime_control.h"

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

static unsigned int praktor_test_counter;

static char *praktor_test_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static char *praktor_test_workspace(void) {
  char temp[SALTS_FS_MAX_PATH];
  char path[SALTS_FS_MAX_PATH];
  unsigned int counter = ++praktor_test_counter;
  if (salts_fs_get_tmpdir(temp, sizeof(temp)) != 0) return NULL;
#ifdef _WIN32
  snprintf(path, sizeof(path), "%s/turbo_praktor_%lu_%u", temp,
           (unsigned long)GetCurrentProcessId(), counter);
#else
  snprintf(path, sizeof(path), "%s/turbo_praktor_%lu_%u", temp,
           (unsigned long)getpid(), counter);
#endif
  if (salts_fs_mkdir(path, 0700) != 0) return NULL;
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
  salts_fs_buf_t buffer = salts_fs_buf_init((void *)yaml, sizeof(yaml) - 1);
  if (salts_fs_path_join(out_path, out_size, workspace, "workflow.yml") != 0) return -1;
  return salts_fs_write_file(out_path, &buffer);
}

static int praktor_test_write_failing_workflow(const char *workspace, char *out_path,
                                               size_t out_size) {
  static const char yaml[] =
      "tasks:\n"
      "  - name: fail_expected\n"
      "    script: |\n"
      "      fail(\"expected failure\");\n";
  salts_fs_buf_t buffer = salts_fs_buf_init((void *)yaml, sizeof(yaml) - 1);
  if (salts_fs_path_join(out_path, out_size, workspace, "failure.yml") != 0) return -1;
  return salts_fs_write_file(out_path, &buffer);
}

static int praktor_test_write_named_workflow(const char *workspace, const char *name,
                                             char *out_path, size_t out_size) {
  static const char yaml[] =
      "tasks:\n"
      "  - name: ok\n"
      "    script: |\n"
      "      ctx.output(\"ok\", true);\n";
  salts_fs_buf_t buffer = salts_fs_buf_init((void *)yaml, sizeof(yaml) - 1);
  if (salts_fs_path_join(out_path, out_size, workspace, name) != 0) return -1;
  return salts_fs_write_file(out_path, &buffer);
}

static void praktor_test_cleanup(const char *workspace, const char *workflow_path) {
  if (workflow_path && workflow_path[0]) salts_fs_unlink(workflow_path);
  if (workspace) salts_fs_rmdir(workspace);
}

static void praktor_test_legacy_workflow_config_init(
    turbo_praktor_workflow_config_t *config) {
  turbo_praktor_workflow_config_init(config);
  config->require_harness_safe = 0;
}

static int praktor_test_write_harness_safe_workflow(
    const char *workspace, char *out_path, size_t out_size) {
  static const char yaml[] =
      "input_policy: strict\n"
      "inputs:\n"
      "  payload:\n"
      "    type: object\n"
      "    required: true\n"
      "outputs:\n"
      "  count:\n"
      "    type: integer\n"
      "    required: true\n"
      "    value: \"{{ tasks.inspect.outputs.count }}\"\n"
      "tasks:\n"
      "  - name: inspect\n"
      "    script: |\n"
      "      ctx.output(\"count\", ctx.get(\"payload.count\"));\n";
  salts_fs_buf_t buffer = salts_fs_buf_init((void *)yaml, sizeof(yaml) - 1);
  if (salts_fs_path_join(out_path, out_size, workspace, "harness-safe.yml") != 0) return -1;
  return salts_fs_write_file(out_path, &buffer);
}

typedef struct praktor_test_observation_s {
  int event_count;
  int detail_count;
  int detail_has_tasks;
} praktor_test_observation_t;

static void praktor_test_event_sink(const json_value_t *event, void *user_data) {
  praktor_test_observation_t *capture = (praktor_test_observation_t *)user_data;
  if (!capture || !event) return;
  if (json_get_string(event, "kind") &&
      strcmp(json_get_string(event, "kind"), "trace") == 0) {
    ++capture->event_count;
  }
}

static void praktor_test_detail_sink(const json_value_t *detail, void *user_data) {
  praktor_test_observation_t *capture = (praktor_test_observation_t *)user_data;
  if (!capture || !detail) return;
  ++capture->detail_count;
  capture->detail_has_tasks = json_object_get(detail, "tasks") != NULL;
}

spec("Praktor workflow tool pack") {
  it("propagates generic cancellation into Praktor controlled execution") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *arguments = json_create_object();
    json_value_t *result = NULL;

    check_not_null(workspace);
    check_not_null(arguments);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "cancel.yml", workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_cancel";
    workflow_config.description = "Cancellation propagation test.";
    workflow_config.workflow_path = workflow_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);

    check_equal(turbo_cancel_source_create(NULL, &source), 0);
    check_equal(turbo_cancel_source_token(source, &token), 0);
    check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), 0);
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;
    context.turn_id = "turn-cancel";
    context.tool_call_id = "call-cancel";

    check_equal(turbo_tool_registry_execute_json_value_with_context(
                     turbo_praktor_tool_pack_registry(pack), "praktor_cancel",
                     arguments, &context, &result),
                 TURBO_TOOL_CANCELLED);
    check_null(result);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    turbo_runtime_json_destroy(arguments);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("preserves deadline-exceeded semantics at the Praktor boundary") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    turbo_cancel_source_config_t cancel_config = {0};
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *arguments = json_create_object();
    json_value_t *result = NULL;

    check_not_null(workspace);
    check_not_null(arguments);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "deadline.yml", workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_deadline";
    workflow_config.description = "Deadline propagation test.";
    workflow_config.workflow_path = workflow_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);

    cancel_config.struct_size = sizeof(cancel_config);
    cancel_config.abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION;
    cancel_config.deadline_mono_ms = 1u;
    check_equal(turbo_cancel_source_create(&cancel_config, &source), 0);
    check_equal(turbo_cancel_source_token(source, &token), 0);
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;
    context.deadline_mono_ms = 1u;

    check_equal(turbo_tool_registry_execute_json_value_with_context(
                     turbo_praktor_tool_pack_registry(pack), "praktor_deadline",
                     arguments, &context, &result),
                 TURBO_TOOL_DEADLINE_EXCEEDED);
    check_null(result);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    turbo_runtime_json_destroy(arguments);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("registers a reviewed workflow with conservative policy metadata") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    turbo_tool_execution_policy_t policy = {0};
    const char *const *capabilities = NULL;
    size_t capability_count = 0;

    check_not_null(workspace);
    check_equal(praktor_test_write_workflow(workspace, workflow_path, sizeof(workflow_path)), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);

    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_inspect";
    workflow_config.description = "Run the reviewed inspect workflow.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.parameters_json =
        "{\"type\":\"object\",\"properties\":{\"payload\":{\"type\":\"object\"}},"
        "\"required\":[\"payload\"],\"additionalProperties\":false}";
    workflow_config.strict = 1;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_equal(turbo_praktor_tool_pack_workflow_count(pack), 1);
    check_equal(turbo_tool_registry_get_execution_policy(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect", &policy),
                 TURBO_TOOL_OK);
    check_equal(policy.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_equal(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    check_equal(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_equal(capability_count, 5);
    check_equal(capabilities[0], "runtime_tools");
    check_equal(capabilities[1], "network");
    check_equal(capabilities[2], "shell");
    check_equal(capabilities[3], "patch");
    check_equal(capabilities[4], "outside_workspace");

    turbo_praktor_tool_pack_destroy(pack);

    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    workflow_config.required_capabilities = NULL;
    workflow_config.required_capability_count = 0;
    workflow_config.tool_name = "praktor_zero_caps";
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_zero_caps",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_equal(capability_count, 5);
    check_equal(capabilities[0], "runtime_tools");
    check_equal(capabilities[1], "network");
    check_equal(capabilities[2], "shell");
    check_equal(capabilities[3], "patch");
    check_equal(capabilities[4], "outside_workspace");

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("executes structured inputs and returns canonical Praktor JSON") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    json_value_t *tasks;
    json_value_t *inspect;
    json_value_t *outputs;
    char *string_output = NULL;
    json_value_t *string_result = NULL;

    check_not_null(workspace);
    check_equal(praktor_test_write_workflow(workspace, workflow_path, sizeof(workflow_path)), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_inspect";
    workflow_config.description = "Run inspect.";
    workflow_config.workflow_path = workflow_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    arguments = json_parse("{\"payload\":{\"name\":\"demo\",\"count\":7}}",
                           strlen("{\"payload\":{\"name\":\"demo\",\"count\":7}}"));
    check_not_null(arguments);
    check_equal(turbo_tool_registry_execute_json_value(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect",
                     arguments, &result),
                 TURBO_TOOL_OK);
    check_equal(json_get_string(result, "workflow_status"), "success");
    tasks = json_object_get(result, "tasks");
    inspect = json_object_get(tasks, "inspect");
    outputs = json_object_get(inspect, "outputs");
    check_equal((int)json_get_double(outputs, "count", -1.0), 7);

    check_equal(turbo_tool_registry_execute(
                     turbo_praktor_tool_pack_registry(pack), "praktor_inspect",
                     "{\"payload\":{\"name\":\"demo\",\"count\":7}}",
                     &string_output),
                 TURBO_TOOL_OK);
    check_not_null(string_output);
    string_result = json_parse(string_output, strlen(string_output));
    check_not_null(string_result);
    check_equal(json_get_string(string_result, "workflow_status"), "success");

    json_free(arguments);
    json_free(result);
    json_free(string_result);
    free(string_output);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("returns workflow failure as structured tool output") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    json_value_t *arguments = json_create_object();
    json_value_t *result = NULL;

    check_not_null(workspace);
    check_equal(praktor_test_write_failing_workflow(
                     workspace, workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_failure";
    workflow_config.description = "Run a workflow that fails by design.";
    workflow_config.workflow_path = workflow_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_execute_json_value(
                     turbo_praktor_tool_pack_registry(pack), "praktor_failure",
                     arguments, &result),
                 TURBO_TOOL_OK);
    check_equal(json_get_string(result, "workflow_status"), "failed");
    check_not_null(json_get_string(result, "error"));

    turbo_runtime_json_destroy(arguments);
    json_free(result);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("enforces the configured Praktor result byte bound") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;

    check_not_null(workspace);
    check_equal(praktor_test_write_workflow(workspace, workflow_path, sizeof(workflow_path)), 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack_config.max_result_bytes = 1;
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_bounded";
    workflow_config.description = "Exercise the result bound.";
    workflow_config.workflow_path = workflow_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    arguments = json_parse("{\"payload\":{\"name\":\"demo\",\"count\":7}}",
                           strlen("{\"payload\":{\"name\":\"demo\",\"count\":7}}"));
    check_not_null(arguments);
    check_equal(turbo_tool_registry_execute_json_value(
                     turbo_praktor_tool_pack_registry(pack), "praktor_bounded",
                     arguments, &result),
                 TURBO_TOOL_ERROR);
    check_null(result);

    json_free(arguments);
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("rejects relative paths, duplicate names, and workflow capacity overflow") {
    char *workspace = praktor_test_workspace();
    char first_path[SALTS_FS_MAX_PATH] = {0};
    char second_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;

    check_not_null(workspace);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "one.yml", first_path, sizeof(first_path)),
                 0);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "two.yml", second_path, sizeof(second_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack_config.max_workflows = 1;
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);

    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_one";
    workflow_config.description = "One.";
    workflow_config.workflow_path = "relative.yml";
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_INVALID_ARGUMENT);

    {
      char missing_path[SALTS_FS_MAX_PATH] = {0};
      check_equal(salts_fs_path_join(missing_path, sizeof(missing_path), workspace, "missing.yml"),
                   0);
      workflow_config.workflow_path = missing_path;
      check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                   TURBO_TOOL_INVALID_ARGUMENT);
    }

    workflow_config.workflow_path = first_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    workflow_config.tool_name = "praktor_two";
    workflow_config.workflow_path = second_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_BACKPRESSURE);

    turbo_praktor_tool_pack_destroy(pack);

    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    workflow_config.tool_name = "praktor_one";
    workflow_config.workflow_path = first_path;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config),
                 TURBO_TOOL_DUPLICATE);
    turbo_praktor_tool_pack_destroy(pack);
    salts_fs_unlink(first_path);
    salts_fs_unlink(second_path);
    praktor_test_cleanup(workspace, NULL);
    free(workspace);
  }

  it("allows an explicitly reviewed workflow to narrow default capabilities") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    const char *reviewed_capabilities[] = {"network"};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    const char *const *capabilities = NULL;
    size_t capability_count = 0;

    check_not_null(workspace);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "net.yml", workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    praktor_test_legacy_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_network";
    workflow_config.description = "Reviewed network-only workflow.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.required_capabilities = reviewed_capabilities;
    workflow_config.required_capability_count = 1;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_network",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    if (turbo_praktor_tool_pack_supports_workflow_plan(pack)) {
      check_equal(capability_count, 2);
      check_equal(capabilities[0], "runtime_tools");
      check_equal(capabilities[1], "network");
    } else {
      check_equal(capability_count, 5);
      check_equal(capabilities[0], "runtime_tools");
      check_equal(capabilities[1], "network");
      check_equal(capabilities[2], "shell");
      check_equal(capabilities[3], "patch");
      check_equal(capabilities[4], "outside_workspace");
    }

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("accepts workflow config v1 prefix callers") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;

    check_not_null(workspace);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "v1.yml", workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);

    memset(&workflow_config, 0, sizeof(workflow_config));
    workflow_config.struct_size = TURBO_PRAKTOR_WORKFLOW_CONFIG_V1_SIZE;
    workflow_config.abi_version = TURBO_PRAKTOR_WORKFLOW_CONFIG_ABI_VERSION_V1;
    workflow_config.tool_name = "praktor_v1";
    workflow_config.description = "v1 config compatibility.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    workflow_config.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("uses WorkflowPlan contracts for harness-native registration and execution") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;
    turbo_tool_definition_t definition = {0};
    const char *const *capabilities = NULL;
    size_t capability_count = 0;
    json_value_t *schema = NULL;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    turbo_tool_execution_context_t context = {0};
    praktor_test_observation_t observation = {0};

    check_not_null(workspace);
    check_equal(praktor_test_write_harness_safe_workflow(
                     workspace, workflow_path, sizeof(workflow_path)),
                 0);

    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    if (turbo_praktor_tool_pack_supports_workflow_plan(pack)) {
      turbo_praktor_workflow_config_init(&workflow_config);
      workflow_config.tool_name = "praktor_harness_safe";
    workflow_config.description = "Harness-safe plan-backed workflow.";
    workflow_config.workflow_path = workflow_path;
    workflow_config.strict = 1;
    check_equal(turbo_praktor_tool_pack_add_workflow(pack, &workflow_config), TURBO_TOOL_OK);

    check_equal(turbo_tool_registry_get_definition(
                     turbo_praktor_tool_pack_registry(pack), 0, &definition),
                 TURBO_TOOL_OK);
    schema = json_parse(definition.parameters_json, strlen(definition.parameters_json));
    check_not_null(schema);
    check_equal(json_get_string(schema, "type"), "object");
    check_false(json_get_bool(schema, "additionalProperties", true));
    check_not_null(json_object_get(json_object_get(schema, "properties"), "payload"));

    check_equal(turbo_tool_registry_get_required_capabilities(
                     turbo_praktor_tool_pack_registry(pack), "praktor_harness_safe",
                     &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_equal(capability_count, 1);
    check_equal(capabilities[0], "runtime_tools");

    arguments = json_parse("{\"payload\":{\"count\":7}}",
                           strlen("{\"payload\":{\"count\":7}}"));
    check_not_null(arguments);
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.thread_id = "thread-plan";
    context.run_id = "run-plan";
    context.turn_id = "turn-plan";
    context.tool_call_id = "call-plan";
    context.event_sink = praktor_test_event_sink;
    context.event_sink_user_data = &observation;
    context.detail_sink = praktor_test_detail_sink;
    context.detail_sink_user_data = &observation;

    check_equal(turbo_tool_registry_execute_json_value_with_context(
                     turbo_praktor_tool_pack_registry(pack), "praktor_harness_safe",
                     arguments, &context, &result),
                 TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_get_string(result, "workflow_status"), "success");
    check_null(json_object_get(result, "tasks"));
    check_equal((int)json_get_double(json_object_get(result, "outputs"), "count", -1.0), 7);
    check_equal(observation.detail_count, 1);
    check_true(observation.detail_has_tasks);
    if (turbo_praktor_tool_pack_supports_execution_events(pack)) {
      check_true(observation.event_count >= 4);
    }

    turbo_runtime_json_destroy(result);
    result = NULL;

    /* The bound plan must detect reviewed bytes changing after registration. */
    {
      static const char changed[] =
          "input_policy: strict\n"
          "inputs:\n"
          "  payload:\n"
          "    type: object\n"
          "    required: true\n"
          "outputs:\n"
          "  count:\n"
          "    type: integer\n"
          "    required: true\n"
          "    value: \"{{ tasks.inspect.outputs.count }}\"\n"
          "tasks:\n"
          "  - name: inspect\n"
          "    script: |\n"
          "      ctx.output(\"count\", 99);\n";
      salts_fs_buf_t buffer =
          salts_fs_buf_init((void *)changed, sizeof(changed) - 1);
      check_equal(salts_fs_write_file(workflow_path, &buffer), 0);
    }

    check_equal(turbo_tool_registry_execute_json_value_with_context(
                     turbo_praktor_tool_pack_registry(pack), "praktor_harness_safe",
                     arguments, &context, &result),
                 TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(json_get_string(result, "workflow_status"), "error");
    check_equal(json_get_string(result, "error_phase"), "plan");

      turbo_runtime_json_destroy(schema);
      turbo_runtime_json_destroy(arguments);
      turbo_runtime_json_destroy(result);
    } else {
      check_true(1);
    }
    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

  it("requires harness-safe qualification by default for config v2") {
    char *workspace = praktor_test_workspace();
    char workflow_path[SALTS_FS_MAX_PATH] = {0};
    turbo_praktor_tool_pack_config_t pack_config;
    turbo_praktor_workflow_config_t workflow_config;
    turbo_praktor_tool_pack_t *pack;

    check_not_null(workspace);
    check_equal(praktor_test_write_named_workflow(
                     workspace, "unsafe.yml", workflow_path, sizeof(workflow_path)),
                 0);
    turbo_praktor_tool_pack_config_init(&pack_config);
    pack = turbo_praktor_tool_pack_create(&pack_config);
    check_not_null(pack);
    turbo_praktor_workflow_config_init(&workflow_config);
    workflow_config.tool_name = "praktor_unsafe_default";
    workflow_config.description = "Missing strict contract and output.";
    workflow_config.workflow_path = workflow_path;
    {
      turbo_tool_status_t add_status =
          turbo_praktor_tool_pack_add_workflow(pack, &workflow_config);
      if (turbo_praktor_tool_pack_supports_workflow_plan(pack)) {
        check_equal(add_status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      } else {
        check_equal(add_status, TURBO_TOOL_OK);
      }
    }

    turbo_praktor_tool_pack_destroy(pack);
    praktor_test_cleanup(workspace, workflow_path);
    free(workspace);
  }

}
