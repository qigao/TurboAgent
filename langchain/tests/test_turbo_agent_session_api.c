#include "tinytest.h"
#include "turbo_agent_test_support.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_workflow.h"
#include "turbo_model_provider.h"
#include "turbo_prompt.h"
#include <json_parser.h>
#include "turbo_retriever.h"
#include "turbo_runnable.h"
#include "turbo_tool_registry.h"
#include "../src/turbo_agent_runtime_internal.h"

#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define session_find_command_descriptor turbo_agent_test_find_command_descriptor
#define session_check_string_array_contains_all turbo_agent_test_check_string_array_contains_all
#define session_check_thread_timeline_json_value turbo_agent_test_check_thread_timeline_json_value
#define session_check_lineage_string_field turbo_agent_test_check_lineage_string_field
#define session_find_lineage_branch turbo_agent_test_find_lineage_branch
#define session_check_lineage_branch turbo_agent_test_check_lineage_branch
#define session_check_thread_lineage_json_value turbo_agent_test_check_thread_lineage_json_value
#define session_check_checkpoint_context turbo_agent_test_check_checkpoint_context
#define session_check_command_descriptor_example turbo_agent_test_check_command_descriptor_example
#define session_check_command_descriptor_fixture turbo_agent_test_check_command_descriptor_fixture
#define session_check_memory_record_fixture turbo_agent_test_check_memory_record_fixture
#define session_check_memory_record_array_fixture turbo_agent_test_check_memory_record_array_fixture
#define session_find_branch_tree_branch turbo_agent_test_find_branch_tree_branch
#define session_find_branch_tree_edge turbo_agent_test_find_branch_tree_edge
#define session_check_branch_tree_branch turbo_agent_test_check_branch_tree_branch
#define session_check_branch_tree_edge turbo_agent_test_check_branch_tree_edge
#define session_check_branch_tree turbo_agent_test_check_branch_tree
#define session_replay_capture_t turbo_agent_test_replay_capture_t
#define session_capture_replayed_history_event turbo_agent_test_capture_replayed_history_event
#define session_observer_capture_t turbo_agent_test_observer_capture_t
#define session_capture_observer_event turbo_agent_test_capture_observer_event
#define session_trace_capture_t turbo_agent_test_trace_capture_t
#define session_capture_trace_event turbo_agent_test_capture_trace_event

CXX_C_API int turbo_agent_session_apply_state_patch_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_get_thread_head_state_json_value(
    turbo_agent_session_t *session, json_value_t **out_state);
CXX_C_API int turbo_agent_session_get_thread_head_trace_events_json_value(
    turbo_agent_session_t *session, json_value_t **out_events);
CXX_C_API int turbo_agent_session_prepare_checkpoint_state_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *state_patch,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_prepare_thread_state_override_json_value(
    turbo_agent_session_t *session, const json_value_t *state_patch,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_prepare_checkpoint_command_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id,
    const json_value_t *command,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_prepare_thread_command_override_json_value(
    turbo_agent_session_t *session, const json_value_t *command,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_apply_thread_state_patch_json_value(
    turbo_agent_session_t *session, const json_value_t *state_patch,
    json_value_t **out_state_override);
CXX_C_API int turbo_agent_session_resume_thread_state_patch_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state);

static int turbo_agent_session_start_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_start_graph(session, graph, state, options, NULL, NULL,
                                         out_summary_json, out_state);
}

static int turbo_agent_session_start_json_value_graph_stream(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_start_graph(session, graph, state, options, event_sink,
                                         event_sink_user_data, out_summary_json, out_state);
}

static int turbo_agent_session_resume_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_resume_json_value_graph_stream(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      event_sink, event_sink_user_data, out_summary_json, out_state);
}

static int turbo_agent_session_fork_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_fork_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_fork_json_value_graph_stream(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_fork_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      event_sink, event_sink_user_data, out_summary_json, out_state);
}

