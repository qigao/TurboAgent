#include "tinytest.h"
#include "turbo_agent_workspace.h"
#include "../examples/engineering_workflow_checks.h"
#include <cmeta_fs.h>
#include <salts/clock.h>
#include <stdlib.h>

typedef struct engineering_fixture_s {
  int build_calls, test_calls, lineage_calls;
  int build_exit, test_exit, total, passed, failed, skipped, malformed_build;
  const char *build_revision, *test_revision, *test_artifact;
  turbo_cancel_source_t *cancel_source;
  int cancel_during_build;
  json_value_t *detail;
} engineering_fixture_t;

static void engineering_observe_detail(const json_value_t *detail, void *user) {
  engineering_fixture_t *fixture = user;
  json_free(fixture->detail);
  fixture->detail = json_clone(detail);
}

static int engineering_has_lineage(const turbo_tool_execution_context_t *context) {
  return context && context->struct_size >= sizeof(*context) && context->thread_id &&
         context->run_id && context->turn_id && context->tool_call_id &&
         strcmp(context->thread_id, "eng-thread") == 0 &&
         strcmp(context->run_id, "eng-run") == 0 &&
         strcmp(context->turn_id, "eng-turn") == 0 &&
         strcmp(context->tool_call_id, "eng-call") == 0 && context->deadline_mono_ms > 0;
}

/* Offline backend fixtures only. Production Hosts supply their own bounded
 * build/test implementations; no fake backend is exported by the example. */
