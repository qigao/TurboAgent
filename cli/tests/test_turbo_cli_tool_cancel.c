#include "tinytest.h"
#include "turbo_agent_policy.h"
#include "turbo_cli.h"
#include "turbo_coding_tools.h"
#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <salts/clock.h>
#include <stdlib.h>
#include <string.h>

enum { TOOL_CANCEL_WAIT_MS = 5000, TOOL_CANCEL_DEADLINE_MS = 1500 };

typedef struct tool_cancel_model_s {
  char *arguments;
  size_t calls;
  int saw_cancel, leaked_detail;
} tool_cancel_model_t;

static int tool_cancel_model(const char *request, char **out, void *user) {
  static const char call[] = "{\"id\":\"shell-response\",\"output\":[{\"type\":\"function_call\","
                             "\"name\":\"shell.exec\",\"call_id\":\"shell-call\"}]}";
  static const char answer[] =
      "{\"id\":\"resume-response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"resumed\"}]}]}";
  tool_cancel_model_t *model = user;
  json_value_t *response;
  *out = NULL;
  if (model->calls++ == 0) {
    response = json_parse(call, sizeof(call) - 1);
    if (!response) return -1;
    if (turbo_runtime_json_object_set_string(json_array_get(json_object_get(response, "output"), 0),
                                             "arguments", model->arguments) != 0) {
      json_free(response);
      return -1;
    }
  } else {
    model->saw_cancel = strstr(request, "tool_cancelled") != NULL ||
                        strstr(request, "tool_deadline_exceeded") != NULL;
    model->leaked_detail =
        strstr(request, "partial stdout") != NULL || strstr(request, "partial stderr") != NULL;
    response = json_parse(answer, sizeof(answer) - 1);
  }
  if (!response) return -1;
  *out = json_serialize(response, NULL);
  json_free(response);
  return *out ? 0 : -1;
}

static int tool_cancel_marker(const char *path, int *out_pid) {
  FILE *file = fopen(path, "r");
  int count = 0, pid;
  if (!file) return 0;
  while (fscanf(file, "%d", &pid) == 1) {
    if (fgetc(file) != '\n') break;
    *out_pid = pid;
    ++count;
  }
  fclose(file);
  return count;
}

static void tool_cancel_count_detail(const json_value_t *detail, void *user) {
  (void)detail;
  ++*(int *)user;
}

static void tool_cancel_wait(cmeta_threadpool_t *pool, turbo_agent_harness_execution_t *execution,
                             turbo_agent_execution_status_t expected) {
  turbo_agent_execution_status_t status;
  int run_rc = -1;
  check_equal(turbo_agent_harness_execution_wait(execution, TOOL_CANCEL_WAIT_MS), SALTS_OK);
  cmeta_threadpool_wait(pool);
  check_equal(turbo_agent_harness_execution_get_status(execution, &status), SALTS_OK);
  check_equal(status, expected);
  check_equal(turbo_agent_harness_execution_result_code(execution, &run_rc), SALTS_OK);
  check_equal(run_rc, 0);
}