static int turbo_agent_session_fork_thread_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_fork_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_resume_checkpoint_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const char *checkpoint_id,
    const json_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_graph(
      session, graph, state_override, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_resume_thread_state_json_value_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_graph(
      session, graph, state_patch, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_PATCH,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_update_thread_state_json_value(
    turbo_agent_session_t *session, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  return turbo_agent_session_apply_thread_state_patch_json_value(session, state_patch,
                                                           out_state_override);
}

static int turbo_agent_session_resume_thread_command_json_value(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_graph(
      session, graph, command, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_COMMAND,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_start_preset_json_value_graph(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_start_preset(session, kind, state, options, NULL, NULL,
                                          out_summary_json, out_state);
}

static int turbo_agent_session_start_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *state = turbo_agent_session_create_input_state_json_value(user_text);
  int rc;

  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset(session, kind, state, options, NULL, NULL,
                                        out_summary_json, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

static int turbo_agent_session_resume_thread_preset_command_json_value(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, json_value_t **out_state) {
  return turbo_agent_session_resume_preset(
      session, kind, command, options,
      &(turbo_agent_session_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_COMMAND,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int session_invoke_preset_text_impl(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  json_value_t *result_state = NULL;
  char *text;
  int rc;

  if (!out_text) {
    return -1;
  }
  *out_text = NULL;
  rc = turbo_agent_session_start_preset(session, kind, state, options, NULL, NULL,
                                        out_summary_json, &result_state);
  if (rc != 0) {
    turbo_runtime_json_destroy(result_state);
    return rc;
  }
  text = turbo_agent_session_result_text(result_state);
  turbo_runtime_json_destroy(result_state);
  if (!text) {
    return -1;
  }
  *out_text = text;
  return 0;
}

static int session_invoke_preset_json_impl(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_json, json_value_t **out_summary_json) {
  char *text = NULL;
  json_value_t *parsed = NULL;
  int rc;

  if (!out_json) {
    return -1;
  }
  *out_json = NULL;
  rc = session_invoke_preset_text_impl(session, kind, state, options, &text, out_summary_json);
  if (rc != 0) {
    return rc;
  }
  rc = turbo_parse_json((const uint8_t *)text, strlen(text), &parsed);
  free(text);
  if (rc != 0 || !parsed) {
    turbo_free_json(&parsed);
    return -1;
  }
  *out_json = parsed;
  return 0;
}

static int turbo_agent_session_invoke_preset_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json) {
  json_value_t *state = turbo_agent_session_create_input_state_json_value(user_text);
  int rc;

  if (!state) {
    return -1;
  }
  rc = session_invoke_preset_text_impl(session, kind, state, options, out_text,
                                       out_summary_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

static int turbo_agent_session_invoke_preset_json(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  json_value_t *state = turbo_agent_session_create_input_state_json_value(user_text);
  int rc;

  if (!state) {
    return -1;
  }
  rc = session_invoke_preset_json_impl(session, kind, state, options, out_json,
                                       out_summary_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

static int turbo_agent_session_invoke_preset_messages_text(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *messages, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  json_value_t *state =
      turbo_agent_session_create_input_messages_state_json_value(messages);
  int rc;

  if (!state) {
    return -1;
  }
  rc = session_invoke_preset_text_impl(session, kind, state, options, out_text,
                                       out_summary_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

static int turbo_agent_session_invoke_preset_text_with_memory(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const char *user_text, const char *memory_namespace, const turbo_graph_run_options_t *options,
    char **out_text, json_value_t **out_summary_json) {
  json_value_t *state =
      turbo_agent_session_create_input_state_with_memory_json_value(session, user_text, memory_namespace);
  int rc;

  if (!state) {
    return -1;
  }
  rc = session_invoke_preset_text_impl(session, kind, state, options, out_text,
                                       out_summary_json);
  turbo_runtime_json_destroy(state);
  return rc;
}

typedef struct {
  const char *key;
  int value;
} session_bool_write_t;

typedef struct {
  const char *name;
  const char *theme;
  const char *locale;
} session_patch_state_write_t;

static char *session_query_only_memory_strdup(const char *text) {
  size_t length;
  char *copy;

  if (!text) {
    return NULL;
  }
  length = strlen(text);
  copy = (char *)malloc(length + 1);
  check_not_null(copy);
  memcpy(copy, text, length + 1);
  return copy;
}

static int session_query_only_memory_query(void *user_data, const char *namespace_prefix,
                                           const char *kind, const char *key_prefix,
                                           const char *text_substring,
                                           char **out_records_json) {
  (void)user_data;
  if (!out_records_json) {
    return -1;
  }
  if (namespace_prefix && strcmp(namespace_prefix, "project") != 0 &&
      strcmp(namespace_prefix, "project/") != 0) {
    return -1;
  }
  if (kind && strcmp(kind, "context") != 0) {
    *out_records_json = session_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (key_prefix && strcmp(key_prefix, "con") != 0) {
    *out_records_json = session_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (text_substring && strcmp(text_substring, "remember") != 0) {
    *out_records_json = session_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  *out_records_json =
      session_query_only_memory_strdup("[{\"id\":\"project/demo::context\","
                                       "\"namespace\":\"project/demo\","
                                       "\"kind\":\"context\",\"key\":\"context\","
                                       "\"text\":\"remember this\","
                                       "\"metadata\":{\"scope\":\"project\","
                                       "\"path\":\"/tmp/notes.md\"},"
                                       "\"created_at\":null}]");
  return *out_records_json ? 0 : -1;
}

static turbo_agent_memory_store_t session_query_only_memory_store_create(void) {
  turbo_agent_memory_store_t store = {0};
  store.query = session_query_only_memory_query;
  return store;
}

static int session_success_transport(const char *request_json, char **out_response_json,
                                     void *user_data) {
  const char *response =
      "{\"id\":\"resp_test\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  size_t len;
  char *copy;

  (void)request_json;
  (void)user_data;
  check_not_null(out_response_json);
  len = strlen(response) + 1;
  copy = (char *)malloc(len);
  check_not_null(copy);
  memcpy(copy, response, len);
  *out_response_json = copy;
  return 0;
}

static int session_tool_echo_handler(const char *arguments_json, char **out_output,
                                     void *user_data) {
  const char *response = "{\"ok\":true}";
  char *copy;

  (void)arguments_json;
  (void)user_data;
  if (!out_output) {
    return -1;
  }
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_output = copy;
  return 0;
}

static int session_json_transport(const char *request_json, char **out_response_json,
                                  void *user_data) {
  const char *response =
      "{\"id\":\"resp_json\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"ok\\\":true,\\\"value\\\":42}\"}]}]}";
  size_t len;
  char *copy;

  (void)request_json;
  (void)user_data;
  check_not_null(out_response_json);
  len = strlen(response) + 1;
  copy = (char *)malloc(len);
  check_not_null(copy);
  memcpy(copy, response, len);
  *out_response_json = copy;
  return 0;
}

static int session_review_transport(const char *request_json, char **out_response_json,
                                    void *user_data) {
  const char *planner_response =
      "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"say ok\\\"]}\"}]}]}";
  const char *executor_response =
      "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  const char *response;
  size_t len;
  char *copy;

  (void)user_data;
  check_not_null(out_response_json);
  response = (request_json && strstr(request_json, "Execute plan step") != NULL) ? executor_response
                                                                                  : planner_response;
  len = strlen(response) + 1;
  copy = (char *)malloc(len);
  check_not_null(copy);
  memcpy(copy, response, len);
  *out_response_json = copy;
  return 0;
}

static int session_retriever_query(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  (void)user_data;
  (void)query;
  (void)options;
  check_not_null(out_results_json);
  *out_results_json = turbo_json_create_array();
  return *out_results_json ? 0 : -1;
}

static json_value_t *session_create_messages_json_value(void) {
  json_value_t *messages = turbo_prompt_messages_create_json_value();

  check_not_null(messages);
  check_int_eq(turbo_prompt_messages_append_json_value(messages, "system", "Be terse."),
               TURBO_PROMPT_OK);
  check_int_eq(turbo_prompt_messages_append_json_value(messages, "user", "hello"), TURBO_PROMPT_OK);
  return messages;
}

static char *session_test_strdup(const char *text) {
  size_t len;
  char *copy;

  check_not_null(text);
  len = strlen(text);
  copy = (char *)malloc(len + 1);
  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static int session_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  session_bool_write_t *write = (session_bool_write_t *)user_data;
  json_value_t *value =
      turbo_json_create_bool(write->value);

  check_not_null(value);
  return turbo_runtime_json_object_set(ctx->json_value_state, write->key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static int session_supervisor_planner_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *reason = (const char *)user_data;

  if (!ctx || !ctx->state || !reason) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_planner", true);
  return turbo_agent_state_request_handoff(ctx->state, "executor", reason);
}

static int session_supervisor_executor_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *final_output = (const char *)user_data;

  if (!ctx || !ctx->state || !final_output) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_executor", true);
  return turbo_agent_state_set_final_answer(ctx->state, final_output);
}

static turbo_graph_t *create_session_review_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("session-review");
  static session_bool_write_t start = {"visited_start", 1};
  static session_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(turbo_graph_add_json_value_node(graph, "start", session_write_bool_json_value_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_node(graph, "end", session_write_bool_json_value_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_graph_t *create_session_supervisor_handoff_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("session-supervisor-handoff");
  static const char *handoff_reason = "delegate execution";
  static const char *final_output = "executor finished";

  check_not_null(graph);
  check_int_eq(turbo_agent_install_supervisor_loop(graph, "supervisor", "handoff", "end", 1),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "planner", session_supervisor_planner_node,
                                    (void *)handoff_reason),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "executor", session_supervisor_executor_node,
                                    (void *)final_output),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "planner", "handoff", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "executor", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_session_review_state(int approved) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_request_review(state, "need approval"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static json_value_t *create_session_supervisor_handoff_state(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static json_value_t *create_session_supervisor_review_state(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
  check_int_eq(turbo_agent_state_request_review(state, "need approval"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static json_value_t *
create_session_override(const json_value_t *result_state, int approved) {
  json_value_t *state = turbo_json_clone(result_state);
  json_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static int session_patch_state_write_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  session_patch_state_write_t *write = (session_patch_state_write_t *)user_data;
  json_value_t *profile = turbo_json_create_object();
  json_value_t *settings = turbo_json_create_object();
  json_value_t *labels = turbo_json_create_array();

  (void)write;
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(labels);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->json_value_state, "visited_start",
                   turbo_json_create_bool(1)),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   profile, "name", turbo_json_create_string("alpha")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   settings, "theme", turbo_json_create_string("light")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   settings, "locale", turbo_json_create_string("en")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   settings, "notes", turbo_json_create_string("keep")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(profile, "settings", settings),
               TURBO_RUNTIME_JSON_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_json_array_append(
                   labels, turbo_json_create_string("seed")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(profile, "labels", labels),
               TURBO_RUNTIME_JSON_OK);
  labels = NULL;
  return turbo_runtime_json_object_set(ctx->json_value_state, "profile", profile) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static turbo_graph_t *create_session_state_patch_graph(void) {
  static session_patch_state_write_t start = {"alpha", "light", "en"};
  static session_bool_write_t end = {"visited_end", 1};
  turbo_graph_t *graph = turbo_graph_create("session-state-patch");

  check_not_null(graph);
  check_int_eq(turbo_graph_add_json_value_node(graph, "start", session_patch_state_write_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_node(graph, "end", session_write_bool_json_value_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_session_state_patch_json_value(void) {
  json_value_t *patch = turbo_json_create_object();
  json_value_t *profile = turbo_json_create_object();
  json_value_t *settings = turbo_json_create_object();
  json_value_t *flags = turbo_json_create_array();

  check_not_null(patch);
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(flags);
  check_int_eq(turbo_runtime_json_object_set(
                   settings, "theme", turbo_json_create_string("dark")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   settings, "extra", turbo_json_create_string("enabled")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(settings, "notes",
                                                  turbo_json_create_null()),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_array_append(
                   flags, turbo_json_create_string("flag-a")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_array_append(
                   flags, turbo_json_create_string("flag-b")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(profile, "settings", settings),
               TURBO_RUNTIME_JSON_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_json_object_set(profile, "flags", flags),
               TURBO_RUNTIME_JSON_OK);
  flags = NULL;
  check_int_eq(turbo_runtime_json_object_set(patch, "profile", profile),
               TURBO_RUNTIME_JSON_OK);
  profile = NULL;
  check_int_eq(turbo_runtime_json_object_set(
                   patch, "patch_version", turbo_json_create_int64(3)),
               TURBO_RUNTIME_JSON_OK);
  return patch;
}

static char *session_temp_root(void) {
  int needed;
  char *path;
  unsigned long long tick = (unsigned long long)turbo_hrtime();

#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  needed = snprintf(NULL, 0, "%sturbonet_session_%llx", temp_dir, tick);
#else
  const char *temp_dir = "/tmp/";
  needed = snprintf(NULL, 0, "%sturbonet_session_%llx", temp_dir, tick);
#endif
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
#ifdef _WIN32
  snprintf(path, (size_t)needed + 1, "%sturbonet_session_%llx", temp_dir, tick);
  _mkdir(path);
#else
  snprintf(path, (size_t)needed + 1, "%sturbonet_session_%llx", temp_dir, tick);
  mkdir(path, 0777);
#endif
  return path;
}

static char *session_temp_path(const char *root_dir, const char *name) {
  int needed;
  char *path;
#ifdef _WIN32
  const char *sep = "\\";
#else
  const char *sep = "/";
#endif

  needed = snprintf(NULL, 0, "%s%s%s", root_dir, sep, name);
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
  snprintf(path, (size_t)needed + 1, "%s%s%s", root_dir, sep, name);
  return path;
}

spec("turbo agent session api") {

  it("should create an agent session from dotenv-backed config") {
    char *root_dir = session_temp_root();
    char *env_path;
    FILE *fp;
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;

    check_not_null(root_dir);
    env_path = session_temp_path(root_dir, ".env");
    check_not_null(env_path);
    fp = fopen(env_path, "wb");
    check_not_null(fp);
    fputs("OPENAI_API_KEY=test-key\n", fp);
    fputs("OPENAI_BASE_URL=https://example.test/v1\n", fp);
    fputs("OPENAI_MODEL=gpt-test\n", fp);
    fputs("OPENAI_PROVIDER=openai_responses\n", fp);
    fclose(fp);

    config.agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.env_path = env_path;
    config.load_env = 1;
    config.overwrite_env = 1;
    session = turbo_agent_session_create(&config);

    check_not_null(session);
    check_not_null(turbo_agent_session_agent(session));
    check_int_eq(turbo_agent_session_has_api_key(session), 1);
    check_str_eq(turbo_agent_session_model(session), "gpt-test");
    check_str_eq(turbo_agent_session_base_url(session), "https://example.test/v1");
    check_str_eq(turbo_agent_session_provider_name(session), "openai_responses");

    free(env_path);
    free(root_dir);
    turbo_agent_session_destroy(session);
  }

  it("should build preset workflow graphs from an agent-backed session") {
    char *root_dir = session_temp_root();
    char *env_path;
    FILE *fp;
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *loop_graph;
    turbo_graph_t *review_graph;
    turbo_graph_t *engineering_graph;

    check_not_null(root_dir);
    env_path = session_temp_path(root_dir, ".env");
    check_not_null(env_path);
    fp = fopen(env_path, "wb");
    check_not_null(fp);
    fputs("OPENAI_API_KEY=test-key\n", fp);
    fputs("OPENAI_BASE_URL=https://example.test/v1\n", fp);
    fputs("OPENAI_MODEL=gpt-test\n", fp);
    fputs("OPENAI_PROVIDER=openai_responses\n", fp);
    fclose(fp);

    config.agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.env_path = env_path;
    config.load_env = 1;
    config.overwrite_env = 1;
    session = turbo_agent_session_create(&config);

    check_not_null(session);
    loop_graph = turbo_agent_session_create_loop_graph(session);
    review_graph = turbo_agent_session_create_review_graph(session);
    engineering_graph = turbo_agent_session_create_engineering_graph(session);
    check_not_null(loop_graph);
    check_not_null(review_graph);
    check_not_null(engineering_graph);

    turbo_graph_destroy(engineering_graph);
    turbo_graph_destroy(review_graph);
    turbo_graph_destroy(loop_graph);
    turbo_agent_session_destroy(session);
    free(env_path);
    free(root_dir);
  }

  it("should start one preset loop workflow from an agent-backed session") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *json_state = turbo_agent_state_create();
    json_value_t *state;
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;

    check_not_null(json_state);
    state = turbo_json_clone(json_state);
    turbo_free_json(&json_state);
    check_not_null(state);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);

    check_not_null(session);
    check_int_eq(turbo_agent_session_start_preset_json_value_graph(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, state, NULL, &summary,
                     &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));

    turbo_runtime_json_destroy(result_state);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }

  it("should create input state and start one preset loop directly from user text") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *seed_state;
    json_value_t *seed_json;
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;

    seed_state = turbo_agent_session_create_input_state_json_value("hello");
    check_not_null(seed_state);
    seed_json = turbo_json_clone(seed_state);
    check_not_null(seed_json);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(seed_json, "input")), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(seed_json, "input"), 0),
                 "role"),
                 "user");
    turbo_free_json(&seed_json);
    turbo_runtime_json_destroy(seed_state);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_start_preset_text(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, "hello", NULL, &summary,
                     &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_not_null(turbo_agent_session_last_run_id(session));

    turbo_runtime_json_destroy(result_state);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should resume one preset review workflow through command helpers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *result_state = NULL;
    json_value_t *resumed_state = NULL;
    json_value_t *command = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_review_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_preset_text(
                     session, TURBO_AGENT_SESSION_WORKFLOW_REVIEW, "hello", &options, &summary,
                     &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_str_eq(turbo_json_get_string(summary, "interrupt_reason"), "review_required");
    check_size_eq(
        turbo_json_array_size(turbo_json_object_get(summary, "available_command_descriptors")), 6);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(summary, "available_command_descriptors"), 0),
                 "name"),
                 "approve_review");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(summary, "available_command_descriptors"), 0),
                 "category"),
                 "review");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(summary, "available_command_descriptors"), 0),
                 "input_mode"),
                 "none");
    check_false(turbo_json_get_bool(turbo_json_array_get(
                    turbo_json_object_get(summary, "available_command_descriptors"), 0),
                "requires_input", true));
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(summary, "available_command_descriptors"), 0),
                 "resume_mode"),
                 "resume_or_fork");
    {
      const json_value_t *request_replan_descriptor =
          session_find_command_descriptor(summary, "request_replan");
      const json_value_t *append_feedback_descriptor =
          session_find_command_descriptor(summary, "append_feedback");
      const json_value_t *override_descriptor =
          session_find_command_descriptor(summary, "override_final_output");
      const json_value_t *request_replan_accepted_keys;
      const json_value_t *append_feedback_accepted_keys;
      const json_value_t *override_accepted_keys;
      const char *const request_replan_expected_accepts[] = {"reason"};
      const char *const append_feedback_expected_accepts[] = {"text"};
      const char *const override_expected_accepts[] = {"text", "output_json"};

      check_not_null(request_replan_descriptor);
      check_not_null(append_feedback_descriptor);
      check_not_null(override_descriptor);
      check_str_eq(turbo_json_get_string(request_replan_descriptor, "primary_key"), "reason");
      check_str_eq(turbo_json_get_string(request_replan_descriptor, "placeholder"),
                   "Describe why the current plan should be rebuilt.");
      check_str_eq(
          turbo_json_get_string(request_replan_descriptor, "success_state_hint"),
          "Marks the state for replanning before the next execution pass.");
      request_replan_accepted_keys =
          turbo_json_object_get(request_replan_descriptor, "accepted_keys");
      check_not_null(request_replan_accepted_keys);
      session_check_string_array_contains_all(request_replan_accepted_keys,
                                              request_replan_expected_accepts,
                                              sizeof(request_replan_expected_accepts) /
                                                  sizeof(request_replan_expected_accepts[0]));
      check_false(turbo_json_get_bool(request_replan_descriptor, "supports_json_value", true));
      session_check_command_descriptor_fixture(request_replan_descriptor,
                                               "command_descriptors.golden.json",
                                               "request_replan");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "primary_key"), "text");
      append_feedback_accepted_keys =
          turbo_json_object_get(append_feedback_descriptor, "accepted_keys");
      check_not_null(append_feedback_accepted_keys);
      session_check_string_array_contains_all(append_feedback_accepted_keys,
                                              append_feedback_expected_accepts,
                                              sizeof(append_feedback_expected_accepts) /
                                                  sizeof(append_feedback_expected_accepts[0]));
      check_false(turbo_json_get_bool(append_feedback_descriptor, "supports_json_value", true));
      check_true(turbo_json_type(
                     turbo_json_object_get(append_feedback_descriptor, "example_payload")) ==
                 TURBO_JSON_OBJECT);
      check_str_eq(turbo_json_get_string(override_descriptor, "primary_key"), "text");
      override_accepted_keys = turbo_json_object_get(override_descriptor, "accepted_keys");
      check_not_null(override_accepted_keys);
      session_check_string_array_contains_all(override_accepted_keys, override_expected_accepts,
                                              sizeof(override_expected_accepts) /
                                                  sizeof(override_expected_accepts[0]));
      check_true(turbo_json_get_bool(override_descriptor, "supports_json_value", false));
      session_check_command_descriptor_fixture(append_feedback_descriptor,
                                               "command_descriptors.golden.json",
                                               "append_feedback");
      session_check_command_descriptor_fixture(override_descriptor,
                                               "command_descriptors.golden.json",
                                               "override_final_output");
    }
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("approve_review")),
                 TURBO_RUNTIME_JSON_OK);
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_session_resume_thread_preset_command_json_value(
                     session, TURBO_AGENT_SESSION_WORKFLOW_REVIEW, command, &options, &summary2,
                     &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");

    turbo_runtime_json_destroy(command);
    turbo_runtime_json_destroy(resumed_state);
    turbo_runtime_json_destroy(result_state);
    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should invoke one preset loop from user text and return final answer text") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_invoke_preset_text(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, "hello", NULL, &text, &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should invoke one preset loop from user text and return parsed final json") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *summary = NULL;
    json_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_json_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_invoke_preset_json(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, "hello", NULL, &result,
                     &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);

    turbo_free_json(&result);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should create input state and invoke one preset loop from canonical messages") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    json_value_t *seed_state;
    json_value_t *seed_json;
    json_value_t *summary = NULL;
    char *text = NULL;

    seed_state = turbo_agent_session_create_input_messages_state_json_value(messages);
    check_not_null(seed_state);
    seed_json = turbo_json_clone(seed_state);
    check_not_null(seed_json);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(seed_json, "input")), 2);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(
                     turbo_json_object_get(seed_json, "input"), 0),
                 "role"),
                 "system");
    turbo_free_json(&seed_json);
    turbo_runtime_json_destroy(seed_state);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_invoke_preset_messages_text(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, messages, NULL, &text, &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should reject invalid canonical messages when building preset input state") {
    json_value_t *messages = turbo_json_create_array();
    json_value_t *bad = turbo_json_create_object();
    json_value_t *state;

    check_not_null(messages);
    check_not_null(bad);
    check_int_eq(turbo_runtime_json_array_append(messages, bad), TURBO_RUNTIME_JSON_OK);

    state = turbo_agent_session_create_input_messages_state_json_value(messages);
    check_null(state);

    turbo_runtime_json_destroy(messages);
  }

  it("should load session long-term memory records into memory context state") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    json_value_t *state;
    json_value_t *json_state;
    char *memory_text;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "/tmp/notes.md", "remember"),
                 0);

    state = turbo_agent_session_create_input_state_with_memory_json_value(session, "hello", "project");
    check_not_null(state);
    json_state = turbo_json_clone(state);
    check_not_null(json_state);
    check_int_eq(turbo_agent_state_memory_layer_count(json_state), 1);
    memory_text = turbo_agent_state_memory_context_text(json_state);
    check_not_null(memory_text);
    check_str_eq(memory_text, "Persistent memory:\n[project] /tmp/notes.md\nremember\n\n");

    free(memory_text);
    turbo_free_json(&json_state);
    turbo_runtime_json_destroy(state);
    turbo_agent_session_destroy(session);
  }

  it("should invoke one preset loop from user text plus session memory context") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    config.agent_config = agent_config;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "/tmp/notes.md", "remember"),
                 0);
    check_int_eq(turbo_agent_session_invoke_preset_text_with_memory(
                     session, TURBO_AGENT_SESSION_WORKFLOW_LOOP, "hello", "project", NULL, &text,
                     &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should invoke through session defaults without restating workflow kind or memory namespace") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    config.memory_namespace = "project";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_int_eq(turbo_agent_session_workflow_kind(session), TURBO_AGENT_SESSION_WORKFLOW_LOOP);
    check_str_eq(turbo_agent_session_memory_namespace(session), "project");

    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "/tmp/notes.md", "remember"),
                 0);
    check_int_eq(turbo_agent_session_invoke_text(session, "hello", NULL, &text, &summary), 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should expose the configured tool registry through session accessors") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};

    check_not_null(registry);
    definition.name = "session.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.handler = session_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    agent_config.tool_registry = registry;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_ptr_eq(turbo_agent_tool_registry(turbo_agent_session_agent(session)), registry);
    check_size_eq(turbo_agent_tool_count(turbo_agent_session_agent(session)), 1);
    check_ptr_eq(turbo_agent_session_tool_registry(session), registry);
    check_size_eq(turbo_agent_session_tool_count(session), 1);

    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

  it("should report configured harness capabilities through session diagnostics") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    json_value_t *capabilities = NULL;

    check_not_null(registry);
    definition.name = "session.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.handler = session_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.api_key = "test-key";
    agent_config.base_url = "https://example.invalid/v1";
    agent_config.transport_fn = session_success_transport;
    agent_config.tool_registry = registry;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    config.memory_namespace = "project";
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_capabilities(session, &capabilities), 0);
    check_not_null(capabilities);
    check_true(turbo_json_get_bool(capabilities, "has_agent", false));
    check_true(turbo_json_get_bool(capabilities, "has_runtime", false));
    check_true(turbo_json_get_bool(capabilities, "has_memory_store", false));
    check_true(turbo_json_get_bool(capabilities, "has_tool_registry", false));
    check_int_eq(turbo_json_get_int(capabilities, "tool_count", -1), 1);
    check_int_eq(turbo_json_get_int(capabilities, "workflow_kind_id", -1),
                 TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING);
    check_str_eq(turbo_json_get_string(capabilities, "workflow_kind"), "engineering");
    check_str_eq(turbo_json_get_string(capabilities, "memory_namespace"), "project");
    check_str_eq(turbo_json_get_string(capabilities, "model"), "gpt-5.4");
    check_str_eq(turbo_json_get_string(capabilities, "base_url"),
                 "https://example.invalid/v1");
    check_str_eq(turbo_json_get_string(capabilities, "provider"), "openai_responses");
    check_true(turbo_json_get_bool(capabilities, "has_api_key", false));
    check_false(turbo_json_get_bool(capabilities, "has_knowledge_store", true));
    check_false(turbo_json_get_bool(capabilities, "has_retriever", true));

    turbo_free_json(&capabilities);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

  it("should report runtime-only harness capabilities without agent tools") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    json_value_t *capabilities = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_REVIEW;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_capabilities(session, &capabilities), 0);
    check_not_null(capabilities);
    check_false(turbo_json_get_bool(capabilities, "has_agent", true));
    check_true(turbo_json_get_bool(capabilities, "has_runtime", false));
    check_false(turbo_json_get_bool(capabilities, "has_memory_store", true));
    check_false(turbo_json_get_bool(capabilities, "has_tool_registry", true));
    check_int_eq(turbo_json_get_int(capabilities, "tool_count", -1), 0);
    check_str_eq(turbo_json_get_string(capabilities, "workflow_kind"), "review");
    check_true(turbo_json_is_null(turbo_json_object_get(capabilities, "model")));
    check_true(turbo_json_is_null(turbo_json_object_get(capabilities, "memory_namespace")));

    turbo_free_json(&capabilities);
    turbo_agent_session_destroy(session);
  }

  it("should report provider tool schemas through session diagnostics") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    json_value_t *schemas = NULL;
    json_value_t *registry_tools;
    json_value_t *openai_responses;
    json_value_t *openai_chat;
    json_value_t *openai_compatible_chat;
    json_value_t *anthropic;
    json_value_t *function_object;

    check_not_null(registry);
    definition.name = "session.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.strict = 1;
    definition.handler = session_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    agent_config.tool_registry = registry;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    check_not_null(schemas);
    check_true(turbo_json_get_bool(schemas, "has_tool_registry", false));
    check_int_eq(turbo_json_get_int(schemas, "tool_count", -1), 1);

    registry_tools = turbo_json_object_get(schemas, "registry");
    openai_responses = turbo_json_object_get(schemas, "openai_responses");
    openai_chat = turbo_json_object_get(schemas, "openai_chat");
    openai_compatible_chat = turbo_json_object_get(schemas, "openai_compatible_chat");
    anthropic = turbo_json_object_get(schemas, "anthropic");
    check_size_eq(turbo_json_array_size(registry_tools), 1);
    check_size_eq(turbo_json_array_size(openai_responses), 1);
    check_size_eq(turbo_json_array_size(openai_chat), 1);
    check_size_eq(turbo_json_array_size(openai_compatible_chat), 1);
    check_size_eq(turbo_json_array_size(anthropic), 1);

    check_str_eq(turbo_json_get_string(turbo_json_array_get(registry_tools, 0), "name"),
                 "session.echo");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(openai_responses, 0), "name"),
                 "session.echo");
    function_object =
        turbo_json_object_get(turbo_json_array_get(openai_chat, 0), "function");
    check_str_eq(turbo_json_get_string(function_object, "name"), "session.echo");
    check_true(turbo_json_get_bool(function_object, "strict", false));
    function_object = turbo_json_object_get(turbo_json_array_get(openai_compatible_chat, 0),
                                            "function");
    check_str_eq(turbo_json_get_string(function_object, "name"), "session_echo");
    check_false(turbo_json_get_bool(function_object, "strict", false));
    check_str_eq(turbo_json_get_string(turbo_json_array_get(anthropic, 0), "name"),
                 "session.echo");

    turbo_free_json(&schemas);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

  it("should report empty tool schema arrays for runtime-only sessions") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    json_value_t *schemas = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    check_not_null(schemas);
    check_false(turbo_json_get_bool(schemas, "has_tool_registry", true));
    check_int_eq(turbo_json_get_int(schemas, "tool_count", -1), 0);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "registry")), 0);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "openai_responses")),
                  0);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "openai_chat")), 0);
    check_size_eq(turbo_json_array_size(
                      turbo_json_object_get(schemas, "openai_compatible_chat")),
                  0);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "anthropic")), 0);

    turbo_free_json(&schemas);
    turbo_agent_session_destroy(session);
  }

  it("should report startup diagnostics for invalid tool schemas") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t dotted = {0};
    turbo_tool_definition_t slashed = {0};
    json_value_t *diagnostics = NULL;
    json_value_t *errors;
    json_value_t *capabilities;

    check_not_null(registry);
    dotted.name = "codex.files";
    dotted.description = "dotted";
    dotted.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    dotted.strict = 1;
    dotted.handler = session_tool_echo_handler;
    slashed = dotted;
    slashed.name = "codex/files";
    slashed.description = "slashed";
    check_int_eq(turbo_tool_registry_add(registry, &dotted), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_add(registry, &slashed), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.provider = turbo_model_provider_openai_compatible_chat_completions();
    agent_config.transport_fn = session_success_transport;
    agent_config.tool_registry = registry;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_startup_diagnostics(session, &diagnostics), 0);
    check_not_null(diagnostics);
    check_int_eq(turbo_json_get_int(diagnostics, "schema_version", -1), 1);
    check_false(turbo_json_get_bool(diagnostics, "ok", true));
    errors = turbo_json_object_get(diagnostics, "errors");
    check_size_eq(turbo_json_array_size(errors), 1);
    check_str_eq(turbo_json_string(turbo_json_array_get(errors, 0)),
                 "invalid_tool_schemas");
    check_true(turbo_json_is_null(turbo_json_object_get(diagnostics, "tool_schemas")));
    capabilities = turbo_json_object_get(diagnostics, "capabilities");
    check_not_null(capabilities);
    check_int_eq(turbo_json_get_int(capabilities, "tool_count", -1), 2);

    turbo_free_json(&diagnostics);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

  it("should keep startup ready when an unselected tool schema format fails") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t dotted = {0};
    turbo_tool_definition_t slashed = {0};
    json_value_t *diagnostics = NULL;
    json_value_t *errors;
    json_value_t *status;
    json_value_t *compatible_status;

    check_not_null(registry);
    dotted.name = "codex.files";
    dotted.description = "dotted";
    dotted.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    dotted.strict = 1;
    dotted.handler = session_tool_echo_handler;
    slashed = dotted;
    slashed.name = "codex/files";
    slashed.description = "slashed";
    check_int_eq(turbo_tool_registry_add(registry, &dotted), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_add(registry, &slashed), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.provider = turbo_model_provider_openai_responses();
    agent_config.transport_fn = session_success_transport;
    agent_config.tool_registry = registry;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_startup_diagnostics(session, &diagnostics), 0);
    check_not_null(diagnostics);
    check_true(turbo_json_get_bool(diagnostics, "ok", false));
    errors = turbo_json_object_get(diagnostics, "errors");
    check_size_eq(turbo_json_array_size(errors), 0);
    check_true(turbo_json_is_null(turbo_json_object_get(diagnostics, "tool_schemas")));
    status = turbo_json_object_get(diagnostics, "tool_schema_status");
    check_str_eq(turbo_json_get_string(status, "selected_format"), "openai_responses");
    compatible_status = turbo_json_object_get(status, "openai_compatible_chat");
    check_false(turbo_json_get_bool(compatible_status, "ok", true));
    check_false(turbo_json_get_bool(compatible_status, "selected", true));

    turbo_free_json(&diagnostics);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

  it("should report workflow prerequisites in startup diagnostics") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *diagnostics = NULL;
    json_value_t *errors;
    json_value_t *workflow;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_startup_diagnostics(session, &diagnostics), 0);
    check_not_null(diagnostics);
    check_false(turbo_json_get_bool(diagnostics, "ok", true));
    errors = turbo_json_object_get(diagnostics, "errors");
    check_size_eq(turbo_json_array_size(errors), 1);
    check_str_eq(turbo_json_string(turbo_json_array_get(errors, 0)),
                 "missing_knowledge_store");
    workflow = turbo_json_object_get(diagnostics, "workflow");
    check_false(turbo_json_get_bool(workflow, "ok", true));
    check_true(turbo_json_get_bool(workflow, "requires_knowledge_store", false));
    check_str_eq(turbo_json_get_string(workflow, "reason"), "missing_knowledge_store");

    turbo_free_json(&diagnostics);
    turbo_agent_session_destroy(session);
  }

  it("should report invalid structured output schema in startup diagnostics") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *diagnostics = NULL;
    json_value_t *errors;
    json_value_t *structured_output;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    agent_config.structured_output_name = "answer";
    agent_config.structured_output_schema_json = "{\"type\":\"object\"";
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_get_startup_diagnostics(session, &diagnostics), 0);
    check_not_null(diagnostics);
    check_false(turbo_json_get_bool(diagnostics, "ok", true));
    errors = turbo_json_object_get(diagnostics, "errors");
    check_size_eq(turbo_json_array_size(errors), 1);
    check_str_eq(turbo_json_string(turbo_json_array_get(errors, 0)),
                 "invalid_structured_output_schema");
    structured_output = turbo_json_object_get(diagnostics, "structured_output");
    check_true(turbo_json_get_bool(structured_output, "configured", false));
    check_false(turbo_json_get_bool(structured_output, "ok", true));
    check_str_eq(turbo_json_get_string(structured_output, "reason"),
                 "invalid_structured_output_schema");

    turbo_free_json(&diagnostics);
    turbo_agent_session_destroy(session);
  }

  it("should invoke through session defaults without summary outputs") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(messages);

    check_int_eq(turbo_agent_session_invoke_text(session, "hello", NULL, &text, NULL), 0);
    check_str_eq(text, "ok");
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));
    free(text);
    text = NULL;

    check_int_eq(turbo_agent_session_invoke_messages_text(session, messages, NULL, &text, NULL),
                 0);
    check_str_eq(text, "ok");

    free(text);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should invoke parsed json through session defaults without summary outputs") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    json_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_json_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(messages);

    check_int_eq(turbo_agent_session_invoke_json(session, "hello", NULL, &result, NULL), 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));
    turbo_free_json(&result);

    check_int_eq(turbo_agent_session_invoke_messages_json(session, messages, NULL, &result, NULL),
                 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);

    turbo_free_json(&result);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should stream canonical messages through session defaults by mode") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    json_value_t *state = NULL;
    json_value_t *summary = NULL;
    session_replay_capture_t capture = {0};
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(messages);

    check_int_eq(turbo_agent_session_start_messages_stream(
                     session, messages, NULL, TURBO_EVENT_STREAM_MESSAGES,
                     session_capture_replayed_history_event, &capture, &summary, &state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(capture.model_count >= 1);
    check_size_eq(capture.trace_count, 0);
    check_size_eq(capture.tool_result_count, 0);
    text = turbo_agent_session_result_text(state);
    check_str_eq(text, "ok");

    free(text);
    turbo_runtime_json_destroy(state);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should batch user text through session defaults") {
    const char *inputs[] = {"hello one", "hello two"};
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *results = NULL;
    const json_value_t *first;
    const json_value_t *second;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_batch_text(session, inputs, 2, NULL, &results), 0);
    check_not_null(results);
    check_size_eq(turbo_json_array_size(results), 2);
    first = turbo_json_array_get(results, 0);
    second = turbo_json_array_get(results, 1);
    check_true(turbo_json_get_bool(first, "ok", false));
    check_true(turbo_json_get_bool(second, "ok", false));
    check_str_eq(turbo_json_get_string(first, "output_text"), "ok");
    check_str_eq(turbo_json_get_string(second, "output_text"), "ok");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(first, "summary"), "status"),
                 "completed");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(second, "summary"), "status"),
                 "completed");

    turbo_free_json(&results);
    turbo_agent_session_destroy(session);
  }

  it("should expose session defaults as a runnable") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_runnable_t *runnable;
    json_value_t *input = turbo_json_create_object();
    json_value_t *output = NULL;
    json_value_t *batch_inputs = turbo_json_create_array();
    json_value_t *batch_outputs = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(input);
    check_not_null(batch_inputs);
    check_int_eq(turbo_runtime_json_object_set(
                     input, "input", turbo_json_create_string("hello")),
                 TURBO_RUNTIME_JSON_OK);

    runnable = turbo_runnable_from_agent_session(session, NULL);
    check_not_null(runnable);
    check_int_eq(turbo_runnable_invoke_json_value(runnable, input, &output), 0);
    check_not_null(output);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(output, "output_text")),
                 "ok");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(
                         turbo_json_object_get(output, "summary"), "status")),
                 "completed");
    check_not_null(turbo_json_object_get(output, "state"));

    check_int_eq(turbo_runtime_json_array_append(
                     batch_inputs, turbo_json_create_string("first")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_array_append(
                     batch_inputs, turbo_json_create_string("second")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runnable_batch_json_value(runnable, batch_inputs, &batch_outputs), 0);
    check_size_eq(turbo_runtime_json_value_size(batch_outputs), 2);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(batch_outputs, 0), "output_text")),
                 "ok");
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_array_get(batch_outputs, 1), "output_text")),
                 "ok");

    turbo_runtime_json_destroy(batch_outputs);
    turbo_runtime_json_destroy(batch_inputs);
    turbo_runtime_json_destroy(output);
    turbo_runtime_json_destroy(input);
    turbo_runnable_destroy(runnable);
    turbo_agent_session_destroy(session);
  }

  it("should create a knowledge engineering preset graph from session config") {
    char *root_dir = session_temp_root();
    char *db_path = session_temp_path(root_dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_graph_t *graph;

    check_not_null(root_dir);
    check_not_null(db_path);
    check_not_null(store);
    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING;
    config.knowledge_store = store;
    config.knowledge_kind = "note";
    config.knowledge_limit = 3;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_int_eq(turbo_agent_session_workflow_kind(session),
                 TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING);

    graph = turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING);
    check_not_null(graph);
    check_str_eq(turbo_graph_get_entry(graph), "knowledge");
    check_size_eq(turbo_graph_node_count(graph), 13);
    check_size_eq(turbo_graph_edge_count(graph), 17);

    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
#ifdef _WIN32
    _rmdir(root_dir);
#else
    rmdir(root_dir);
#endif
    free(root_dir);
  }

  it("should reject knowledge engineering preset without a knowledge store") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_null(turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING));

    turbo_agent_session_destroy(session);
  }

  it("should create a retriever engineering preset graph from session config") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_retriever_config_t retriever_config = {0};
    turbo_retriever_t *retriever;
    turbo_graph_t *graph;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    retriever_config.query = session_retriever_query;
    retriever = turbo_retriever_create(&retriever_config);
    check_not_null(retriever);
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING;
    config.retriever = retriever;
    config.retriever_kind = "note";
    config.retriever_limit = 3;
    config.retriever_scope = "retriever";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_int_eq(turbo_agent_session_workflow_kind(session),
                 TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING);

    graph = turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING);
    check_not_null(graph);
    check_str_eq(turbo_graph_get_entry(graph), "retriever");
    check_size_eq(turbo_graph_node_count(graph), 13);
    check_size_eq(turbo_graph_edge_count(graph), 17);

    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
    turbo_retriever_destroy(retriever);
  }

  it("should reject retriever engineering preset without a retriever") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.agent_config = agent_config;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_null(turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING));

    turbo_agent_session_destroy(session);
  }

  it("should invoke canonical messages through session defaults with memory namespace") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.agent_config = agent_config;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    config.memory_namespace = "project";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(messages);

    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "/tmp/notes.md", "remember"),
                 0);
    check_int_eq(turbo_agent_session_invoke_messages_text(session, messages, NULL, &text, &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should invoke canonical messages through session defaults and return parsed json") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    json_value_t *messages = session_create_messages_json_value();
    json_value_t *summary = NULL;
    json_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_json_transport;
    config.agent_config = agent_config;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    config.memory_namespace = "project";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(messages);

    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "notes",
                                                        "project", "/tmp/notes.md", "remember"),
                 0);
    check_int_eq(turbo_agent_session_invoke_messages_json(session, messages, NULL, &result,
                                                          &summary),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);

    turbo_free_json(&result);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(messages);
    turbo_agent_session_destroy(session);
  }

  it("should bridge live trace sinks through session wrappers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_agent_trace_json_value_sink_t sink = {0};
    session_trace_capture_t capture = {0};
    json_value_t *thread_trace_events = NULL;
    json_value_t *run_trace_events = NULL;
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.agent_config = agent_config;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    sink.callback = session_capture_trace_event;
    sink.user_data = &capture;

    check_int_eq(turbo_agent_session_add_trace_json_value_sink(session, &sink), 0);
    check_int_eq(turbo_agent_session_set_trace_history_enabled(session, 1), 0);
    check_int_eq(turbo_agent_session_invoke_text(session, "hello", NULL, &text, &summary), 0);
    check_str_eq(text, "ok");
    check_true(capture.count >= 2);
    check_size_eq(capture.trace_count, capture.count);
    check_true(capture.model_request_count >= 1);
    check_true(capture.model_response_count >= 1);
    check_int_eq(turbo_agent_session_get_thread_trace_events_json_value(session, &thread_trace_events), 0);
    check_not_null(thread_trace_events);
    check_true(turbo_runtime_json_value_size(thread_trace_events) >= capture.count);
    check_int_eq(turbo_agent_session_get_run_trace_events_json_value(session, NULL, &run_trace_events), 0);
    check_not_null(run_trace_events);
    check_true(turbo_runtime_json_value_size(run_trace_events) >= capture.count);

    turbo_runtime_json_destroy(run_trace_events);
    turbo_runtime_json_destroy(thread_trace_events);
    free(text);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should bridge live observer sinks through session wrappers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_config_t agent_config = {0};
    turbo_agent_observer_json_value_sink_t sink = {0};
    session_observer_capture_t capture = {0};
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = session_success_transport;
    config.agent_config = agent_config;
    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    sink.callback = session_capture_observer_event;
    sink.user_data = &capture;

    check_int_eq(turbo_agent_session_add_observer_json_value_sink(session, &sink), 0);
    check_int_eq(turbo_agent_session_invoke_text(session, "hello", NULL, &text, &summary), 0);
    check_str_eq(text, "ok");
    check_true(capture.count >= 2);
    check_true(capture.model_delta_count >= 2);
    check_size_eq(capture.tool_call_started_count, 0);
    check_size_eq(capture.tool_result_count, 0);

    free(text);
    turbo_free_json(&summary);
    turbo_agent_session_destroy(session);
  }

  it("should emit live canonical stream events through session wrappers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *override = NULL;
    json_value_t *resumed_state = NULL;
    json_value_t *forked_state = NULL;
    json_value_t *command = NULL;
    json_value_t *summary = NULL;
    json_value_t *resumed_summary = NULL;
    json_value_t *forked_summary = NULL;
    session_replay_capture_t start_capture = {0};
    session_replay_capture_t resume_capture = {0};
    session_replay_capture_t fork_capture = {0};
    turbo_event_stream_filter_t start_filter = {0};
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *first_run_id = NULL;
    char *first_checkpoint_id = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    start_filter.mode = TURBO_EVENT_STREAM_DEBUG;
    start_filter.sink = session_capture_replayed_history_event;
    start_filter.sink_user_data = &start_capture;
    check_int_eq(turbo_agent_session_start_json_value_graph_stream(
                     session, graph, state, &options, turbo_event_stream_filter_sink_json_value,
                     &start_filter, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_true(start_capture.count >= 1);
    check_true(start_capture.trace_count >= 1);
    check_size_eq(start_capture.model_count, 0);
    check_size_eq(start_capture.tool_result_count, 0);
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));
    check_not_null(turbo_agent_session_last_checkpoint_id(session));
    first_run_id = session_query_only_memory_strdup(turbo_agent_session_last_run_id(session));
    first_checkpoint_id =
        session_query_only_memory_strdup(turbo_agent_session_last_checkpoint_id(session));
    check_not_null(first_run_id);
    check_not_null(first_checkpoint_id);

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("approve_review")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_agent_session_apply_checkpoint_command_json_value(session, first_checkpoint_id,
                                                                   command, &override),
                 0);
    turbo_runtime_json_destroy(command);
    command = NULL;

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_session_resume_json_value_graph_stream(
                     session, graph, first_checkpoint_id, override, &options,
                     session_capture_replayed_history_event, &resume_capture, &resumed_summary,
                     &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(resumed_summary, "status"), "completed");
    check_str_eq(turbo_agent_session_last_run_id(session), first_run_id);
    check_true(resume_capture.count >= 1);
    check_true(resume_capture.trace_count + resume_capture.model_count +
                   resume_capture.tool_result_count >=
               1);

    check_int_eq(turbo_agent_session_fork_json_value_graph_stream(
                     session, graph, first_checkpoint_id, override, &options,
                     session_capture_replayed_history_event, &fork_capture, &forked_summary,
                     &forked_state),
                 0);
    check_str_eq(turbo_json_get_string(forked_summary, "status"), "completed");
    check_true(strcmp(turbo_agent_session_last_run_id(session), first_run_id) != 0);
    check_true(fork_capture.count >= 1);
    check_true(fork_capture.trace_count + fork_capture.model_count + fork_capture.tool_result_count >=
               1);

    free(first_checkpoint_id);
    free(first_run_id);
    turbo_free_json(&forked_summary);
    turbo_free_json(&resumed_summary);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(forked_state);
    turbo_runtime_json_destroy(resumed_state);
    turbo_runtime_json_destroy(override);
    turbo_runtime_json_destroy(command);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should fail loudly on malformed memory-context records") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    json_value_t *state;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_int_eq(turbo_agent_session_memory_put(session, "project/demo", "bad", "{\"path\":\"/tmp/x\"}"),
                 0);
    state = turbo_agent_session_create_input_state_with_memory_json_value(session, "hello", "project");
    check_null(state);

    turbo_agent_session_destroy(session);
  }

  it("should expose one owned long-term memory store through the session") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    char *value_json = NULL;
    json_value_t *records = NULL;
    json_value_t *record_views = NULL;
    json_value_t *queried = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(turbo_agent_session_memory_store(session));

    check_int_eq(
        turbo_agent_session_memory_put(session, "project/demo", "notes", "{\"text\":\"remember\"}"),
        0);
    check_int_eq(turbo_agent_session_memory_get(session, "project/demo", "notes", &value_json), 0);
    check_str_eq(value_json, "{\"text\":\"remember\"}");
    free(value_json);

    check_int_eq(turbo_agent_session_memory_list(session, "project", &records), 0);
    check_size_eq(turbo_json_array_size(records), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(records, 0), "namespace"),
                 "project/demo");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(records, 0), "key"), "notes");
    check_int_eq(turbo_agent_session_memory_list_records(session, "project", &record_views), 0);
    check_size_eq(turbo_json_array_size(record_views), 1);
    session_check_memory_record_fixture(turbo_json_array_get(record_views, 0),
                                        "memory_json_record.golden.json", NULL);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(record_views, 0), "kind"), "json");
    check_int_eq(turbo_agent_session_memory_put_context(session, "project/demo", "context",
                                                        "project", "/tmp/notes.md",
                                                        "remember this"),
                 0);
    check_int_eq(turbo_agent_session_memory_query_records(session, "project", "context", "con",
                                                          "remember", &queried),
                 0);
    check_size_eq(turbo_json_array_size(queried), 1);
    session_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                              "context_query");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "text"),
                 "remember this");

    check_int_eq(turbo_agent_session_memory_delete(session, "project/demo", "notes"), 0);

    turbo_free_json(&queried);
    turbo_free_json(&record_views);
    turbo_free_json(&records);
    turbo_agent_session_destroy(session);
  }

  it("should expose one query-only long-term memory store through the session") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    json_value_t *record_views = NULL;
    json_value_t *queried = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = session_query_only_memory_store_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(turbo_agent_session_memory_store(session));

    check_int_eq(turbo_agent_session_memory_list_records(session, "project", &record_views), 0);
    check_size_eq(turbo_json_array_size(record_views), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(record_views, 0), "kind"), "context");

    check_int_eq(turbo_agent_session_memory_query_records(session, "project", "context", "con",
                                                          "remember", &queried),
                 0);
    session_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                              "context_query");

    turbo_free_json(&queried);
    turbo_free_json(&record_views);
    turbo_agent_session_destroy(session);
  }

  it("should expose canonical memory-record helpers through the session") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *record = turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
    json_value_t *loaded = NULL;
    json_value_t *queried = NULL;
    json_value_t *state = NULL;
    json_value_t *state_json = NULL;
    char *memory_text = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.memory_store = turbo_agent_memory_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(record);

    check_int_eq(turbo_agent_session_memory_validate_record(record), 0);
    check_int_eq(turbo_agent_session_memory_put_record(session, record), 0);
    check_int_eq(turbo_agent_session_memory_get_record(session, "project/demo", "context", &loaded),
                 0);
    session_check_memory_record_fixture(loaded, "memory_context_record.golden.json", NULL);

    options.namespace_prefix = "project";
    options.kind = "context";
    options.key_prefix = "con";
    options.text_substring = "remember";
    check_int_eq(turbo_agent_session_memory_query_records_ex(session, &options, &queried), 0);
    session_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                              "context_query");

    state = turbo_agent_session_create_input_state_with_memory_json_value(session, "hello", "project");
    check_not_null(state);
    state_json = turbo_json_clone(state);
    check_not_null(state_json);
    memory_text = turbo_agent_state_memory_context_text(state_json);
    check_not_null(memory_text);
    check_true(strstr(memory_text, "remember this") != NULL);

    free(memory_text);
    turbo_free_json(&state_json);
    turbo_runtime_json_destroy(state);
    turbo_free_json(&queried);
    turbo_free_json(&loaded);
    turbo_free_json(&record);
    turbo_agent_session_destroy(session);
  }

  it("should remember thread run and checkpoint ids across start resume and fork") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *override = NULL;
    json_value_t *summary = NULL;
    json_value_t *fork_summary = NULL;
    json_value_t *lineage = NULL;
    json_value_t *branch_tree = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *first_run_id;
    char *first_checkpoint_id;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    first_run_id = session_test_strdup(turbo_agent_session_last_run_id(session));
    first_checkpoint_id = session_test_strdup(turbo_agent_session_last_checkpoint_id(session));
    check_not_null(first_run_id);
    check_not_null(first_checkpoint_id);

    override = create_session_override(result_state, 1);
    turbo_runtime_json_destroy(result_state);
    result_state = NULL;
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    turbo_free_json(&summary);
    check_int_eq(turbo_agent_session_resume_json_value_graph(session, graph, NULL, override, &options,
                                                       &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(turbo_agent_session_last_run_id(session), first_run_id);
    check_str_eq(turbo_agent_session_last_checkpoint_id(session), first_checkpoint_id);

    turbo_runtime_json_destroy(result_state);
    result_state = NULL;
    check_int_eq(turbo_agent_session_fork_json_value_graph(session, graph, NULL, override, NULL,
                                                     &fork_summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(fork_summary, "status"), "completed");
    check_true(strcmp(turbo_agent_session_last_run_id(session), first_run_id) != 0);
    check_str_eq(turbo_agent_session_last_checkpoint_id(session), first_checkpoint_id);
    check_str_eq(turbo_agent_session_thread_id(session),
                 turbo_json_get_string(fork_summary, "thread_id"));
    check_int_eq(turbo_agent_session_list_thread_lineage(session, &lineage), 0);
    session_check_thread_lineage_json_value(lineage, turbo_agent_session_thread_id(session),
                                      turbo_json_get_string(fork_summary, "run_id"), NULL,
                                      first_checkpoint_id);
    check_true(turbo_json_array_size(turbo_json_object_get(lineage, "branches")) >= 2);
    session_check_lineage_branch(
        session_find_lineage_branch(turbo_json_object_get(lineage, "branches"), NULL), NULL, NULL,
        NULL, first_checkpoint_id);
    session_check_lineage_branch(
        session_find_lineage_branch(turbo_json_object_get(lineage, "branches"), first_run_id),
        first_run_id, first_checkpoint_id, first_checkpoint_id, first_checkpoint_id);
    check_int_eq(turbo_agent_session_get_branch_tree(session, &branch_tree), 0);
    session_check_branch_tree(branch_tree, turbo_agent_session_thread_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session), NULL, NULL, 2, 1);
    session_check_branch_tree_branch(
        session_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                        first_run_id),
        first_run_id, NULL, NULL, first_checkpoint_id);
    session_check_branch_tree_branch(
        session_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                        turbo_agent_session_last_run_id(session)),
        turbo_agent_session_last_run_id(session), first_run_id, first_checkpoint_id,
        first_checkpoint_id);
    session_check_branch_tree_edge(
        session_find_branch_tree_edge(turbo_json_object_get(branch_tree, "edges"),
                                      first_checkpoint_id, turbo_agent_session_last_run_id(session)),
        first_checkpoint_id, turbo_agent_session_last_run_id(session));

    free(first_run_id);
    free(first_checkpoint_id);
    turbo_free_json(&lineage);
    turbo_free_json(&branch_tree);
    turbo_free_json(&fork_summary);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(override);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should allow unified resume and fork with null override input") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *fork_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    turbo_agent_session_exec_options_t exec_options = {
        .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
        .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
    };
    char *first_run_id;
    char *first_checkpoint_id;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_graph(session, graph, state, &options, NULL, NULL,
                                                 &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    first_run_id = session_test_strdup(turbo_agent_session_last_run_id(session));
    first_checkpoint_id = session_test_strdup(turbo_agent_session_last_checkpoint_id(session));
    check_not_null(first_run_id);
    check_not_null(first_checkpoint_id);

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    exec_options.checkpoint_id = first_checkpoint_id;
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    result_state = NULL;

    check_int_eq(turbo_agent_session_resume_graph(session, graph, NULL, &options, &exec_options,
                                                  NULL, NULL, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(turbo_agent_session_last_run_id(session), first_run_id);
    check_str_eq(turbo_agent_session_last_checkpoint_id(session), first_checkpoint_id);

    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    result_state = NULL;

    check_int_eq(turbo_agent_session_fork_graph(session, graph, NULL, NULL, &exec_options, NULL,
                                                NULL, &fork_summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(fork_summary, "status"), "completed");
    check_true(strcmp(turbo_agent_session_last_run_id(session), first_run_id) != 0);
    check_str_eq(turbo_agent_session_last_checkpoint_id(session), first_checkpoint_id);

    free(first_run_id);
    free(first_checkpoint_id);
    turbo_free_json(&fork_summary);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should query thread run checkpoint and history through the session wrapper") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *thread_json = NULL;
    json_value_t *run_json = NULL;
    json_value_t *latest_run_json = NULL;
    json_value_t *pending_run_json = NULL;
    json_value_t *checkpoint_json = NULL;
    json_value_t *latest_checkpoint_json = NULL;
    json_value_t *runs_json = NULL;
    json_value_t *checkpoints_json = NULL;
    json_value_t *history = NULL;
    session_replay_capture_t replay = {0};
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_thread(session, &thread_json), 0);
    check_str_eq(turbo_json_get_string(thread_json, "id"), turbo_agent_session_thread_id(session));

    check_int_eq(turbo_agent_session_get_run(session, NULL, &run_json), 0);
    check_str_eq(turbo_json_get_string(run_json, "id"), turbo_agent_session_last_run_id(session));

    check_int_eq(turbo_agent_session_get_latest_run(session, &latest_run_json), 0);
    check_str_eq(turbo_json_get_string(latest_run_json, "id"),
                 turbo_agent_session_last_run_id(session));

    check_int_eq(turbo_agent_session_get_pending_run(session, &pending_run_json), 0);
    check_str_eq(turbo_json_get_string(pending_run_json, "id"),
                 turbo_agent_session_last_run_id(session));

    check_int_eq(turbo_agent_session_get_checkpoint(session, NULL, &checkpoint_json), 0);
    check_str_eq(turbo_json_get_string(checkpoint_json, "id"),
                 turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(turbo_agent_session_get_latest_checkpoint(session, NULL, &latest_checkpoint_json),
                 0);
    check_str_eq(turbo_json_get_string(latest_checkpoint_json, "id"),
                 turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(turbo_agent_session_list_runs(session, &runs_json), 0);
    check_size_eq(turbo_json_array_size(runs_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "id"),
                 turbo_agent_session_last_run_id(session));

    check_int_eq(turbo_agent_session_list_checkpoints(session, NULL, &checkpoints_json), 0);
    check_size_eq(turbo_json_array_size(checkpoints_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(checkpoints_json, 0), "id"),
                 turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(turbo_agent_session_load_history_events_json_value(session, NULL, NULL, &history), 0);
    check_true(turbo_runtime_json_value_size(history) >= 1);
    check_int_eq(turbo_agent_session_replay_history_json_value(session, NULL, NULL,
                                                         session_capture_replayed_history_event,
                                                         &replay),
                 0);
    check_size_eq(replay.count, turbo_runtime_json_value_size(history));
    check_true(replay.count >= 1);
    turbo_runtime_json_destroy(history);
    history = NULL;
    memset(&replay, 0, sizeof(replay));
    check_int_eq(turbo_agent_session_load_thread_history_events_json_value(session, &history), 0);
    check_true(turbo_runtime_json_value_size(history) >= 1);
    check_int_eq(turbo_agent_session_replay_thread_history_json_value(
                     session, session_capture_replayed_history_event, &replay),
                 0);
    check_size_eq(replay.count, turbo_runtime_json_value_size(history));

    turbo_runtime_json_destroy(history);
    turbo_free_json(&checkpoints_json);
    turbo_free_json(&runs_json);
    turbo_free_json(&latest_checkpoint_json);
    turbo_free_json(&checkpoint_json);
    turbo_free_json(&pending_run_json);
    turbo_free_json(&latest_run_json);
    turbo_free_json(&run_json);
    turbo_free_json(&thread_json);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should merge TurboParser JSON-native state patches through the thread replay wrapper") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_state_patch_graph();
    json_value_t *state = turbo_agent_state_create_json_value();
    json_value_t *result_state = NULL;
    json_value_t *patched_state = NULL;
    json_value_t *resumed_state = NULL;
    json_value_t *patch = create_session_state_patch_json_value();
    json_value_t *summary = NULL;
    json_value_t *resumed_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    const json_value_t *profile;
    const json_value_t *settings;
    const json_value_t *flags;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(patch);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(turbo_agent_session_update_thread_state_json_value(session, patch, &patched_state), 0);
    check_not_null(patched_state);
    profile = turbo_json_object_get(patched_state, "profile");
    settings = profile ? turbo_json_object_get(profile, "settings") : NULL;
    flags = profile ? turbo_json_object_get(profile, "flags") : NULL;
    check_not_null(profile);
    check_not_null(settings);
    check_not_null(flags);
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(profile, "name")),
                 "alpha");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(settings, "theme")),
                 "dark");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(settings, "locale")),
                 "en");
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(settings, "extra")),
                 "enabled");
    check_true(turbo_json_type(
                   turbo_json_object_get(settings, "notes")) ==
               TURBO_JSON_NULL);
    check_size_eq(turbo_runtime_json_value_size(flags), 2);

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_session_resume_thread_state_json_value_graph(
                     session, graph, patch, &options, &resumed_summary, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(resumed_summary, "status"), "completed");
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(resumed_state, "visited_end"), 0));
    check_str_eq(turbo_runtime_json_value_as_string(
                     turbo_json_object_get(
                         turbo_json_object_get(
                             turbo_json_object_get(resumed_state, "profile"),
                             "settings"),
                         "theme")),
                 "dark");

    turbo_runtime_json_destroy(resumed_state);
    turbo_runtime_json_destroy(patched_state);
    turbo_runtime_json_destroy(patch);
    turbo_free_json(&resumed_summary);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should bridge durable history through session observer wrappers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(1);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    session_observer_capture_t thread_capture = {0};
    session_observer_capture_t checkpoint_capture = {0};
    turbo_agent_observer_json_value_sink_t thread_sink = {0};
    turbo_agent_observer_json_value_sink_t checkpoint_sink = {0};
    const char *interrupt_before_end[] = {"end"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_end;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    thread_sink.callback = session_capture_observer_event;
    thread_sink.user_data = &thread_capture;
    checkpoint_sink.callback = session_capture_observer_event;
    checkpoint_sink.user_data = &checkpoint_capture;

    check_int_eq(turbo_agent_session_observe_thread_history_json_value(session, &thread_sink), 0);
    check_int_eq(turbo_agent_session_observe_history_json_value(session, NULL, NULL, &checkpoint_sink),
                 0);
    check_true(thread_capture.count >= 1);
    check_true(thread_capture.interrupted_count >= 1);
    check_size_eq(checkpoint_capture.count, thread_capture.count);
    check_size_eq(checkpoint_capture.interrupted_count, thread_capture.interrupted_count);

    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose thread timeline for an interrupted thread through the session wrapper") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *timeline = NULL;
    json_value_t *thread_lineage = NULL;
    json_value_t *summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_thread_timeline_json_value(session, &timeline), 0);
    session_check_thread_timeline_json_value(timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session),
                                       turbo_agent_session_last_checkpoint_id(session), 1, 1, 1,
                                       1);
    check_int_eq(turbo_agent_session_list_thread_lineage(session, &thread_lineage), 0);
    session_check_thread_lineage_json_value(thread_lineage, turbo_agent_session_thread_id(session),
                                      turbo_agent_session_last_run_id(session),
                                      turbo_agent_session_last_run_id(session),
                                      turbo_agent_session_last_checkpoint_id(session));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(thread_lineage, "branches")), 1);
    session_check_lineage_branch(
        session_find_lineage_branch(turbo_json_object_get(thread_lineage, "branches"), NULL), NULL,
        NULL, NULL, turbo_agent_session_last_checkpoint_id(session));

    turbo_free_json(&thread_lineage);
    turbo_runtime_json_destroy(timeline);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose mailbox wrappers through the session surface") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_supervisor_review_state();
    json_value_t *result_state = NULL;
    json_value_t *override = NULL;
    json_value_t *summary = NULL;
    json_value_t *inbox = NULL;
    json_value_t *history = NULL;
    json_value_t *override_json = NULL;
    const json_value_t *override_inbox = NULL;
    const json_value_t *override_history = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_supervisor_inbox(session, &inbox), 0);
    check_int_eq(turbo_agent_session_get_supervisor_handoff_history(session, &history), 0);
    check_not_null(inbox);
    check_not_null(history);
    check_size_eq(turbo_json_array_size(inbox), 0);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "target_agent"),
                 "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "reason"),
                 "delegate execution");

    check_int_eq(
        turbo_agent_session_append_supervisor_inbox_message_json_value(session, "supervisor",
                                                                 "review this", &override),
        0);
    check_not_null(override);
    override_json = turbo_json_clone(override);
    check_not_null(override_json);
    override_inbox = turbo_agent_state_supervisor_inbox(override_json);
    override_history = turbo_agent_state_supervisor_handoff_history(override_json);
    check_not_null(override_inbox);
    check_not_null(override_history);
    check_size_eq(turbo_json_array_size(override_inbox), 1);
    check_size_eq(turbo_json_array_size(override_history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(override_inbox, 0), "source_agent"),
                 "supervisor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(override_inbox, 0), "text"),
                 "review this");

    turbo_free_json(&override_json);
    turbo_runtime_json_destroy(override);
    turbo_free_json(&history);
    turbo_free_json(&inbox);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose one supervisor inspect bundle through the session surface") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_supervisor_review_state();
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *inspect = NULL;
    const json_value_t *supervisor = NULL;
    const json_value_t *inbox = NULL;
    const json_value_t *history = NULL;
    const json_value_t *latest_handoff_event = NULL;
    const json_value_t *control = NULL;
    const json_value_t *workflow = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_supervisor_inspect(session, &inspect), 0);
    check_not_null(inspect);
    supervisor = turbo_json_object_get(inspect, "supervisor");
    inbox = turbo_json_object_get(inspect, "inbox");
    history = turbo_json_object_get(inspect, "handoff_history");
    latest_handoff_event = turbo_json_object_get(inspect, "latest_handoff_event");
    control = turbo_json_object_get(inspect, "control_snapshot");
    workflow = turbo_json_object_get(inspect, "workflow_snapshot");
    check_not_null(supervisor);
    check_not_null(inbox);
    check_not_null(history);
    check_not_null(latest_handoff_event);
    check_not_null(control);
    check_not_null(workflow);
    check_str_eq(turbo_json_get_string(supervisor, "active_agent"), "planner");
    check_str_eq(turbo_json_get_string(supervisor, "target_agent"), "executor");
    check_str_eq(turbo_json_get_string(supervisor, "handoff_reason"), "delegate execution");
    check_int_eq(turbo_json_get_int(supervisor, "inbox_count", 0), 0);
    check_int_eq(turbo_json_get_int(supervisor, "handoff_count", 0), 1);
    check_size_eq(turbo_json_array_size(inbox), 0);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(latest_handoff_event, "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(latest_handoff_event), "requested");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(latest_handoff_event), "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(latest_handoff_event), "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(latest_handoff_event),
                 "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(latest_handoff_event), "planner");
    check_not_null(turbo_json_object_get(control, "supervisor"));
    check_not_null(turbo_json_object_get(workflow, "supervisor"));

    turbo_free_json(&inspect);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should run one real supervisor handoff loop through the session surface") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_supervisor_handoff_graph();
    json_value_t *state = create_session_supervisor_handoff_state();
    json_value_t *result_state = NULL;
    json_value_t *inspect_timeline = NULL;
    json_value_t *summary = NULL;
    json_value_t *state_json = NULL;
    json_value_t *inbox = NULL;
    json_value_t *history = NULL;
    json_value_t *supervisor_inspect = NULL;
    json_value_t *orchestration_inspect = NULL;
    const json_value_t *latest_handoff_event = NULL;
    const json_value_t *nested_latest_handoff_event = NULL;
    const json_value_t *thread_lineage;
    const json_value_t *child_runs;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, NULL, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(turbo_json_get_string(summary, "thread_id"),
                 turbo_agent_session_thread_id(session));
    check_str_eq(turbo_json_get_string(summary, "run_id"),
                 turbo_agent_session_last_run_id(session));
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);
    check_str_eq(turbo_json_get_string(summary, "active_agent"), "executor");
    check_true(turbo_json_type(turbo_json_object_get(summary, "handoff_target_agent")) ==
               TURBO_JSON_NULL);
    check_true(turbo_json_type(turbo_json_object_get(summary, "handoff_reason")) ==
               TURBO_JSON_NULL);
    check_not_null(turbo_agent_session_thread_id(session));
    check_not_null(turbo_agent_session_last_run_id(session));
    check_null(turbo_agent_session_last_checkpoint_id(session));

    state_json = turbo_json_clone(result_state);
    check_not_null(state_json);
    check_true(turbo_json_get_bool(state_json, "visited_planner", false));
    check_true(turbo_json_get_bool(state_json, "visited_executor", false));
    check_str_eq(turbo_agent_state_active_agent(state_json), "executor");
    check_null(turbo_agent_state_handoff_target_agent(state_json));
    check_null(turbo_agent_state_handoff_reason(state_json));

    check_int_eq(turbo_agent_session_get_supervisor_inbox(session, &inbox), 0);
    check_int_eq(turbo_agent_session_get_supervisor_handoff_history(session, &history), 0);
    check_size_eq(turbo_json_array_size(inbox), 0);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "target_agent"),
                 "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "reason"),
                 "delegate execution");

    check_int_eq(turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect), 0);
    turbo_agent_test_check_supervisor_inspect(supervisor_inspect, "executor", "", "", 0, 1);
    latest_handoff_event = turbo_json_object_get(supervisor_inspect, "latest_handoff_event");
    check_not_null(latest_handoff_event);
    check_str_eq(turbo_json_get_string(latest_handoff_event, "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(latest_handoff_event), "committed");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(latest_handoff_event), "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(latest_handoff_event), "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(latest_handoff_event),
                 "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(latest_handoff_event), "executor");

    check_int_eq(turbo_agent_session_get_orchestration_inspect(session, &orchestration_inspect), 0);
    turbo_agent_test_check_supervisor_inspect(
        turbo_json_object_get(orchestration_inspect, "supervisor_inspect"), "executor", "", "",
        0, 1);
    nested_latest_handoff_event = turbo_json_object_get(
        turbo_json_object_get(orchestration_inspect, "supervisor_inspect"), "latest_handoff_event");
    check_not_null(nested_latest_handoff_event);
    check_str_eq(turbo_json_get_string(nested_latest_handoff_event, "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(nested_latest_handoff_event), "committed");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(nested_latest_handoff_event),
                 "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(nested_latest_handoff_event),
                 "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(nested_latest_handoff_event),
                 "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(nested_latest_handoff_event),
                 "executor");
    inspect_timeline = turbo_json_clone(
        turbo_json_object_get(orchestration_inspect, "thread_timeline"));
    check_not_null(inspect_timeline);
    session_check_thread_timeline_json_value(inspect_timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session), NULL, 0, 1, 0,
                                       0);
    thread_lineage = turbo_json_object_get(orchestration_inspect, "thread_lineage");
    check_not_null(thread_lineage);
    check_str_eq(turbo_json_get_string(thread_lineage, "thread_id"),
                 turbo_agent_session_thread_id(session));
    check_str_eq(turbo_json_get_string(thread_lineage, "latest_run_id"),
                 turbo_agent_session_last_run_id(session));
    check_not_null(turbo_json_object_get(thread_lineage, "pending_run_id"));
    check_not_null(turbo_json_object_get(thread_lineage, "root_checkpoint_id"));
    check_not_null(turbo_json_object_get(thread_lineage, "branches"));
    check_true(turbo_json_type(turbo_json_object_get(thread_lineage, "branches")) ==
               TURBO_JSON_ARRAY);
    child_runs = turbo_json_object_get(orchestration_inspect, "child_runs");
    check_not_null(child_runs);
    check_true(turbo_json_type(child_runs) == TURBO_JSON_ARRAY);
    check_size_eq(turbo_json_array_size(child_runs), 0);

    turbo_runtime_json_destroy(inspect_timeline);
    turbo_free_json(&orchestration_inspect);
    turbo_free_json(&supervisor_inspect);
    turbo_free_json(&history);
    turbo_free_json(&inbox);
    turbo_free_json(&state_json);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose one orchestration inspect bundle through the session surface") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_supervisor_review_state();
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *inspect = NULL;
    json_value_t *timeline = NULL;
    const json_value_t *supervisor_inspect = NULL;
    const json_value_t *thread_lineage = NULL;
    const json_value_t *branch_tree = NULL;
    const json_value_t *child_runs = NULL;
    const json_value_t *latest_handoff_event = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_orchestration_inspect(session, &inspect), 0);
    check_not_null(inspect);
    supervisor_inspect = turbo_json_object_get(inspect, "supervisor_inspect");
    thread_lineage = turbo_json_object_get(inspect, "thread_lineage");
    branch_tree = turbo_json_object_get(inspect, "branch_tree");
    child_runs = turbo_json_object_get(inspect, "child_runs");
    check_not_null(supervisor_inspect);
    check_not_null(thread_lineage);
    check_not_null(branch_tree);
    check_not_null(child_runs);
    check_true(turbo_json_type(child_runs) == TURBO_JSON_ARRAY);
    check_size_eq(turbo_json_array_size(child_runs), 0);
    check_not_null(turbo_json_object_get(supervisor_inspect, "supervisor"));
    latest_handoff_event = turbo_json_object_get(supervisor_inspect, "latest_handoff_event");
    check_not_null(latest_handoff_event);
    check_str_eq(turbo_json_get_string(latest_handoff_event, "kind"), "handoff");
    check_str_eq(turbo_agent_state_handoff_event_phase(latest_handoff_event), "requested");
    check_str_eq(turbo_agent_state_handoff_event_from_agent(latest_handoff_event), "planner");
    check_str_eq(turbo_agent_state_handoff_event_target_agent(latest_handoff_event), "executor");
    check_str_eq(turbo_agent_state_handoff_event_reason(latest_handoff_event),
                 "delegate execution");
    check_str_eq(turbo_agent_state_handoff_event_active_agent(latest_handoff_event), "planner");
    check_not_null(turbo_json_object_get(supervisor_inspect, "control_snapshot"));
    check_not_null(turbo_json_object_get(supervisor_inspect, "workflow_snapshot"));

    timeline = turbo_json_clone(
        turbo_json_object_get(inspect, "thread_timeline"));
    check_not_null(timeline);
    session_check_thread_timeline_json_value(timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session),
                                       turbo_agent_session_last_checkpoint_id(session), 1, 1, 1,
                                       1);
    session_check_thread_lineage_json_value(thread_lineage, turbo_agent_session_thread_id(session),
                                      turbo_agent_session_last_run_id(session),
                                      turbo_agent_session_last_run_id(session),
                                      turbo_agent_session_last_checkpoint_id(session));
    session_check_branch_tree(branch_tree, turbo_agent_session_thread_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_checkpoint_id(session), 1, 0);

    turbo_runtime_json_destroy(timeline);
    turbo_free_json(&inspect);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose one observability index bundle through the session surface") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_supervisor_review_state();
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *index_json = NULL;
    const json_value_t *counts = NULL;
    const json_value_t *history_events = NULL;
    const json_value_t *trace_events = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_session_get_observability_index(session, &index_json), 0);
    check_not_null(index_json);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(index_json, "thread"), "id"),
                 turbo_agent_session_thread_id(session));
    check_not_null(turbo_json_object_get(index_json, "thread_timeline"));
    check_not_null(turbo_json_object_get(index_json, "thread_lineage"));
    check_not_null(turbo_json_object_get(index_json, "branch_tree"));
    check_str_eq(turbo_json_get_string(index_json, "current_status"), "interrupted");
    check_str_eq(turbo_json_get_string(index_json, "current_interrupt_reason"),
                 "review_required");
    check_str_eq(turbo_json_get_string(index_json, "current_pending_action"), "review");
    check_str_eq(turbo_json_get_string(
                     turbo_json_object_get(index_json, "current_checkpoint_summary"), "id"),
                 turbo_agent_session_last_checkpoint_id(session));
    check_str_eq(turbo_json_get_string(index_json, "latest_run_status"), "interrupted");
    check_not_null(turbo_json_get_string(index_json, "latest_run_updated_at"));
    check_str_eq(turbo_json_get_string(index_json, "pending_run_id"),
                 turbo_agent_session_last_run_id(session));
    check_str_eq(turbo_json_get_string(index_json, "pending_checkpoint_id"),
                 turbo_agent_session_last_checkpoint_id(session));
    check_false(turbo_json_get_bool(index_json, "has_failure", true));
    check_false(turbo_json_get_bool(index_json, "has_model_error", true));
    check_false(turbo_json_get_bool(index_json, "has_guardrail_rejection", true));
    check_false(turbo_json_get_bool(index_json, "replan_requested", true));
    check_true(turbo_json_is_null(turbo_json_object_get(index_json, "current_failure_reason")));
    check_str_eq(turbo_json_get_string(index_json, "current_review_note"), "need approval");
    check_true(turbo_json_get_bool(index_json, "has_pending_review", false));
    check_true(turbo_json_get_bool(index_json, "has_handoff", false));
    check_str_eq(turbo_json_get_string(index_json, "active_agent"), "planner");
    history_events = turbo_json_object_get(index_json, "history_events");
    trace_events = turbo_json_object_get(index_json, "trace_events");
    counts = turbo_json_object_get(index_json, "counts");
    check_true(turbo_json_type(history_events) == TURBO_JSON_ARRAY);
    check_true(turbo_json_type(trace_events) == TURBO_JSON_ARRAY);
    check_not_null(counts);
    check_int_eq(turbo_json_get_int(counts, "runs", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "interrupted_runs", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "completed_runs", -1), 0);
    check_int_eq(turbo_json_get_int(counts, "current_run_checkpoints", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "branches", -1), 1);
    check_int_eq(turbo_json_get_int(counts, "edges", -1), 0);
    check_int_eq(turbo_json_get_int(counts, "history_events", -1),
                 (int)turbo_json_array_size(history_events));
    check_int_eq(turbo_json_get_int(counts, "trace_events", -1),
                 (int)turbo_json_array_size(trace_events));

    turbo_free_json(&index_json);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose thread timeline for a completed-only thread through the session wrapper") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(1);
    json_value_t *thread_state = NULL;
    json_value_t *run_state = NULL;
    json_value_t *replay_override = NULL;
    json_value_t *replay_state = NULL;
    json_value_t *result_state = NULL;
    json_value_t *timeline = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, NULL, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);

    check_int_eq(turbo_agent_session_get_thread_timeline_json_value(session, &timeline), 0);
    session_check_thread_timeline_json_value(timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session), NULL, 0, 1, 0,
                                       0);
    check_int_eq(turbo_agent_session_get_thread_state_json_value(session, &thread_state), 0);
    check_int_eq(turbo_agent_session_get_run_state_json_value(session, NULL, &run_state), 0);
    replay_override = create_session_override(thread_state, 1);
    check_not_null(replay_override);
    check_true(turbo_agent_session_fork_thread_json_value_graph(session, graph, replay_override, NULL,
                                                          &replay_summary, &replay_state) != 0);
    check_null(replay_summary);
    check_null(replay_state);
    turbo_runtime_json_destroy(replay_override);
    replay_override = NULL;

    turbo_runtime_json_destroy(timeline);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(run_state);
    turbo_runtime_json_destroy(thread_state);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should fork an interrupted thread through the thread-scoped replay wrapper") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *thread_state = NULL;
    json_value_t *replay_override = NULL;
    json_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_int_eq(turbo_agent_session_get_thread_state_json_value(session, &thread_state), 0);
    replay_override = create_session_override(thread_state, 1);
    check_not_null(replay_override);
    check_int_eq(turbo_agent_session_fork_thread_json_value_graph(session, graph, replay_override, NULL,
                                                            &replay_summary, &replay_state),
                 0);
    check_str_eq(turbo_json_get_string(replay_summary, "status"), "completed");
    check_false(strcmp(turbo_json_get_string(replay_summary, "run_id"),
                       turbo_json_get_string(summary, "run_id")) == 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(replay_state, "visited_end"), 0));

    turbo_free_json(&replay_summary);
    turbo_runtime_json_destroy(replay_state);
    turbo_runtime_json_destroy(replay_override);
    turbo_runtime_json_destroy(thread_state);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should resume one interrupted checkpoint through the explicit checkpoint-scoped replay alias") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *override = NULL;
    json_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;
    json_value_t *checkpoint_context = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *first_run_id;
    char *first_checkpoint_id;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    first_run_id = session_test_strdup(turbo_agent_session_last_run_id(session));
    first_checkpoint_id = session_test_strdup(turbo_agent_session_last_checkpoint_id(session));
    check_not_null(first_run_id);
    check_not_null(first_checkpoint_id);

    override = create_session_override(result_state, 1);
    check_not_null(override);
    turbo_runtime_json_destroy(result_state);
    result_state = NULL;
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_session_resume_checkpoint_json_value_graph(
                     session, graph, first_checkpoint_id, override, &options, &replay_summary,
                     &replay_state),
                 0);
    check_str_eq(turbo_json_get_string(replay_summary, "status"), "completed");
    check_str_eq(turbo_json_get_string(replay_summary, "run_id"), first_run_id);
    check_str_eq(turbo_agent_session_last_run_id(session), first_run_id);
    check_str_eq(turbo_agent_session_last_checkpoint_id(session), first_checkpoint_id);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(replay_state, "visited_end"), 0));
    check_int_eq(turbo_agent_session_get_checkpoint_context(session, first_checkpoint_id,
                                                             &checkpoint_context),
                 0);
    session_check_checkpoint_context(checkpoint_context, turbo_agent_session_thread_id(session),
                                     first_run_id, first_checkpoint_id, NULL, 1);

    free(first_run_id);
    free(first_checkpoint_id);
    turbo_free_json(&replay_summary);
    turbo_free_json(&checkpoint_context);
    turbo_runtime_json_destroy(replay_state);
    turbo_runtime_json_destroy(override);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose persisted state and apply checkpoint commands through session wrappers") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    json_value_t *result_state = NULL;
    json_value_t *thread_state = NULL;
    json_value_t *thread_head_state = NULL;
    json_value_t *thread_trace_events = NULL;
    json_value_t *thread_head_trace_events = NULL;
    json_value_t *run_state = NULL;
    json_value_t *checkpoint_state = NULL;
    json_value_t *state_patch = NULL;
    json_value_t *prepared_state_override = NULL;
    json_value_t *legacy_state_override = NULL;
    json_value_t *command = NULL;
    json_value_t *override = NULL;
    json_value_t *rejected_override = NULL;
    json_value_t *prepared_command_override = NULL;
    json_value_t *resumed_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    json_value_t *override_json = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(turbo_agent_session_get_thread_state_json_value(session, &thread_state), 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(thread_state, "visited_start"), 0));
    check_int_eq(turbo_agent_session_get_thread_head_state_json_value(session, &thread_head_state), 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(thread_head_state, "visited_start"), 0));
    check_int_eq(turbo_agent_session_get_thread_trace_events_json_value(session, &thread_trace_events), 0);
    check_int_eq(turbo_agent_session_get_thread_head_trace_events_json_value(session,
                                                                       &thread_head_trace_events),
                 0);
    check_size_eq(turbo_runtime_json_value_size(thread_head_trace_events),
                  turbo_runtime_json_value_size(thread_trace_events));

    check_int_eq(turbo_agent_session_get_run_state_json_value(session, NULL, &run_state), 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(run_state, "visited_start"), 0));

    check_int_eq(turbo_agent_session_get_checkpoint_state_json_value(session, NULL, &checkpoint_state),
                 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(checkpoint_state, "visited_start"), 0));

    state_patch = create_session_state_patch_json_value();
    check_not_null(state_patch);
    check_int_eq(turbo_agent_session_prepare_thread_state_override_json_value(session, state_patch,
                                                                        &prepared_state_override),
                 0);
    check_int_eq(
        turbo_agent_session_update_thread_state_json_value(session, state_patch, &legacy_state_override),
        0);
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_object_get(
                         turbo_json_object_get(prepared_state_override, "profile"),
                         "settings"),
                     "theme")),
                 "dark");
    check_str_eq(turbo_runtime_json_value_as_string(turbo_json_object_get(
                     turbo_json_object_get(
                         turbo_json_object_get(legacy_state_override, "profile"),
                         "settings"),
                     "theme")),
                 "dark");
    turbo_runtime_json_destroy(thread_state);
    thread_state = NULL;
    check_int_eq(turbo_agent_session_get_thread_state_json_value(session, &thread_state), 0);
    check_null(turbo_json_object_get(thread_state, "profile"));

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("approve_review")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_agent_session_apply_thread_command_json_value(session, command, &override), 0);
    override_json = turbo_json_clone(override);
    check_not_null(override_json);
    check_true(turbo_agent_state_review_approved(override_json));
    turbo_free_json(&override_json);
    turbo_runtime_json_destroy(command);
    command = NULL;

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("request_replan")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "reason",
                     turbo_json_create_string("manual review requested")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_agent_session_prepare_checkpoint_command_override_json_value(
                     session, turbo_agent_session_last_checkpoint_id(session), command,
                     &prepared_command_override),
                 0);
    override_json = turbo_json_clone(prepared_command_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_replan_requested(override_json));
    check_str_eq(turbo_agent_state_replan_reason(override_json), "manual review requested");
    turbo_free_json(&override_json);
    turbo_runtime_json_destroy(command);
    command = NULL;
    turbo_runtime_json_destroy(checkpoint_state);
    checkpoint_state = NULL;
    check_int_eq(turbo_agent_session_get_checkpoint_state_json_value(session, NULL, &checkpoint_state),
                 0);
    override_json = turbo_json_clone(checkpoint_state);
    check_not_null(override_json);
    check_false(turbo_agent_state_replan_requested(override_json));
    turbo_free_json(&override_json);
    override_json = NULL;

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("reject_review")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "reason",
                     turbo_json_create_string("needs another pass")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_agent_session_apply_thread_command_json_value(session, command, &rejected_override),
                 0);
    override_json = turbo_json_clone(rejected_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_review_required(override_json));
    check_false(turbo_agent_state_review_approved(override_json));
    check_str_eq(turbo_agent_state_review_note(override_json), "needs another pass");
    turbo_free_json(&override_json);
    turbo_runtime_json_destroy(command);
    command = NULL;

    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("approve_review")),
                 TURBO_RUNTIME_JSON_OK);
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_session_resume_thread_command_json_value(session, graph, command, &options,
                                                                &summary2, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");
    turbo_runtime_json_destroy(command);
    command = NULL;

    turbo_runtime_json_destroy(resumed_state);
    resumed_state = NULL;
    turbo_runtime_json_destroy(run_state);
    run_state = NULL;
    check_int_eq(turbo_agent_session_get_run_state_json_value(session, NULL, &run_state), 0);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(run_state, "visited_end"), 0));

    turbo_runtime_json_destroy(override);
    override = NULL;
    command = turbo_json_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "kind",
                     turbo_json_create_string("request_replan")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_runtime_json_object_set(
                     command, "reason",
                     turbo_json_create_string("manual review requested")),
                 TURBO_RUNTIME_JSON_OK);
    check_int_eq(turbo_agent_session_apply_checkpoint_command_json_value(
                     session, turbo_agent_session_last_checkpoint_id(session), command, &override),
                 0);
    override_json = turbo_json_clone(override);
    check_not_null(override_json);
    check_true(turbo_agent_state_replan_requested(override_json));
    check_str_eq(turbo_agent_state_replan_reason(override_json), "manual review requested");

    turbo_free_json(&override_json);
    turbo_runtime_json_destroy(prepared_command_override);
    turbo_runtime_json_destroy(legacy_state_override);
    turbo_runtime_json_destroy(prepared_state_override);
    turbo_runtime_json_destroy(state_patch);
    turbo_runtime_json_destroy(override);
    turbo_runtime_json_destroy(rejected_override);
    turbo_runtime_json_destroy(command);
    turbo_runtime_json_destroy(checkpoint_state);
    turbo_runtime_json_destroy(run_state);
    turbo_runtime_json_destroy(thread_head_trace_events);
    turbo_runtime_json_destroy(thread_trace_events);
    turbo_runtime_json_destroy(thread_head_state);
    turbo_runtime_json_destroy(thread_state);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should query child run and checkpoint records through parent tool-result output items") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;
    json_value_t *output_item = turbo_json_create_object();
    json_value_t *run_json = NULL;
    json_value_t *checkpoint_json = NULL;
    json_value_t *checkpoint_context = NULL;
    json_value_t *checkpoints_json = NULL;
    json_value_t *inspect_json = NULL;
    json_value_t *orchestration_inspect = NULL;
    json_value_t *multi_agent_inspect = NULL;
    json_value_t *child_branch_tree = NULL;
    json_value_t *inspect_timeline = NULL;
    json_value_t *child_timeline = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(output_item);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_session_last_run_id(session));
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    turbo_json_object_set_string(output_item, "child_run_id",
                                 turbo_agent_session_last_run_id(session));
    turbo_json_object_set_string(output_item, "child_checkpoint_id",
                                 turbo_agent_session_last_checkpoint_id(session));
    turbo_json_object_set_string(output_item, "child_thread_id",
                                 turbo_agent_session_thread_id(session));
    turbo_json_object_set_string(output_item, "parent_agent_run_id", "run_parent");
    turbo_json_object_set_string(output_item, "parent_tool_call_id", "call_parent");
    turbo_json_object_set_string(output_item, "parent_tool_name", "delegate");
    turbo_json_object_set_string(output_item, "parent_graph_run_id", "run_graph_parent");
    turbo_json_object_set_string(output_item, "call_frame_id", "frame_parent");

    check_int_eq(turbo_agent_session_get_child_run(session, output_item, &run_json), 0);
    check_int_eq(turbo_agent_session_get_child_checkpoint(session, output_item, &checkpoint_json),
                 0);
    check_int_eq(
        turbo_agent_session_get_child_checkpoint_context(session, output_item, &checkpoint_context),
        0);
    check_int_eq(
        turbo_agent_session_get_child_thread_timeline_json_value(session, output_item, &child_timeline),
        0);
    check_int_eq(turbo_agent_session_get_child_branch_tree(session, output_item, &child_branch_tree),
                 0);
    check_int_eq(
        turbo_agent_session_list_child_checkpoints(session, output_item, &checkpoints_json), 0);
    check_int_eq(turbo_agent_session_get_child_inspect(session, output_item, &inspect_json), 0);
    check_int_eq(turbo_agent_session_get_child_orchestration_inspect(session, output_item,
                                                                     &orchestration_inspect),
                 0);
    check_int_eq(turbo_agent_session_get_child_multi_agent_inspect(session, output_item,
                                                                   &multi_agent_inspect),
                 0);
    check_str_eq(turbo_json_get_string(run_json, "id"),
                 turbo_agent_session_last_run_id(session));
    check_str_eq(turbo_json_get_string(checkpoint_json, "id"),
                 turbo_agent_session_last_checkpoint_id(session));
    check_size_eq(turbo_json_array_size(checkpoints_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(checkpoints_json, 0), "id"),
                 turbo_agent_session_last_checkpoint_id(session));
    session_check_checkpoint_context(checkpoint_context, turbo_agent_session_thread_id(session),
                                     turbo_agent_session_last_run_id(session),
                                     turbo_agent_session_last_checkpoint_id(session), NULL, 1);
    session_check_thread_timeline_json_value(child_timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session),
                                       turbo_agent_session_last_checkpoint_id(session), 1, 1, 1,
                                       1);
    session_check_branch_tree(child_branch_tree, turbo_agent_session_thread_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_checkpoint_id(session), 1, 0);
    check_not_null(inspect_json);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(inspect_json, "run"), "id"),
                 turbo_agent_session_last_run_id(session));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(inspect_json, "checkpoints")), 1);
    check_str_eq(
        turbo_json_get_string(turbo_json_object_get(inspect_json, "latest_checkpoint"), "id"),
        turbo_agent_session_last_checkpoint_id(session));
    session_check_checkpoint_context(turbo_json_object_get(inspect_json, "checkpoint_context"),
                                     turbo_agent_session_thread_id(session),
                                     turbo_agent_session_last_run_id(session),
                                     turbo_agent_session_last_checkpoint_id(session), NULL, 1);
    check_true(turbo_json_array_size(turbo_json_object_get(inspect_json, "history_events")) >= 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(inspect_json, "trace_events")), 0);
    inspect_timeline = turbo_json_clone(
        turbo_json_object_get(inspect_json, "thread_timeline"));
    check_not_null(inspect_timeline);
    session_check_thread_timeline_json_value(inspect_timeline, turbo_agent_session_thread_id(session),
                                       turbo_agent_session_last_run_id(session),
                                       turbo_agent_session_last_checkpoint_id(session), 1, 1, 1,
                                       1);
    session_check_branch_tree(turbo_json_object_get(inspect_json, "branch_tree"),
                              turbo_agent_session_thread_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_run_id(session),
                              turbo_agent_session_last_checkpoint_id(session), 1, 0);
    check_not_null(orchestration_inspect);
    check_str_eq(turbo_json_get_string(orchestration_inspect, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(orchestration_inspect, "parent_tool_call_id"),
                 "call_parent");
    check_str_eq(turbo_json_get_string(orchestration_inspect, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(orchestration_inspect, "parent_graph_run_id"),
                 "run_graph_parent");
    check_str_eq(turbo_json_get_string(orchestration_inspect, "call_frame_id"), "frame_parent");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(
                     turbo_json_object_get(orchestration_inspect, "child_inspect"), "run"),
                     "id"),
                 turbo_agent_session_last_run_id(session));
    check_not_null(multi_agent_inspect);
    check_not_null(turbo_json_object_get(multi_agent_inspect, "supervisor_inspect"));
    check_not_null(turbo_json_object_get(multi_agent_inspect, "orchestration_inspect"));
    check_not_null(turbo_json_object_get(multi_agent_inspect, "child_orchestration_inspect"));
    check_str_eq(turbo_json_get_string(
                     turbo_json_object_get(multi_agent_inspect, "child_orchestration_inspect"),
                     "parent_tool_name"),
                 "delegate");
    check_str_eq(turbo_json_get_string(
                     turbo_json_object_get(multi_agent_inspect, "child_orchestration_inspect"),
                     "call_frame_id"),
                 "frame_parent");

    turbo_free_json(&multi_agent_inspect);
    turbo_runtime_json_destroy(inspect_timeline);
    turbo_runtime_json_destroy(child_timeline);
    turbo_free_json(&orchestration_inspect);
    turbo_free_json(&child_branch_tree);
    turbo_free_json(&inspect_json);
    turbo_free_json(&checkpoints_json);
    turbo_free_json(&checkpoint_context);
    turbo_free_json(&checkpoint_json);
    turbo_free_json(&run_json);
    turbo_free_json(&output_item);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should keep multi-agent inspect golden fixtures parseable") {
    json_value_t *child_orchestration =
        turbo_agent_test_load_fixture_json("child_orchestration_inspect.golden.json");
    json_value_t *child_multi_agent =
        turbo_agent_test_load_fixture_json("child_multi_agent_inspect.golden.json");

    check_not_null(child_orchestration);
    check_not_null(child_multi_agent);

    turbo_agent_test_check_child_orchestration_inspect(
        child_orchestration, "run_parent", "call_parent", "delegate", "thr_123", "run_123",
        "ckpt_123", 1);
    turbo_agent_test_check_child_multi_agent_inspect(
        child_multi_agent, "thr_123", "run_123", "ckpt_123", "run_parent", "call_parent",
        "delegate");

    turbo_free_json(&child_multi_agent);
    turbo_free_json(&child_orchestration);
  }

  it("should load child history events through parent tool-result output items") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(0);
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    json_value_t *summary = NULL;
    json_value_t *result_state = NULL;
    json_value_t *output_item = turbo_json_create_object();
    json_value_t *events = NULL;
    json_value_t *trace_events = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(output_item);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, &options, &summary,
                                                      &result_state),
                 0);
    check_not_null(turbo_agent_session_last_run_id(session));
    check_not_null(turbo_agent_session_last_checkpoint_id(session));

    turbo_json_object_set_string(output_item, "child_run_id",
                                 turbo_agent_session_last_run_id(session));
    turbo_json_object_set_string(output_item, "child_checkpoint_id",
                                 turbo_agent_session_last_checkpoint_id(session));

    check_int_eq(
        turbo_agent_session_load_child_history_events_json_value(session, output_item, &events), 0);
    check_true(turbo_runtime_json_value_size(events) >= 1);
    check_int_eq(turbo_agent_session_get_child_trace_events_json_value(session, output_item, &trace_events),
                 0);
    check_size_eq(turbo_runtime_json_value_size(trace_events), 0);

    turbo_runtime_json_destroy(trace_events);
    turbo_runtime_json_destroy(events);
    turbo_free_json(&output_item);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should list child runs through configured parent lineage defaults") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(1);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *runs_json = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    config.parent_agent_run_id = "run_parent";
    config.parent_tool_call_id = "call_parent";
    config.parent_tool_name = "delegate";
    config.parent_graph_run_id = "run_graph_parent";
    config.call_frame_id = "frame_parent";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, NULL, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(summary, "parent_graph_run_id"), "run_graph_parent");
    check_str_eq(turbo_json_get_string(summary, "call_frame_id"), "frame_parent");
    check_int_eq(turbo_agent_session_list_child_runs(session, NULL, &runs_json), 0);
    check_size_eq(turbo_json_array_size(runs_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "id"),
                 turbo_agent_session_last_run_id(session));
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "parent_tool_call_id"),
                 "call_parent");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "parent_graph_run_id"),
                 "run_graph_parent");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "call_frame_id"),
                 "frame_parent");

    turbo_free_json(&runs_json);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should expose nullable parent lineage fields on summaries without parent context") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(1);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, NULL, &summary,
                                                      &result_state),
                 0);
    check_true(turbo_json_object_get(summary, "parent_agent_run_id") != NULL);
    check_true(turbo_json_object_get(summary, "parent_tool_call_id") != NULL);
    check_true(turbo_json_object_get(summary, "parent_tool_name") != NULL);
    check_true(turbo_json_object_get(summary, "parent_graph_run_id") != NULL);
    check_true(turbo_json_object_get(summary, "call_frame_id") != NULL);
    check_null(turbo_json_get_string(summary, "parent_agent_run_id"));
    check_null(turbo_json_get_string(summary, "parent_tool_call_id"));
    check_null(turbo_json_get_string(summary, "parent_tool_name"));
    check_null(turbo_json_get_string(summary, "parent_graph_run_id"));
    check_null(turbo_json_get_string(summary, "call_frame_id"));

    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }

  it("should list child runs through current execution context defaults") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_t *session;
    turbo_graph_t *graph = create_session_review_graph();
    json_value_t *state = create_session_review_state(1);
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *runs_json = NULL;
    turbo_agent_execution_context_t saved_context = {0};
    turbo_agent_execution_context_t current_context = {0};

    config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(graph);
    check_not_null(state);

    turbo_agent_execution_context_get(&saved_context);
    current_context = saved_context;
    current_context.run_id = "run_parent_auto";
    current_context.tool_call_id = "call_parent_auto";
    current_context.tool_name = "delegate";
    turbo_agent_execution_context_set(&current_context);

    check_int_eq(turbo_agent_session_start_json_value_graph(session, graph, state, NULL, &summary,
                                                      &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "parent_agent_run_id"), "run_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_call_id"), "call_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(summary, "parent_graph_run_id"), "run_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "call_frame_id"), "call_parent_auto");
    check_int_eq(turbo_agent_session_list_child_runs(session, NULL, &runs_json), 0);
    check_size_eq(turbo_json_array_size(runs_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "id"),
                 turbo_agent_session_last_run_id(session));
    check_str_eq(
        turbo_json_get_string(turbo_json_array_get(runs_json, 0), "parent_agent_run_id"),
        "run_parent_auto");
    check_str_eq(
        turbo_json_get_string(turbo_json_array_get(runs_json, 0), "parent_graph_run_id"),
        "run_parent_auto");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs_json, 0), "call_frame_id"),
                 "call_parent_auto");

    turbo_agent_execution_context_set(&saved_context);
    turbo_free_json(&runs_json);
    turbo_free_json(&summary);
    turbo_runtime_json_destroy(result_state);
    turbo_runtime_json_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
  }
}