static turbo_tool_status_t engineering_fixture_build(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  engineering_fixture_t *fixture = user;
  json_value_t *result = json_create_object();
  (void)args;
  *out = NULL;
  ++fixture->build_calls;
  fixture->lineage_calls += engineering_has_lineage(context);
  if (!result ||
      turbo_runtime_json_object_set_string(result, "source_revision", fixture->build_revision) ||
      turbo_runtime_json_object_set_string(result, "artifact_id", "artifact-1") ||
      (fixture->malformed_build
           ? turbo_runtime_json_object_set_string(result, "exit_code", "0")
           : turbo_runtime_json_object_set_int64(result, "exit_code", fixture->build_exit))) {
    json_free(result);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  if (fixture->cancel_during_build) turbo_cancel_source_cancel(fixture->cancel_source, TURBO_CANCEL_USER);
  *out = result;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t engineering_fixture_test(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  engineering_fixture_t *fixture = user;
  json_value_t *result = NULL;
  const char *artifact = engineering_id(args, "artifact_id");
  *out = NULL;
  ++fixture->test_calls;
  fixture->lineage_calls += engineering_has_lineage(context);
  if (!artifact || strcmp(artifact, "artifact-1") != 0 || fixture->build_calls != 1)
    return TURBO_TOOL_INVALID_ARGUMENT;
  result = json_create_object();
  if (!result ||
      turbo_runtime_json_object_set_string(result, "source_revision", fixture->test_revision) ||
      turbo_runtime_json_object_set_string(result, "artifact_id", fixture->test_artifact) ||
      turbo_runtime_json_object_set_int64(result, "exit_code", fixture->test_exit) ||
      turbo_runtime_json_object_set_int64(result, "tests_total", fixture->total) ||
      turbo_runtime_json_object_set_int64(result, "tests_passed", fixture->passed) ||
      turbo_runtime_json_object_set_int64(result, "tests_failed", fixture->failed) ||
      turbo_runtime_json_object_set_int64(result, "tests_skipped", fixture->skipped)) {
    json_free(result);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  *out = result;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t engineering_fixture_add_tools(
    turbo_tool_registry_t *registry, engineering_fixture_t *fixture) {
  static const char *const required[] = {"shell", "patch"};
  static const char test_schema[] =
      "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
      "\"preset\":{\"type\":\"string\"},\"source_revision\":{\"type\":\"string\"},"
      "\"artifact_id\":{\"type\":\"string\"}},\"required\":[\"preset\",\"source_revision\",\"artifact_id\"]}";
  size_t i;
  for (i = 0; i < 2; ++i) {
    turbo_tool_definition_v6_t definition = {0};
    turbo_tool_status_t status;
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition.name = i == 0 ? "engineering.build" : "engineering.test";
    definition.definition.description = "Offline engineering backend fixture.";
    definition.definition.parameters_json = i == 0 ? engineering_request_schema : test_schema;
    definition.definition.strict = 1;
    definition.definition.user_data = fixture;
    definition.json_value_context_handler = i == 0 ? engineering_fixture_build : engineering_fixture_test;
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
    definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
    definition.required_capabilities = required;
    definition.required_capability_count = 2;
    definition.result_schema_json = "{\"type\":\"object\",\"additionalProperties\":true}";
    definition.strict_result = 1;
    definition.effect_flags = TURBO_TOOL_EFFECT_PROCESS | TURBO_TOOL_EFFECT_WRITE;
    status = turbo_tool_registry_add_v6(registry, &definition);
    if (status != TURBO_TOOL_OK) return status;
  }
  return engineering_register_checks(registry);
}

static turbo_tool_status_t engineering_fixture_execute(
    turbo_praktor_tool_pack_t *pack, engineering_fixture_t *fixture,
    const json_value_t *arguments, const turbo_cancel_token_t *token, json_value_t **out) {
  turbo_tool_execution_context_t context = {0};
  context.struct_size = sizeof(context);
  context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
  context.cancel_token = token;
  context.deadline_mono_ms = cmeta_monotonic_ms() + 10000;
  context.thread_id = "eng-thread";
  context.run_id = "eng-run";
  context.turn_id = "eng-turn";
  context.tool_call_id = "eng-call";
  context.detail_sink = engineering_observe_detail;
  context.detail_sink_user_data = fixture;
  return turbo_tool_registry_execute_json_value_with_context(
      turbo_praktor_tool_pack_registry(pack), "engineering_build_test", arguments, &context, out);
}

spec("reviewed engineering build/test workflow") {
  static engineering_fixture_t fixture;
  static char *root;
  static char workflow_path[SALTS_FS_MAX_PATH];
  static turbo_tool_registry_t *tools, *projection;
  static turbo_praktor_tool_pack_t *pack;
  static json_value_t *arguments, *result;

  before_each() {
    static const char input[] = "{\"preset\":\"debug\",\"source_revision\":\"rev-1\"}";
    static const char *const names[] = {"engineering.build", "engineering.test",
        "engineering.check_request", "engineering.check_build", "engineering.check_tests"};
    cmeta_fs_buf_t buffer = {0};
    turbo_praktor_tool_pack_config_t config;
    fixture = (engineering_fixture_t){0};
    fixture.total = fixture.passed = 4;
    fixture.build_revision = fixture.test_revision = "rev-1";
    fixture.test_artifact = "artifact-1";
    root = tt_make_temp_dir("engineering_workflow");
    check_not_null(root);
    check_equal(cmeta_fs_path_join(workflow_path, sizeof(workflow_path), root, "workflow.yml"), 0);
    check_equal(cmeta_fs_read_file(ENGINEERING_WORKFLOW_SOURCE, &buffer), 0);
    check_equal(cmeta_fs_write_file(workflow_path, &buffer), 0);
    cmeta_fs_buf_free(&buffer);
    tools = turbo_tool_registry_create();
    check_not_null(tools);
    check_equal(engineering_fixture_add_tools(tools, &fixture), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_project(tools, names, 5, &projection), TURBO_TOOL_OK);
    turbo_praktor_tool_pack_config_init(&config);
    config.max_workflows = 1;
    config.max_result_bytes = 65536;
    pack = turbo_praktor_tool_pack_create(&config);
    check_not_null(pack);
    check_equal(engineering_register_workflow(pack, workflow_path, projection), TURBO_TOOL_OK);
    arguments = json_parse(input, sizeof(input) - 1);
    check_not_null(arguments);
    result = NULL;
  }

  after_each() {
    json_free(result); result = NULL;
    json_free(arguments); arguments = NULL;
    json_free(fixture.detail); fixture.detail = NULL;
    turbo_praktor_tool_pack_destroy(pack); pack = NULL;
    turbo_tool_registry_destroy(projection); projection = NULL;
    turbo_tool_registry_destroy(tools); tools = NULL;
    check_equal(tt_remove_tree(root), 0);
    free(root); root = NULL;
  }

  it("verifies source, artifact and actual test counts through the reviewed five-step DAG") {
    check_equal(engineering_fixture_execute(pack, &fixture, arguments, NULL, &result), TURBO_TOOL_OK);
    check_true(engineering_workflow_verified(result, arguments));
    check_equal(fixture.build_calls, 1);
    check_equal(fixture.test_calls, 1);
    check_equal(fixture.lineage_calls, 2);
    check_not_null(fixture.detail);
    check_not_null(json_object_get(fixture.detail, "tasks"));
    check_null(json_object_get(result, "tasks"));
  }

  for (int scenario = 0; scenario < 3; ++scenario) {
    it("blocks testing after invalid build evidence, scenario %d", scenario) {
      if (scenario == 0) fixture.build_exit = 17;
      if (scenario == 1) fixture.build_revision = "stale";
      if (scenario == 2) fixture.malformed_build = 1;
      engineering_fixture_execute(pack, &fixture, arguments, NULL, &result);
      check_false(engineering_workflow_verified(result, arguments));
      check_equal(fixture.build_calls, 1);
      check_equal(fixture.test_calls, 0);
    }
  }

  for (int scenario = 0; scenario < 7; ++scenario) {
    it("rejects incomplete test evidence even after successful tool transport, scenario %d", scenario) {
      if (scenario == 0) { fixture.failed = 1; fixture.passed = 3; }
      if (scenario == 1) { fixture.skipped = 1; fixture.passed = 3; }
      if (scenario == 2) { fixture.total = fixture.passed = 0; }
      if (scenario == 3) fixture.test_exit = 1;
      if (scenario == 4) fixture.test_revision = "stale";
      if (scenario == 5) fixture.test_artifact = "other-artifact";
      if (scenario == 6) fixture.passed = 3;
      engineering_fixture_execute(pack, &fixture, arguments, NULL, &result);
      check_false(engineering_workflow_verified(result, arguments));
      check_equal(fixture.build_calls, 1);
      check_equal(fixture.test_calls, 1);
    }
  }

  for (int scenario = 0; scenario < 4; ++scenario) {
    it("rejects an invalid request before the build callback, scenario %d", scenario) {
      if (scenario == 0) check_equal(turbo_runtime_json_object_set_string(arguments, "preset", "debug; command"), 0);
      if (scenario == 1) check_equal(turbo_runtime_json_object_set_string(arguments, "source_revision", "../outside"), 0);
      if (scenario == 2) check_equal(turbo_runtime_json_object_set_string(arguments, "command", "ignored"), 0);
      if (scenario == 3) {
        static const char bytes[] = "debug\0hidden";
        json_value_t *value = json_create_string_n(bytes, sizeof(bytes) - 1);
        check_not_null(value);
        check_equal(turbo_runtime_json_object_set(arguments, "preset", value), TURBO_RUNTIME_JSON_OK);
      }
      engineering_fixture_execute(pack, &fixture, arguments, NULL, &result);
      check_false(engineering_workflow_verified(result, arguments));
      check_equal(fixture.build_calls, 0);
      check_equal(fixture.test_calls, 0);
    }
  }

  for (int during_build = 0; during_build <= 1; ++during_build) {
    it("propagates cancellation without dispatching tests, during_build=%d", during_build) {
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      check_equal(turbo_cancel_source_create(NULL, &source), 0);
      check_equal(turbo_cancel_source_token(source, &token), 0);
      fixture.cancel_source = source;
      fixture.cancel_during_build = during_build;
      if (!during_build) check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), 0);
      check_equal(engineering_fixture_execute(pack, &fixture, arguments, token, &result), TURBO_TOOL_CANCELLED);
      check_false(engineering_workflow_verified(result, arguments));
      check_equal(fixture.build_calls, during_build);
      check_equal(fixture.test_calls, 0);
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      fixture.cancel_source = NULL;
    }
  }

  it("refuses changed workflow bytes before either effect callback") {
    static const char changed[] = "input_policy: strict\ntasks: []\n";
    cmeta_fs_buf_t buffer = cmeta_fs_buf_init((void *)changed, sizeof(changed) - 1);
    check_equal(cmeta_fs_write_file(workflow_path, &buffer), 0);
    engineering_fixture_execute(pack, &fixture, arguments, NULL, &result);
    check_false(engineering_workflow_verified(result, arguments));
    check_equal(fixture.build_calls, 0);
    check_equal(fixture.test_calls, 0);
  }

  it("rejects registration without all five approved tool identities") {
    turbo_praktor_tool_pack_config_t config;
    turbo_praktor_tool_pack_t *denied;
    turbo_tool_registry_t *empty = turbo_tool_registry_create();
    check_not_null(empty);
    turbo_praktor_tool_pack_config_init(&config);
    denied = turbo_praktor_tool_pack_create(&config);
    check_not_null(denied);
    check_not_equal(engineering_register_workflow(denied, workflow_path, empty), TURBO_TOOL_OK);
    check_equal(turbo_praktor_tool_pack_workflow_count(denied), 0u);
    check_equal(fixture.build_calls, 0);
    turbo_praktor_tool_pack_destroy(denied);
    turbo_tool_registry_destroy(empty);
  }

  it("requires every Host grant before projecting the Skill macro tool") {
    static const char skill[] =
        "---\nname: build-and-test\ndescription: Run reviewed engineering checks\n"
        "tools: [engineering_build_test]\ncapabilities: [runtime_tools, shell, patch]\n---\n"
        "Use Host presets and verify the actual tool result before declaring completion.\n";
    char skills[SALTS_FS_MAX_PATH], folder[SALTS_FS_MAX_PATH], path[SALTS_FS_MAX_PATH];
    cmeta_fs_buf_t buffer = cmeta_fs_buf_init((void *)skill, sizeof(skill) - 1);
    check_equal(cmeta_fs_path_join(skills, sizeof(skills), root, "skills"), 0);
    check_equal(cmeta_fs_path_join(folder, sizeof(folder), skills, "build-and-test"), 0);
    check_equal(cmeta_fs_path_join(path, sizeof(path), folder, "SKILL.md"), 0);
    check_equal(cmeta_fs_mkdir(skills, 0700), 0);
    check_equal(cmeta_fs_mkdir(folder, 0700), 0);
    check_equal(cmeta_fs_write_file(path, &buffer), 0);
    for (int missing = 0; missing < 4; ++missing) {
      turbo_agent_policy_t policy = turbo_agent_policy_default();
      turbo_agent_workspace_config_t config;
      turbo_agent_workspace_t *workspace = NULL;
      turbo_agent_workspace_selection_t *selection = NULL;
      policy.allow_runtime_tools = missing != 0;
      policy.allow_shell = missing != 1;
      policy.allow_patch = missing != 2;
      turbo_agent_workspace_config_init(&config);
      config.workspace_root = root;
      config.policy = &policy;
      check_equal(turbo_agent_workspace_create(&config, &workspace), TURBO_AGENT_WORKSPACE_OK);
      check_equal(turbo_agent_workspace_prepare(workspace, "$build-and-test",
          turbo_praktor_tool_pack_registry(pack), NULL, &selection),
          missing == 3 ? TURBO_AGENT_WORKSPACE_OK : TURBO_AGENT_WORKSPACE_CAPABILITY_DENIED);
      if (missing == 3) {
        const turbo_tool_registry_t *selected = turbo_agent_workspace_selection_tools(selection);
        check_equal(turbo_agent_policy_check_tool(&policy, selected, "engineering_build_test", NULL),
                    TURBO_AGENT_POLICY_ALLOW);
        check_equal(turbo_agent_workspace_selection_tool_count(selection), 1u);
      } else check_null(selection);
      turbo_agent_workspace_selection_destroy(selection);
      turbo_agent_workspace_destroy(workspace);
    }
    check_equal(fixture.build_calls, 0);
    check_equal(fixture.test_calls, 0);
  }

  it("does not accept transport success or altered delivery summaries as completion") {
    json_value_t *outputs;
    check_equal(engineering_fixture_execute(pack, &fixture, arguments, NULL, &result), TURBO_TOOL_OK);
    check_true(engineering_workflow_verified(result, arguments));
    outputs = json_object_get(result, "outputs");
    check_equal(turbo_runtime_json_object_set_int64(outputs, "tests_skipped", 1), 0);
    check_false(engineering_workflow_verified(result, arguments));
    check_equal(turbo_runtime_json_object_set_int64(outputs, "tests_skipped", 0), 0);
    check_equal(turbo_runtime_json_object_set_string(arguments, "source_revision", "other-revision"), 0);
    check_false(engineering_workflow_verified(result, arguments));
    check_equal(turbo_runtime_json_object_set_string(arguments, "source_revision", "rev-1"), 0);
    check_equal(turbo_runtime_json_object_set_string(arguments, "preset", "release"), 0);
    check_false(engineering_workflow_verified(result, arguments));
    check_equal(fixture.build_calls, 1);
    check_equal(fixture.test_calls, 1);
  }
}