spec("coding execution cancellation and resume") {
  static tstr root, marker, command;
  static turbo_tool_registry_t *registry;
  static cmeta_threadpool_t *pool;
  static turbo_agent_harness_t *harness;
  static turbo_agent_harness_execution_t *execution;
  static tool_cancel_model_t model;
  static json_value_t *arguments, *summary, *state, *journal;

  before_each() {
    char temp[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
    cmeta_uuid_t id;
    turbo_coding_workspace_config_t workspace;
    turbo_agent_session_config_t session = {0};
    turbo_agent_app_config_t app = {0};
    turbo_agent_harness_config_t config;
    turbo_agent_policy_t policy = {0};
    cmeta_threadpool_config_t workers = {.num_threads = 1, .queue_capacity = 1};
    model = (tool_cancel_model_t){0};
    check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
    check_equal(cmeta_uuid_v4_generate(&id), 0);
    check_equal(cmeta_uuid_format(&id, uuid, sizeof(uuid)), 0);
    root = tstr_format("{}/{}", temp, uuid);
    check_not_null(root);
    check_equal(cmeta_fs_mkdir(root, 0700), 0);
    marker = tstr_format("{}/started.txt", root);
    check_not_null(marker);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    turbo_coding_workspace_config_init(&workspace);
    workspace.files.workspace_root = root;
    check_equal(turbo_coding_tools_add_workspace(registry, &workspace), TURBO_TOOL_OK);
    pool = cmeta_threadpool_create_with_config(&workers);
    check_not_null(pool);
    session.agent_config.model = "test-model";
    session.agent_config.transport_fn = tool_cancel_model;
    session.agent_config.transport_user_data = &model;
    session.agent_config.tool_registry = registry;
    session.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app.session_config = &session;
    turbo_agent_harness_config_init(&config);
    config.app_config = &app;
    config.executor = pool;
    harness = turbo_agent_harness_create(&config);
    check_not_null(harness);
    policy.workspace_root = root;
    policy.allow_shell = 1;
    check_equal(turbo_agent_set_tool_policy(turbo_agent_session_agent(turbo_agent_app_session(
                                                turbo_agent_harness_app(harness))),
                                            &policy),
                0);
  }

  after_each() {
    cmeta_fs_stat_t metadata;
    if (execution) {
      turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_harness_execution_wait(execution, UINT64_MAX);
      turbo_agent_harness_execution_release(execution);
      execution = NULL;
    }
    if (pool) cmeta_threadpool_wait(pool);
    turbo_agent_harness_release(harness);
    harness = NULL;
    cmeta_threadpool_destroy(pool);
    pool = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    json_free(arguments);
    json_free(summary);
    json_free(state);
    json_free(journal);
    arguments = summary = state = journal = NULL;
    json_serialize_free(model.arguments);
    model.arguments = NULL;
    tstr_free(command);
    command = NULL;
    if (marker && cmeta_fs_stat(marker, &metadata) == 0) check_equal(cmeta_fs_unlink(marker), 0);
    if (root) check_equal(cmeta_fs_rmdir(root), 0);
    tstr_free(marker);
    tstr_free(root);
    marker = root = NULL;
  }

  it("does not launch or publish process detail when already expired") {
    turbo_tool_execution_context_t context = {0};
    int details = 0, pid = 0;
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.deadline_mono_ms = cmeta_monotonic_ms();
    context.detail_sink = tool_cancel_count_detail;
    context.detail_sink_user_data = &details;
    command = tstr_format("\"{}\" partial", CODING_PROCESS_FIXTURE);
    check_not_null(command);
    arguments = json_create_object();
    check_not_null(arguments);
    check_equal(turbo_runtime_json_object_set_string(arguments, "command", command), 0);
    check_equal(turbo_tool_registry_execute_json_value_with_context(registry, "shell.exec",
                                                                    arguments, &context, &state),
                TURBO_TOOL_DEADLINE_EXCEEDED);
    check_null(state);
    check_equal(details, 0);
    check_equal(tool_cancel_marker(marker, &pid), 0);
  }

  const struct {
    const char *name, *mode, *detail_status;
    turbo_cancel_reason_t reason;
    turbo_agent_execution_status_t execution_status;
    turbo_tool_status_t tool_status;
  } cases[] = {{"user", "partial", "cancelled", TURBO_CANCEL_USER, TURBO_AGENT_EXECUTION_CANCELLED,
                TURBO_TOOL_CANCELLED},
               {"shutdown", "partial", "cancelled", TURBO_CANCEL_SHUTDOWN,
                TURBO_AGENT_EXECUTION_CANCELLED, TURBO_TOOL_CANCELLED},
               {"deadline", "partial", "deadline_exceeded", TURBO_CANCEL_DEADLINE,
                TURBO_AGENT_EXECUTION_TIMED_OUT, TURBO_TOOL_DEADLINE_EXCEEDED},
               {"binary output", "partial_binary", "cancelled", TURBO_CANCEL_USER,
                TURBO_AGENT_EXECUTION_CANCELLED, TURBO_TOOL_CANCELLED}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    it("journals partial process detail for %s and resumes without replay", cases[i].name) {
      turbo_agent_harness_run_options_t options;
      turbo_agent_session_exec_options_t resume = {0};
      turbo_agent_runtime_t *runtime =
          turbo_agent_session_runtime(turbo_agent_app_session(turbo_agent_harness_app(harness)));
      const json_value_t *record, *detail;
      uint64_t deadline;
      int pid = 0, count = 0;
      bool alive = true;
      command = tstr_format("\"{}\" {}", CODING_PROCESS_FIXTURE, cases[i].mode);
      check_not_null(command);
      arguments = json_create_object();
      check_not_null(arguments);
      check_equal(turbo_runtime_json_object_set_string(arguments, "command", command), 0);
      model.arguments = json_serialize(arguments, NULL);
      check_not_null(model.arguments);
      turbo_agent_harness_run_options_init(&options);
      if (cases[i].reason == TURBO_CANCEL_DEADLINE)
        options.deadline_mono_ms = cmeta_monotonic_ms() + TOOL_CANCEL_DEADLINE_MS;
      check_equal(turbo_agent_harness_start_text(harness, "run command", &options, &execution),
                  SALTS_OK);
      deadline = cmeta_monotonic_ms() + TOOL_CANCEL_WAIT_MS;
      while (!(count = tool_cancel_marker(marker, &pid)) && cmeta_monotonic_ms() < deadline)
        cmeta_sleep_ms(5);
      check_equal(count, 1);
      check_greater(pid, 0);
      if (cases[i].reason != TURBO_CANCEL_DEADLINE)
        check_equal(turbo_agent_harness_execution_cancel(execution, cases[i].reason), SALTS_OK);
      tool_cancel_wait(pool, execution, cases[i].execution_status);
      check_equal(cmeta_process_is_pid_alive(pid, &alive), SALTS_OK);
      check_false(alive);
      check_equal(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
      check_not_null(json_get_string(summary, "checkpoint_id"));
      check_null(turbo_agent_state_final_answer_text(state));
      check_equal(turbo_agent_state_event_count(state), 2);
      check_equal(model.calls, 1);
      check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id",
                                                      "shell-call", &journal),
                  0);
      check_equal(json_array_size(journal), 1);
      record = json_array_get(journal, 0);
      check_equal(json_get_string(record, "phase"), "committed");
      check_equal(json_get_int(record, "status", 0), cases[i].tool_status);
      check_null(json_object_get(record, "output"));
      detail = json_object_get(record, "detail");
      check_not_null(detail);
      check_false(json_get_bool(detail, "ok", true));
      check_equal(json_get_string(detail, "status"), cases[i].detail_status);
      check_not_null(json_get_string(detail, "process_state"));
      check_equal(json_get_string(detail, "stdout"), i == 3 ? "" : "partial stdout");
      check_equal(json_get_string(detail, "stderr"), i == 3 ? "" : "partial stderr");
      check_equal(json_get_int(json_object_get(detail, "metrics"), "output_bytes", 0),
                  i == 3 ? 15 : 28);
      check_false(json_get_bool(detail, "retryable", true));
      turbo_agent_harness_execution_release(execution);
      execution = NULL;
      json_free(summary);
      json_free(state);
      summary = state = NULL;
      turbo_agent_harness_run_options_init(&options);
      options.session_options = &resume;
      check_equal(turbo_agent_harness_resume(harness, NULL, &options, &execution), SALTS_OK);
      tool_cancel_wait(pool, execution, TURBO_AGENT_EXECUTION_COMPLETED);
      check_equal(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
      check_equal(turbo_agent_state_final_answer_text(state), "resumed");
      check_equal(model.calls, 2);
      check_true(model.saw_cancel);
      check_false(model.leaked_detail);
      check_equal(tool_cancel_marker(marker, &pid), 1);
    }
  }
}
