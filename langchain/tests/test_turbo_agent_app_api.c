#include "tinytest.h"
#include "turbo_agent_test_support.h"

#include "turbo_agent_app.h"
#include "turbo_agent.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_session.h"
#include "turbo_agent_subagent.h"
#include "turbo_agent_state.h"
#include "turbo_agent_workflow.h"
#include "turbo_retriever.h"
#include "turbo_runnable.h"
#include "turbo_tool_registry.h"
#include "../src/turbo_agent_runtime_internal.h"
#include "turbo_graph.h"
#include "turbo_parser.h"
#include "turbo_runtime_data_bind.h"

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

#define app_find_command_descriptor turbo_agent_test_find_command_descriptor
#define app_check_string_array_contains_all turbo_agent_test_check_string_array_contains_all
#define app_check_thread_timeline_bind turbo_agent_test_check_thread_timeline_bind
#define app_check_lineage_string_field turbo_agent_test_check_lineage_string_field
#define app_find_lineage_branch turbo_agent_test_find_lineage_branch
#define app_check_lineage_branch turbo_agent_test_check_lineage_branch
#define app_check_thread_lineage_bind turbo_agent_test_check_thread_lineage_bind
#define app_check_checkpoint_context turbo_agent_test_check_checkpoint_context
#define app_check_command_descriptor_example turbo_agent_test_check_command_descriptor_example
#define app_check_command_descriptor_fixture turbo_agent_test_check_command_descriptor_fixture
#define app_check_memory_record_fixture turbo_agent_test_check_memory_record_fixture
#define app_check_memory_record_array_fixture turbo_agent_test_check_memory_record_array_fixture
#define app_find_branch_tree_branch turbo_agent_test_find_branch_tree_branch
#define app_find_branch_tree_edge turbo_agent_test_find_branch_tree_edge
#define app_check_branch_tree_branch turbo_agent_test_check_branch_tree_branch
#define app_check_branch_tree_edge turbo_agent_test_check_branch_tree_edge
#define app_check_branch_tree turbo_agent_test_check_branch_tree
#define app_replay_capture_t turbo_agent_test_replay_capture_t
#define app_capture_replayed_history_event turbo_agent_test_capture_replayed_history_event
#define app_observer_capture_t turbo_agent_test_observer_capture_t
#define app_capture_observer_event turbo_agent_test_capture_observer_event
#define app_trace_capture_t turbo_agent_test_trace_capture_t
#define app_capture_trace_event turbo_agent_test_capture_trace_event

CXX_C_API int turbo_agent_app_apply_state_patch_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_app_get_thread_head_state_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_state);
CXX_C_API int turbo_agent_app_get_thread_head_trace_events_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_events);
CXX_C_API int turbo_agent_app_prepare_checkpoint_state_override_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_app_prepare_thread_state_override_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
static int turbo_agent_app_prepare_checkpoint_command_override_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_app_prepare_thread_command_override_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_app_apply_thread_state_patch_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override);
CXX_C_API int turbo_agent_app_resume_thread_state_patch_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state);

static int turbo_agent_app_start_bind_graph_stream(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_start(app, graph, state, options, event_sink, event_sink_user_data,
                                    out_summary_json, out_state);
}

static int turbo_agent_app_resume_bind_graph_stream(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_resume(
      app, graph, state_override, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      event_sink, event_sink_user_data, out_summary_json, out_state);
}

static int turbo_agent_app_fork_bind_graph_stream(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override, const turbo_graph_run_options_t *options,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_fork(
      app, graph, state_override, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      event_sink, event_sink_user_data, out_summary_json, out_state);
}

static int turbo_agent_app_resume_thread_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_resume(
      app, graph, state_override, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_app_fork_checkpoint_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *state_override, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_fork(
      app, graph, state_override, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_OVERRIDE,
          .checkpoint_id = checkpoint_id,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_app_resume_thread_preset_command_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_resume_preset(
      app, turbo_agent_app_workflow_kind(app), command, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_COMMAND,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_app_update_thread_state_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *state_patch,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_app_apply_thread_state_patch_bind(app, state_patch, out_state_override);
}

static int turbo_agent_app_resume_thread_state_bind_graph(
    turbo_agent_app_t *app, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state_patch, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_resume(
      app, graph, state_patch, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_THREAD,
          .input_kind = TURBO_SESSION_INPUT_PATCH,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_app_resume_checkpoint_command_bind(
    turbo_agent_app_t *app, turbo_graph_t *graph, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_exec_resume(
      app, graph, command, options,
      &(turbo_agent_app_exec_options_t){
          .scope = TURBO_SESSION_SCOPE_CHECKPOINT,
          .input_kind = TURBO_SESSION_INPUT_COMMAND,
          .checkpoint_id = checkpoint_id,
      },
      NULL, NULL, out_summary_json, out_state);
}

static int turbo_agent_session_start_bind_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_runtime_data_bind_value_t *state, const turbo_graph_run_options_t *options,
    json_value_t **out_summary_json, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_session_start_graph(session, graph, state, options, NULL, NULL,
                                         out_summary_json, out_state);
}

static int turbo_agent_app_get_thread_timeline_bind(
    turbo_agent_app_t *app, turbo_runtime_data_bind_value_t **out_timeline) {
  return turbo_agent_session_get_thread_timeline_bind(turbo_agent_app_session(app), out_timeline);
}

static int turbo_agent_app_prepare_checkpoint_command_override_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_prepare_checkpoint_command_override_bind(
      turbo_agent_app_session(app), checkpoint_id, command, out_state_override);
}

static int turbo_agent_app_apply_checkpoint_command_bind(
    turbo_agent_app_t *app, const char *checkpoint_id,
    const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_apply_checkpoint_command_bind(turbo_agent_app_session(app),
                                                           checkpoint_id, command,
                                                           out_state_override);
}

static int turbo_agent_app_apply_thread_command_bind(
    turbo_agent_app_t *app, const turbo_runtime_data_bind_value_t *command,
    turbo_runtime_data_bind_value_t **out_state_override) {
  return turbo_agent_session_apply_thread_command_bind(turbo_agent_app_session(app), command,
                                                       out_state_override);
}

static int turbo_agent_app_load_child_history_events_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  return turbo_agent_session_load_child_history_events_bind(turbo_agent_app_session(app),
                                                            output_item, out_events);
}

static int turbo_agent_app_get_child_trace_events_bind(
    turbo_agent_app_t *app, const json_value_t *output_item,
    turbo_runtime_data_bind_value_t **out_events) {
  return turbo_agent_session_get_child_trace_events_bind(turbo_agent_app_session(app), output_item,
                                                         out_events);
}

typedef struct {
  int call_count;
} app_tool_transport_state_t;

typedef struct {
  const char *key;
  int value;
} app_bool_write_t;

typedef struct {
  const char *name;
  const char *theme;
  const char *locale;
} app_patch_state_write_t;

typedef struct {
  int call_count;
  int saw_knowledge_context;
} app_knowledge_transport_state_t;

typedef struct {
  int call_count;
  int saw_retriever_context;
} app_retriever_transport_state_t;

typedef struct {
  int call_count;
} app_retriever_query_state_t;

static char *app_query_only_memory_strdup(const char *text) {
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

static int app_query_only_memory_query(void *user_data, const char *namespace_prefix,
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
    *out_records_json = app_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (key_prefix && strcmp(key_prefix, "con") != 0) {
    *out_records_json = app_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  if (text_substring && strcmp(text_substring, "remember") != 0) {
    *out_records_json = app_query_only_memory_strdup("[]");
    return *out_records_json ? 0 : -1;
  }
  *out_records_json =
      app_query_only_memory_strdup("[{\"id\":\"project/demo::context\","
                                   "\"namespace\":\"project/demo\","
                                   "\"kind\":\"context\",\"key\":\"context\","
                                   "\"text\":\"remember this\","
                                   "\"metadata\":{\"scope\":\"project\","
                                   "\"path\":\"/tmp/notes.md\"},"
                                   "\"created_at\":null}]");
  return *out_records_json ? 0 : -1;
}

static turbo_agent_memory_store_t app_query_only_memory_store_create(void) {
  turbo_agent_memory_store_t store = {0};
  store.query = app_query_only_memory_query;
  return store;
}

static int app_success_transport(const char *request_json, char **out_response_json,
                                 void *user_data) {
  const char *response =
      "{\"id\":\"resp_test\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  char *copy;
  (void)request_json;
  (void)user_data;
  if (!out_response_json) {
    return -1;
  }
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int app_tool_echo_handler(const char *arguments_json, char **out_output,
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

static int app_json_transport(const char *request_json, char **out_response_json,
                              void *user_data) {
  const char *response =
      "{\"id\":\"resp_json\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"ok\\\":true,\\\"value\\\":42}\"}]}]}";
  char *copy;
  (void)request_json;
  (void)user_data;
  if (!out_response_json) {
    return -1;
  }
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int app_review_transport(const char *request_json, char **out_response_json,
                                void *user_data) {
  const char *planner_response =
      "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"say ok\\\"]}\"}]}]}";
  const char *executor_response =
      "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  const char *response;
  char *copy;
  (void)user_data;
  if (!out_response_json) {
    return -1;
  }
  response = (request_json && strstr(request_json, "Execute plan step") != NULL) ? executor_response
                                                                                  : planner_response;
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int app_knowledge_engineering_transport(const char *request_json,
                                               char **out_response_json,
                                               void *user_data) {
  app_knowledge_transport_state_t *state =
      (app_knowledge_transport_state_t *)user_data;
  const char *planner_response =
      "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"answer from local docs\\\"]}\"}]}]}";
  const char *executor_response =
      "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"knowledge app ok\"}]}]}";
  const char *response;
  char *copy;

  if (!state || !out_response_json) {
    return -1;
  }
  state->call_count++;
  if (request_json && strstr(request_json, "app knowledge marker") &&
      strstr(request_json, "local docs")) {
    state->saw_knowledge_context = 1;
  }
  response = request_json && strstr(request_json, "Execute plan step")
                 ? executor_response
                 : planner_response;
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int app_retriever_query(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  app_retriever_query_state_t *state =
      (app_retriever_query_state_t *)user_data;
  json_value_t *results;
  json_value_t *item;

  if (!state || !query || !options || !out_results_json) {
    return -1;
  }
  state->call_count++;
  check_not_null(strstr(query, "retriever planner context"));
  check_str_eq(options->kind, "note");
  check_size_eq(options->limit, 2);

  results = turbo_json_create_array();
  item = turbo_json_create_object();
  if (!results || !item) {
    turbo_free_json(&results);
    turbo_free_json(&item);
    return -1;
  }
  turbo_json_object_set_string(item, "document_id", "app-retriever");
  turbo_json_object_set_string(item, "uri", "memory://app-retriever");
  turbo_json_object_set_string(
      item, "text", "app retriever marker from generic retriever context");
  turbo_json_array_add(results, item);
  *out_results_json = results;
  return 0;
}

static int app_retriever_engineering_transport(const char *request_json,
                                               char **out_response_json,
                                               void *user_data) {
  app_retriever_transport_state_t *state =
      (app_retriever_transport_state_t *)user_data;
  const char *planner_response =
      "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"answer from retriever\\\"]}\"}]}]}";
  const char *executor_response =
      "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"retriever app ok\"}]}]}";
  const char *response;
  char *copy;

  if (!state || !out_response_json) {
    return -1;
  }
  state->call_count++;
  if (request_json && strstr(request_json, "app retriever marker") &&
      strstr(request_json, "generic retriever context")) {
    state->saw_retriever_context = 1;
  }
  response = request_json && strstr(request_json, "Execute plan step")
                 ? executor_response
                 : planner_response;
  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static int app_parent_tool_transport(const char *request_json, char **out_response_json,
                                     void *user_data) {
  app_tool_transport_state_t *state = (app_tool_transport_state_t *)user_data;
  const char *response;
  char *copy;

  (void)request_json;
  if (!state || !out_response_json) {
    return -1;
  }

  state->call_count++;
  if (state->call_count == 1) {
    response =
        "{\"id\":\"resp_parent_tool_1\",\"output\":[{\"type\":\"function_call\","
        "\"call_id\":\"call_parent_1\",\"name\":\"delegate\","
        "\"arguments\":\"{\\\"input\\\":\\\"child task\\\"}\"}]}";
  } else {
    response =
        "{\"id\":\"resp_parent_tool_2\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"parent done\"}]}]}";
  }

  copy = (char *)malloc(strlen(response) + 1);
  if (!copy) {
    return -1;
  }
  memcpy(copy, response, strlen(response) + 1);
  *out_response_json = copy;
  return 0;
}

static char *create_app_temp_root(void) {
  int needed;
  char *path;
  unsigned long long tick = (unsigned long long)turbo_hrtime();

#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  needed = snprintf(NULL, 0, "%sturbonet_app_%llx", temp_dir, tick);
#else
  const char *temp_dir = "/tmp/";
  needed = snprintf(NULL, 0, "%sturbonet_app_%llx", temp_dir, tick);
#endif
  check_true(needed > 0);
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
#ifdef _WIN32
  snprintf(path, (size_t)needed + 1, "%sturbonet_app_%llx", temp_dir, tick);
  _mkdir(path);
#else
  snprintf(path, (size_t)needed + 1, "%sturbonet_app_%llx", temp_dir, tick);
  mkdir(path, 0700);
#endif
  return path;
}

static char *create_app_temp_path(const char *root, const char *name) {
  const char *sep;
  int needed;
  char *path;

  check_not_null(root);
  check_not_null(name);
#ifdef _WIN32
  sep = "\\";
#else
  sep = "/";
#endif
  needed = snprintf(NULL, 0, "%s%s%s", root, sep, name);
  check_true(needed > 0);
  path = (char *)malloc((size_t)needed + 1);
  check_not_null(path);
  snprintf(path, (size_t)needed + 1, "%s%s%s", root, sep, name);
  return path;
}

static turbo_runtime_data_bind_value_t *app_create_messages_bind(void) {
  turbo_runtime_data_bind_value_t *messages = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *system_message = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *user_message = turbo_runtime_data_bind_value_create_object();

  if (!messages || !system_message || !user_message) {
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_runtime_data_bind_value_destroy(system_message);
    turbo_runtime_data_bind_value_destroy(user_message);
    return NULL;
  }
  check_int_eq(turbo_runtime_data_bind_object_set(
                   system_message, "role", turbo_runtime_data_bind_value_create_string("system")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   system_message, "content",
                   turbo_runtime_data_bind_value_create_string("Be terse.")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   user_message, "role", turbo_runtime_data_bind_value_create_string("user")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   user_message, "content", turbo_runtime_data_bind_value_create_string("hello")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(messages, system_message),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(messages, user_message),
               TURBO_RUNTIME_DATA_BIND_OK);
  return messages;
}

static int app_write_bool_bind_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  app_bool_write_t *write = (app_bool_write_t *)user_data;
  turbo_runtime_data_bind_value_t *value =
      turbo_runtime_data_bind_value_create_bool(write->value);

  check_not_null(value);
  return turbo_runtime_data_bind_object_set(ctx->bind_state, write->key, value) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static int app_supervisor_planner_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *reason = (const char *)user_data;

  if (!ctx || !ctx->state || !reason) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_planner", true);
  return turbo_agent_state_request_handoff(ctx->state, "executor", reason);
}

static int app_supervisor_executor_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *final_output = (const char *)user_data;

  if (!ctx || !ctx->state || !final_output) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_executor", true);
  return turbo_agent_state_set_final_answer(ctx->state, final_output);
}

static turbo_graph_t *create_app_review_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("app-review");
  static app_bool_write_t start = {"visited_start", 1};
  static app_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(turbo_graph_add_bind_node(graph, "start", app_write_bool_bind_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_node(graph, "end", app_write_bool_bind_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_graph_t *create_app_supervisor_handoff_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("app-supervisor-handoff");
  static const char *handoff_reason = "delegate execution";
  static const char *final_output = "executor finished";

  check_not_null(graph);
  check_int_eq(turbo_agent_install_supervisor_loop(graph, "supervisor", "handoff", "end", 1),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "planner", app_supervisor_planner_node,
                                    (void *)handoff_reason),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "executor", app_supervisor_executor_node,
                                    (void *)final_output),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "planner", "handoff", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "executor", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_runtime_data_bind_value_t *create_app_review_state(int approved) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_request_review(state, "need approval"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *create_app_supervisor_handoff_state(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *create_app_supervisor_review_state(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
  check_int_eq(turbo_agent_state_request_review(state, "need approval"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *
create_app_override_from_result(const turbo_runtime_data_bind_value_t *result_state, int approved) {
  json_value_t *state = turbo_runtime_data_bind_value_to_json(result_state);
  turbo_runtime_data_bind_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_review_approved(state, approved), 0);
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static int app_patch_state_write_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  app_patch_state_write_t *write = (app_patch_state_write_t *)user_data;
  turbo_runtime_data_bind_value_t *profile = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *settings = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *labels = turbo_runtime_data_bind_value_create_array();

  (void)write;
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(labels);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   ctx->bind_state, "visited_start",
                   turbo_runtime_data_bind_value_create_bool(1)),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   profile, "name", turbo_runtime_data_bind_value_create_string("alpha")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "theme", turbo_runtime_data_bind_value_create_string("light")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "locale", turbo_runtime_data_bind_value_create_string("en")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "notes", turbo_runtime_data_bind_value_create_string("keep")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "settings", settings),
               TURBO_RUNTIME_DATA_BIND_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_data_bind_array_append(
                   labels, turbo_runtime_data_bind_value_create_string("seed")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "labels", labels),
               TURBO_RUNTIME_DATA_BIND_OK);
  labels = NULL;
  return turbo_runtime_data_bind_object_set(ctx->bind_state, "profile", profile) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static turbo_graph_t *create_app_state_patch_graph(void) {
  static app_patch_state_write_t start = {"alpha", "light", "en"};
  static app_bool_write_t end = {"visited_end", 1};
  turbo_graph_t *graph = turbo_graph_create("app-state-patch");

  check_not_null(graph);
  check_int_eq(turbo_graph_add_bind_node(graph, "start", app_patch_state_write_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_node(graph, "end", app_write_bool_bind_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static turbo_runtime_data_bind_value_t *create_app_state_patch_bind(void) {
  turbo_runtime_data_bind_value_t *patch = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *profile = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *settings = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *flags = turbo_runtime_data_bind_value_create_array();

  check_not_null(patch);
  check_not_null(profile);
  check_not_null(settings);
  check_not_null(flags);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "theme", turbo_runtime_data_bind_value_create_string("dark")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(
                   settings, "extra", turbo_runtime_data_bind_value_create_string("enabled")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(settings, "notes",
                                                  turbo_runtime_data_bind_value_create_null()),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   flags, turbo_runtime_data_bind_value_create_string("flag-a")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_array_append(
                   flags, turbo_runtime_data_bind_value_create_string("flag-b")),
               TURBO_RUNTIME_DATA_BIND_OK);
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "settings", settings),
               TURBO_RUNTIME_DATA_BIND_OK);
  settings = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(profile, "flags", flags),
               TURBO_RUNTIME_DATA_BIND_OK);
  flags = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(patch, "profile", profile),
               TURBO_RUNTIME_DATA_BIND_OK);
  profile = NULL;
  check_int_eq(turbo_runtime_data_bind_object_set(
                   patch, "patch_version", turbo_runtime_data_bind_value_create_int64(3)),
               TURBO_RUNTIME_DATA_BIND_OK);
  return patch;
}

spec("turbo agent app api") {

  it("should create one app and invoke text through session defaults") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(turbo_agent_app_session(app));

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(text, "ok");

    free(text);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should invoke app defaults without summary outputs") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runtime_data_bind_value_t *messages = app_create_messages_bind();
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(messages);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, NULL), 0);
    check_str_eq(text, "ok");
    check_not_null(turbo_agent_app_thread_id(app));
    check_not_null(turbo_agent_app_last_run_id(app));
    free(text);
    text = NULL;

    check_int_eq(turbo_agent_app_invoke_messages_text(app, messages, NULL, &text, NULL), 0);
    check_str_eq(text, "ok");

    free(text);
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_agent_app_destroy(app);
  }

  it("should expose the configured tool registry through app accessors") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};

    check_not_null(registry);
    definition.name = "app.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.handler = app_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_ptr_eq(turbo_agent_session_tool_registry(turbo_agent_app_session(app)), registry);
    check_size_eq(turbo_agent_session_tool_count(turbo_agent_app_session(app)), 1);
    check_ptr_eq(turbo_agent_app_tool_registry(app), registry);
    check_size_eq(turbo_agent_app_tool_count(app), 1);

    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("should report configured harness capabilities through app diagnostics") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    json_value_t *capabilities = NULL;

    check_not_null(registry);
    definition.name = "app.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.handler = app_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.api_key = "test-key";
    agent_config.base_url = "https://example.invalid/v1";
    agent_config.transport_fn = app_success_transport;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = turbo_agent_memory_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    session_config.memory_namespace = "project";
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_get_capabilities(app, &capabilities), 0);
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
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("should report provider tool schemas through app diagnostics") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    json_value_t *schemas = NULL;
    json_value_t *openai_compatible_chat;
    json_value_t *function_object;

    check_not_null(registry);
    definition.name = "app.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.strict = 1;
    definition.handler = app_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_get_tool_schemas(app, &schemas), 0);
    check_not_null(schemas);
    check_true(turbo_json_get_bool(schemas, "has_tool_registry", false));
    check_int_eq(turbo_json_get_int(schemas, "tool_count", -1), 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "registry")), 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "openai_responses")),
                  1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "openai_chat")), 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(schemas, "anthropic")), 1);
    openai_compatible_chat = turbo_json_object_get(schemas, "openai_compatible_chat");
    check_size_eq(turbo_json_array_size(openai_compatible_chat), 1);
    function_object =
        turbo_json_object_get(turbo_json_array_get(openai_compatible_chat, 0), "function");
    check_str_eq(turbo_json_get_string(function_object, "name"), "app_echo");

    turbo_free_json(&schemas);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("should report startup diagnostics through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    json_value_t *diagnostics = NULL;
    json_value_t *errors;
    json_value_t *capabilities;
    json_value_t *tool_schemas;
    json_value_t *workflow;
    json_value_t *structured_output;
    json_value_t *tool_schema_status;

    check_not_null(registry);
    definition.name = "app.echo";
    definition.description = "Echo one fixed test result.";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
    definition.strict = 1;
    definition.handler = app_tool_echo_handler;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_get_startup_diagnostics(app, &diagnostics), 0);
    check_not_null(diagnostics);
    check_int_eq(turbo_json_get_int(diagnostics, "schema_version", -1), 1);
    check_true(turbo_json_get_bool(diagnostics, "ok", false));
    errors = turbo_json_object_get(diagnostics, "errors");
    check_size_eq(turbo_json_array_size(errors), 0);
    capabilities = turbo_json_object_get(diagnostics, "capabilities");
    tool_schemas = turbo_json_object_get(diagnostics, "tool_schemas");
    workflow = turbo_json_object_get(diagnostics, "workflow");
    structured_output = turbo_json_object_get(diagnostics, "structured_output");
    tool_schema_status = turbo_json_object_get(diagnostics, "tool_schema_status");
    check_not_null(capabilities);
    check_not_null(tool_schemas);
    check_not_null(workflow);
    check_not_null(structured_output);
    check_not_null(tool_schema_status);
    check_int_eq(turbo_json_get_int(capabilities, "tool_count", -1), 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(tool_schemas, "registry")), 1);
    check_true(turbo_json_get_bool(workflow, "ok", false));
    check_false(turbo_json_get_bool(structured_output, "configured", true));
    check_str_eq(turbo_json_get_string(tool_schema_status, "selected_format"),
                 "openai_responses");

    turbo_free_json(&diagnostics);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("should stream user text through app defaults by mode") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runtime_data_bind_value_t *state = NULL;
    json_value_t *summary = NULL;
    app_replay_capture_t capture = {0};
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_start_text_stream(
                     app, "hello", NULL, TURBO_EVENT_STREAM_MESSAGES,
                     app_capture_replayed_history_event, &capture, &summary, &state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(capture.model_count >= 1);
    check_size_eq(capture.trace_count, 0);
    check_size_eq(capture.tool_result_count, 0);
    text = turbo_agent_app_result_text(state);
    check_str_eq(text, "ok");

    free(text);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should batch user text through app defaults") {
    const char *inputs[] = {"hello one", "hello two"};
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *results = NULL;
    const json_value_t *first;
    const json_value_t *second;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_batch_text(app, inputs, 2, NULL, &results), 0);
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
    turbo_agent_app_destroy(app);
  }

  it("should expose app defaults as a runnable") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runnable_t *runnable;
    turbo_runtime_data_bind_value_t *input = turbo_runtime_data_bind_value_create_object();
    turbo_runtime_data_bind_value_t *output = NULL;
    turbo_runtime_data_bind_value_t *batch_inputs = turbo_runtime_data_bind_value_create_array();
    turbo_runtime_data_bind_value_t *batch_outputs = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(input);
    check_not_null(batch_inputs);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     input, "input", turbo_runtime_data_bind_value_create_string("hello")),
                 TURBO_RUNTIME_DATA_BIND_OK);

    runnable = turbo_runnable_from_agent_app(app, NULL);
    check_not_null(runnable);
    check_int_eq(turbo_runnable_invoke_bind(runnable, input, &output), 0);
    check_not_null(output);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(output, "output_text")),
                 "ok");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_object_get(output, "summary"), "status")),
                 "completed");
    check_not_null(turbo_runtime_data_bind_object_get(output, "state"));

    check_int_eq(turbo_runtime_data_bind_array_append(
                     batch_inputs, turbo_runtime_data_bind_value_create_string("first")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_array_append(
                     batch_inputs, turbo_runtime_data_bind_value_create_string("second")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runnable_batch_bind(runnable, batch_inputs, &batch_outputs), 0);
    check_size_eq(turbo_runtime_data_bind_value_size(batch_outputs), 2);
    check_str_eq(turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(
                     turbo_runtime_data_bind_array_get(batch_outputs, 0), "output_text")),
                 "ok");
    check_str_eq(turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(
                     turbo_runtime_data_bind_array_get(batch_outputs, 1), "output_text")),
                 "ok");

    turbo_runtime_data_bind_value_destroy(batch_outputs);
    turbo_runtime_data_bind_value_destroy(batch_inputs);
    turbo_runtime_data_bind_value_destroy(output);
    turbo_runtime_data_bind_value_destroy(input);
    turbo_runnable_destroy(runnable);
    turbo_agent_app_destroy(app);
  }

  it("should create one app and invoke parsed json through session defaults") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_json_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_invoke_json(app, "hello", NULL, &result, &summary), 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);

    turbo_free_json(&result);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should invoke parsed json through app defaults without summary outputs") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runtime_data_bind_value_t *messages = app_create_messages_bind();
    json_value_t *result = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_json_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(messages);

    check_int_eq(turbo_agent_app_invoke_json(app, "hello", NULL, &result, NULL), 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);
    check_not_null(turbo_agent_app_thread_id(app));
    check_not_null(turbo_agent_app_last_run_id(app));
    turbo_free_json(&result);

    check_int_eq(turbo_agent_app_invoke_messages_json(app, messages, NULL, &result, NULL), 0);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(result, "value", 0), 42);

    turbo_free_json(&result);
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_agent_app_destroy(app);
  }

  it("should invoke canonical messages and query runtime through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runtime_data_bind_value_t *messages = app_create_messages_bind();
    json_value_t *summary = NULL;
    json_value_t *thread_json = NULL;
    json_value_t *run_json = NULL;
    json_value_t *latest_run_json = NULL;
    json_value_t *pending_run_json = NULL;
    json_value_t *latest_checkpoint_json = NULL;
    json_value_t *runs_json = NULL;
    json_value_t *checkpoints_json = NULL;
    turbo_runtime_data_bind_value_t *events = NULL;
    app_replay_capture_t replay = {0};
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(messages);

    check_int_eq(turbo_agent_app_invoke_messages_text(app, messages, NULL, &text, &summary), 0);
    check_str_eq(text, "ok");
    check_int_eq(turbo_agent_app_get_thread(app, &thread_json), 0);
    check_int_eq(turbo_agent_app_get_run(app, NULL, &run_json), 0);
    check_int_eq(turbo_agent_app_get_latest_run(app, &latest_run_json), 0);
    check_int_eq(turbo_agent_app_get_pending_run(app, &pending_run_json), -1);
    check_int_eq(turbo_agent_app_get_latest_checkpoint(app, NULL, &latest_checkpoint_json), -1);
    check_int_eq(turbo_agent_app_list_runs(app, &runs_json), 0);
    check_int_eq(turbo_agent_app_list_checkpoints(app, NULL, &checkpoints_json), 0);
    check_int_eq(turbo_agent_app_load_history_events_bind(app, NULL, NULL, &events), 0);
    turbo_runtime_data_bind_value_destroy(events);
    events = NULL;
    check_int_eq(turbo_agent_app_load_thread_history_events_bind(app, &events), 0);
    check_int_eq(turbo_agent_app_replay_thread_history_bind(
                     app, app_capture_replayed_history_event, &replay),
                 0);
    check_str_eq(turbo_json_get_string(thread_json, "id"), turbo_json_get_string(summary, "thread_id"));
    check_str_eq(turbo_json_get_string(run_json, "id"), turbo_json_get_string(summary, "run_id"));
    check_str_eq(turbo_json_get_string(latest_run_json, "id"), turbo_json_get_string(summary, "run_id"));
    check_size_eq(turbo_json_array_size(runs_json), 1);
    check_size_eq(turbo_json_array_size(checkpoints_json), 0);
    check_size_eq(turbo_runtime_data_bind_value_size(events), 0);
    check_size_eq(replay.count, 0);
    {
      turbo_runtime_data_bind_value_t *timeline = NULL;

      check_int_eq(turbo_agent_app_get_thread_timeline_bind(app, &timeline), 0);
      app_check_thread_timeline_bind(timeline, turbo_json_get_string(summary, "thread_id"),
                                     turbo_json_get_string(summary, "run_id"), NULL, 0, 1, 0, 0);
      turbo_runtime_data_bind_value_destroy(timeline);
    }

    free(text);
    turbo_free_json(&summary);
    turbo_free_json(&thread_json);
    turbo_free_json(&run_json);
    turbo_free_json(&latest_run_json);
    turbo_free_json(&pending_run_json);
    turbo_free_json(&latest_checkpoint_json);
    turbo_free_json(&runs_json);
    turbo_free_json(&checkpoints_json);
    turbo_runtime_data_bind_value_destroy(events);
    turbo_runtime_data_bind_value_destroy(messages);
    turbo_agent_app_destroy(app);
  }

  it("should bridge live trace sinks through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_agent_trace_bind_sink_t sink = {0};
    app_trace_capture_t capture = {0};
    turbo_runtime_data_bind_value_t *thread_trace_events = NULL;
    turbo_runtime_data_bind_value_t *run_trace_events = NULL;
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    sink.callback = app_capture_trace_event;
    sink.user_data = &capture;

    check_int_eq(turbo_agent_app_add_trace_bind_sink(app, &sink), 0);
    check_int_eq(turbo_agent_app_set_trace_history_enabled(app, 1), 0);
    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    check_str_eq(text, "ok");
    check_true(capture.count >= 2);
    check_size_eq(capture.trace_count, capture.count);
    check_true(capture.model_request_count >= 1);
    check_true(capture.model_response_count >= 1);
    check_int_eq(turbo_agent_app_get_thread_trace_events_bind(app, &thread_trace_events), 0);
    check_not_null(thread_trace_events);
    check_true(turbo_runtime_data_bind_value_size(thread_trace_events) >= capture.count);
    check_int_eq(turbo_agent_app_get_run_trace_events_bind(app, NULL, &run_trace_events), 0);
    check_not_null(run_trace_events);
    check_true(turbo_runtime_data_bind_value_size(run_trace_events) >= capture.count);

    turbo_runtime_data_bind_value_destroy(run_trace_events);
    turbo_runtime_data_bind_value_destroy(thread_trace_events);
    free(text);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should bridge live observer sinks through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_agent_observer_bind_sink_t sink = {0};
    app_observer_capture_t capture = {0};
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    sink.callback = app_capture_observer_event;
    sink.user_data = &capture;

    check_int_eq(turbo_agent_app_add_observer_bind_sink(app, &sink), 0);
    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    check_str_eq(text, "ok");
    check_true(capture.count >= 2);
    check_true(capture.model_delta_count >= 2);
    check_size_eq(capture.tool_call_started_count, 0);
    check_size_eq(capture.tool_result_count, 0);

    free(text);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should emit live canonical stream events through app wrappers") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    turbo_runtime_data_bind_value_t *forked_state = NULL;
    turbo_runtime_data_bind_value_t *command = NULL;
    json_value_t *summary = NULL;
    json_value_t *resumed_summary = NULL;
    json_value_t *forked_summary = NULL;
    app_replay_capture_t start_capture = {0};
    app_replay_capture_t resume_capture = {0};
    app_replay_capture_t fork_capture = {0};
    turbo_event_stream_filter_t start_filter = {0};
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    char *first_run_id = NULL;
    char *first_checkpoint_id = NULL;

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    start_filter.mode = TURBO_EVENT_STREAM_DEBUG;
    start_filter.sink = app_capture_replayed_history_event;
    start_filter.sink_user_data = &start_capture;
    check_int_eq(turbo_agent_app_start_bind_graph_stream(
                     app, graph, state, &options, turbo_event_stream_filter_sink_bind,
                     &start_filter, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_true(start_capture.count >= 1);
    check_true(start_capture.trace_count >= 1);
    check_size_eq(start_capture.model_count, 0);
    check_size_eq(start_capture.tool_result_count, 0);
    check_not_null(turbo_agent_app_thread_id(app));
    check_not_null(turbo_agent_app_last_run_id(app));
    check_not_null(turbo_agent_app_last_checkpoint_id(app));
    first_run_id = app_query_only_memory_strdup(turbo_agent_app_last_run_id(app));
    first_checkpoint_id = app_query_only_memory_strdup(turbo_agent_app_last_checkpoint_id(app));
    check_not_null(first_run_id);
    check_not_null(first_checkpoint_id);

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_app_apply_checkpoint_command_bind(app, first_checkpoint_id, command,
                                                               &override),
                 0);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_app_resume_bind_graph_stream(
                     app, graph, first_checkpoint_id, override, &options,
                     app_capture_replayed_history_event, &resume_capture, &resumed_summary,
                     &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(resumed_summary, "status"), "completed");
    check_str_eq(turbo_agent_app_last_run_id(app), first_run_id);
    check_true(resume_capture.count >= 1);
    check_true(resume_capture.trace_count + resume_capture.model_count +
                   resume_capture.tool_result_count >=
               1);

    check_int_eq(turbo_agent_app_fork_bind_graph_stream(
                     app, graph, first_checkpoint_id, override, &options,
                     app_capture_replayed_history_event, &fork_capture, &forked_summary,
                     &forked_state),
                 0);
    check_str_eq(turbo_json_get_string(forked_summary, "status"), "completed");
    check_true(strcmp(turbo_agent_app_last_run_id(app), first_run_id) != 0);
    check_true(fork_capture.count >= 1);
    check_true(fork_capture.trace_count + fork_capture.model_count + fork_capture.tool_result_count >=
               1);

    free(first_checkpoint_id);
    free(first_run_id);
    turbo_free_json(&forked_summary);
    turbo_free_json(&resumed_summary);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(forked_state);
    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_runtime_data_bind_value_destroy(command);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should resume an interrupted thread through the thread-scoped replay wrapper") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *replay_override = NULL;
    turbo_runtime_data_bind_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_review_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_int_eq(turbo_agent_app_get_thread_state_bind(app, &thread_state), 0);
    replay_override = create_app_override_from_result(thread_state, 1);
    check_not_null(replay_override);
    check_int_eq(turbo_agent_app_resume_thread_bind_graph(app, graph, replay_override, NULL,
                                                          &replay_summary, &replay_state),
                 0);
    check_str_eq(turbo_json_get_string(replay_summary, "status"), "completed");
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(replay_state, "visited_end"), 0));

    turbo_free_json(&replay_summary);
    turbo_runtime_data_bind_value_destroy(replay_state);
    turbo_runtime_data_bind_value_destroy(replay_override);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should fork one interrupted checkpoint through the explicit checkpoint-scoped replay alias") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;
    json_value_t *run_json = NULL;
    json_value_t *checkpoint_context = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    const char *checkpoint_id;
    const char *run_id;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_review_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
    run_id = turbo_json_get_string(summary, "run_id");
    check_not_null(checkpoint_id);

    override = create_app_override_from_result(result_state, 1);
    check_not_null(override);
    turbo_runtime_data_bind_value_destroy(result_state);
    result_state = NULL;

    check_int_eq(turbo_agent_app_fork_checkpoint_bind_graph(app, graph, checkpoint_id, override,
                                                             NULL, &replay_summary, &replay_state),
                 0);
    check_str_eq(turbo_json_get_string(replay_summary, "status"), "completed");
    check_true(strcmp(turbo_json_get_string(replay_summary, "run_id"), run_id) != 0);
    check_int_eq(turbo_agent_app_get_run(app, turbo_json_get_string(replay_summary, "run_id"),
                                        &run_json),
                 0);
    check_str_eq(turbo_json_get_string(run_json, "forked_from_checkpoint_id"), checkpoint_id);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(replay_state, "visited_end"), 0));
    check_int_eq(turbo_agent_app_get_checkpoint_context(app, checkpoint_id, &checkpoint_context),
                 0);
    app_check_checkpoint_context(checkpoint_context, turbo_agent_app_thread_id(app), run_id,
                                 checkpoint_id, NULL, 1);

    turbo_free_json(&run_json);
    turbo_free_json(&replay_summary);
    turbo_free_json(&checkpoint_context);
    turbo_runtime_data_bind_value_destroy(replay_state);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should bridge durable history through app observer wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(1);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    app_observer_capture_t thread_capture = {0};
    app_observer_capture_t checkpoint_capture = {0};
    turbo_agent_observer_bind_sink_t thread_sink = {0};
    turbo_agent_observer_bind_sink_t checkpoint_sink = {0};
    const char *interrupt_before_end[] = {"end"};
    turbo_graph_run_options_t options = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_review_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_end;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    thread_sink.callback = app_capture_observer_event;
    thread_sink.user_data = &thread_capture;
    checkpoint_sink.callback = app_capture_observer_event;
    checkpoint_sink.user_data = &checkpoint_capture;

    check_int_eq(turbo_agent_app_observe_thread_history_bind(app, &thread_sink), 0);
    check_int_eq(turbo_agent_app_observe_history_bind(app, NULL, NULL, &checkpoint_sink), 0);
    check_true(thread_capture.count >= 1);
    check_true(thread_capture.interrupted_count >= 1);
    check_size_eq(checkpoint_capture.count, thread_capture.count);
    check_size_eq(checkpoint_capture.interrupted_count, thread_capture.interrupted_count);

    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should reject thread-scoped replay on a completed-only thread") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(1);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *run_state = NULL;
    turbo_runtime_data_bind_value_t *replay_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *replay_summary = NULL;

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      NULL, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);
    check_int_eq(turbo_agent_app_get_thread_state_bind(app, &thread_state), 0);
    check_int_eq(turbo_agent_app_get_run_state_bind(app, NULL, &run_state), 0);
    check_true(thread_state != NULL);
    check_true(run_state != NULL);
    check_true(turbo_agent_app_resume_thread_bind_graph(app, graph, thread_state, NULL,
                                                        &replay_summary, &replay_state) != 0);
    check_null(replay_summary);
    check_null(replay_state);

    turbo_runtime_data_bind_value_destroy(run_state);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should resume one configured preset workflow through app command helpers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    turbo_runtime_data_bind_value_t *command = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    json_value_t *pending_run_json = NULL;
    json_value_t *latest_checkpoint_json = NULL;
    turbo_runtime_data_bind_value_t *events = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_review_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_REVIEW;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_app_start_text(app, "hello", &options, &summary, &result_state), 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_str_eq(turbo_json_get_string(summary, "interrupt_reason"), "review_required");
    check_size_eq(
        turbo_json_array_size(turbo_json_object_get(summary, "available_command_descriptors")), 6);
    check_int_eq(turbo_agent_app_get_pending_run(app, &pending_run_json), 0);
    check_str_eq(turbo_json_get_string(pending_run_json, "id"), turbo_json_get_string(summary, "run_id"));
    check_int_eq(turbo_agent_app_get_latest_checkpoint(app, NULL, &latest_checkpoint_json), 0);
    check_str_eq(turbo_json_get_string(latest_checkpoint_json, "id"),
                 turbo_json_get_string(summary, "checkpoint_id"));
    check_int_eq(turbo_agent_app_load_thread_history_events_bind(app, &events), 0);
    check_true(turbo_runtime_data_bind_value_size(events) >= 1);
    {
      turbo_runtime_data_bind_value_t *timeline = NULL;

      check_int_eq(turbo_agent_app_get_thread_timeline_bind(app, &timeline), 0);
      app_check_thread_timeline_bind(timeline, turbo_agent_app_thread_id(app),
                                     turbo_json_get_string(summary, "run_id"),
                                     turbo_json_get_string(summary, "checkpoint_id"), 1, 1, 1, 1);
      turbo_runtime_data_bind_value_destroy(timeline);
    }
    {
      const json_value_t *request_replan_descriptor =
          app_find_command_descriptor(summary, "request_replan");
      const json_value_t *append_feedback_descriptor =
          app_find_command_descriptor(summary, "append_feedback");
      const json_value_t *override_descriptor =
          app_find_command_descriptor(summary, "override_final_output");
      const json_value_t *request_replan_accepted_keys;
      const json_value_t *append_feedback_accepted_keys;
      const json_value_t *override_accepted_keys;
      const char *const request_replan_expected_accepts[] = {"reason"};
      const char *const append_feedback_expected_accepts[] = {"text"};
      const char *const override_expected_accepts[] = {"text", "output_json"};

      check_not_null(request_replan_descriptor);
      check_not_null(append_feedback_descriptor);
      check_not_null(override_descriptor);
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
      check_str_eq(turbo_json_get_string(request_replan_descriptor, "primary_key"), "reason");
      check_str_eq(turbo_json_get_string(request_replan_descriptor, "placeholder"),
                   "Describe why the current plan should be rebuilt.");
      request_replan_accepted_keys =
          turbo_json_object_get(request_replan_descriptor, "accepted_keys");
      check_not_null(request_replan_accepted_keys);
      app_check_string_array_contains_all(request_replan_accepted_keys,
                                          request_replan_expected_accepts,
                                          sizeof(request_replan_expected_accepts) /
                                              sizeof(request_replan_expected_accepts[0]));
      check_false(turbo_json_get_bool(request_replan_descriptor, "supports_json_value", true));
      app_check_command_descriptor_fixture(request_replan_descriptor,
                                           "command_descriptors.golden.json",
                                           "request_replan");
      check_str_eq(turbo_json_get_string(append_feedback_descriptor, "primary_key"), "text");
      append_feedback_accepted_keys =
          turbo_json_object_get(append_feedback_descriptor, "accepted_keys");
      check_not_null(append_feedback_accepted_keys);
      app_check_string_array_contains_all(append_feedback_accepted_keys,
                                          append_feedback_expected_accepts,
                                          sizeof(append_feedback_expected_accepts) /
                                              sizeof(append_feedback_expected_accepts[0]));
      check_false(turbo_json_get_bool(append_feedback_descriptor, "supports_json_value", true));
      check_str_eq(turbo_json_get_string(override_descriptor, "primary_key"), "text");
      override_accepted_keys = turbo_json_object_get(override_descriptor, "accepted_keys");
      check_not_null(override_accepted_keys);
      app_check_string_array_contains_all(override_accepted_keys, override_expected_accepts,
                                          sizeof(override_expected_accepts) /
                                              sizeof(override_expected_accepts[0]));
      check_true(turbo_json_get_bool(override_descriptor, "supports_json_value", false));
      check_true(turbo_json_type(turbo_json_object_get(override_descriptor, "example_payload")) ==
                 TURBO_JSON_OBJECT);
      app_check_command_descriptor_fixture(append_feedback_descriptor,
                                           "command_descriptors.golden.json",
                                           "append_feedback");
      app_check_command_descriptor_fixture(override_descriptor,
                                           "command_descriptors.golden.json",
                                           "override_final_output");
    }

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(
        turbo_agent_app_resume_thread_preset_command_bind(app, command, &options, &summary2,
                                                          &resumed_state),
        0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");

    turbo_runtime_data_bind_value_destroy(command);
    turbo_runtime_data_bind_value_destroy(events);
    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_free_json(&latest_checkpoint_json);
    turbo_free_json(&pending_run_json);
    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should merge bind-native state patches through the thread replay wrapper") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_state_patch_graph();
    turbo_runtime_data_bind_value_t *state = turbo_agent_state_create_bind();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *patched_state = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    turbo_runtime_data_bind_value_t *patch = create_app_state_patch_bind();
    json_value_t *summary = NULL;
    json_value_t *resumed_summary = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};
    const turbo_runtime_data_bind_value_t *profile;
    const turbo_runtime_data_bind_value_t *settings;
    const turbo_runtime_data_bind_value_t *flags;

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(patch);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_app_last_checkpoint_id(app));

    check_int_eq(turbo_agent_app_update_thread_state_bind(app, patch, &patched_state), 0);
    check_not_null(patched_state);
    profile = turbo_runtime_data_bind_object_get(patched_state, "profile");
    settings = profile ? turbo_runtime_data_bind_object_get(profile, "settings") : NULL;
    flags = profile ? turbo_runtime_data_bind_object_get(profile, "flags") : NULL;
    check_not_null(profile);
    check_not_null(settings);
    check_not_null(flags);
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(profile, "name")),
                 "alpha");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "theme")),
                 "dark");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "locale")),
                 "en");
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(settings, "extra")),
                 "enabled");
    check_true(turbo_runtime_data_bind_value_kind(
                   turbo_runtime_data_bind_object_get(settings, "notes")) ==
               TURBO_RUNTIME_DATA_BIND_VALUE_NULL);
    check_size_eq(turbo_runtime_data_bind_value_size(flags), 2);

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    check_int_eq(turbo_agent_app_resume_thread_state_bind_graph(
                     app, graph, patch, &options, &resumed_summary, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(resumed_summary, "status"), "completed");
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(resumed_state, "visited_end"), 0));
    check_str_eq(turbo_runtime_data_bind_value_as_string(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_object_get(
                             turbo_runtime_data_bind_object_get(resumed_state, "profile"),
                             "settings"),
                         "theme")),
                 "dark");

    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(patched_state);
    turbo_runtime_data_bind_value_destroy(patch);
    turbo_free_json(&resumed_summary);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should expose persisted state and apply checkpoint commands through app wrappers") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *thread_state = NULL;
    turbo_runtime_data_bind_value_t *thread_head_state = NULL;
    turbo_runtime_data_bind_value_t *thread_trace_events = NULL;
    turbo_runtime_data_bind_value_t *thread_head_trace_events = NULL;
    turbo_runtime_data_bind_value_t *run_state = NULL;
    turbo_runtime_data_bind_value_t *checkpoint_state = NULL;
    turbo_runtime_data_bind_value_t *state_patch = NULL;
    turbo_runtime_data_bind_value_t *prepared_state_override = NULL;
    turbo_runtime_data_bind_value_t *legacy_state_override = NULL;
    turbo_runtime_data_bind_value_t *command = NULL;
    turbo_runtime_data_bind_value_t *override = NULL;
    turbo_runtime_data_bind_value_t *final_output_override = NULL;
    turbo_runtime_data_bind_value_t *payload = NULL;
    turbo_runtime_data_bind_value_t *prepared_command_override = NULL;
    turbo_runtime_data_bind_value_t *resumed_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *summary2 = NULL;
    json_value_t *lineage = NULL;
    json_value_t *branch_tree = NULL;
    json_value_t *override_json = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_app_last_checkpoint_id(app));
    check_int_eq(turbo_agent_app_list_thread_lineage(app, &lineage), 0);
    app_check_thread_lineage_bind(lineage, turbo_agent_app_thread_id(app),
                                  turbo_json_get_string(summary, "run_id"),
                                  turbo_json_get_string(summary, "run_id"),
                                  turbo_json_get_string(summary, "checkpoint_id"));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(lineage, "branches")), 1);
    app_check_lineage_branch(app_find_lineage_branch(turbo_json_object_get(lineage, "branches"),
                                                     NULL),
                             NULL, NULL, NULL,
                             turbo_json_get_string(summary, "checkpoint_id"));
    check_int_eq(turbo_agent_app_get_branch_tree(app, &branch_tree), 0);
    app_check_branch_tree(branch_tree, turbo_agent_app_thread_id(app),
                          turbo_json_get_string(summary, "run_id"),
                          turbo_json_get_string(summary, "run_id"),
                          turbo_json_get_string(summary, "run_id"),
                          turbo_json_get_string(summary, "checkpoint_id"), 1, 0);
    app_check_branch_tree_branch(
        app_find_branch_tree_branch(turbo_json_object_get(branch_tree, "branches"),
                                    turbo_json_get_string(summary, "run_id")),
        turbo_json_get_string(summary, "run_id"), NULL, NULL,
        turbo_json_get_string(summary, "checkpoint_id"));
    check_null(app_find_branch_tree_edge(turbo_json_object_get(branch_tree, "edges"),
                                         turbo_json_get_string(summary, "checkpoint_id"),
                                         turbo_json_get_string(summary, "run_id")));

    check_int_eq(turbo_agent_app_get_thread_state_bind(app, &thread_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(thread_state, "visited_start"), 0));
    check_int_eq(turbo_agent_app_get_thread_head_state_bind(app, &thread_head_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(thread_head_state, "visited_start"), 0));
    check_int_eq(turbo_agent_app_get_thread_trace_events_bind(app, &thread_trace_events), 0);
    check_int_eq(turbo_agent_app_get_thread_head_trace_events_bind(app,
                                                                   &thread_head_trace_events),
                 0);
    check_size_eq(turbo_runtime_data_bind_value_size(thread_head_trace_events),
                  turbo_runtime_data_bind_value_size(thread_trace_events));
    check_int_eq(turbo_agent_app_get_run_state_bind(app, NULL, &run_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(run_state, "visited_start"), 0));
    check_int_eq(turbo_agent_app_get_checkpoint_state_bind(app, NULL, &checkpoint_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(checkpoint_state, "visited_start"), 0));

    state_patch = create_app_state_patch_bind();
    check_not_null(state_patch);
    check_int_eq(turbo_agent_app_prepare_thread_state_override_bind(app, state_patch,
                                                                    &prepared_state_override),
                 0);
    check_int_eq(turbo_agent_app_update_thread_state_bind(app, state_patch,
                                                          &legacy_state_override),
                 0);
    check_str_eq(turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_object_get(prepared_state_override, "profile"),
                         "settings"),
                     "theme")),
                 "dark");
    check_str_eq(turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(
                     turbo_runtime_data_bind_object_get(
                         turbo_runtime_data_bind_object_get(legacy_state_override, "profile"),
                         "settings"),
                     "theme")),
                 "dark");
    turbo_runtime_data_bind_value_destroy(thread_state);
    thread_state = NULL;
    check_int_eq(turbo_agent_app_get_thread_state_bind(app, &thread_state), 0);
    check_null(turbo_runtime_data_bind_object_get(thread_state, "profile"));

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_app_apply_thread_command_bind(app, command, &override), 0);
    override_json = turbo_runtime_data_bind_value_to_json(override);
    check_not_null(override_json);
    check_true(turbo_agent_state_review_approved(override_json));
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("request_replan")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "reason",
                     turbo_runtime_data_bind_value_create_string("manual review requested")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_app_prepare_checkpoint_command_override_bind(
                     app, turbo_agent_app_last_checkpoint_id(app), command,
                     &prepared_command_override),
                 0);
    override_json = turbo_runtime_data_bind_value_to_json(prepared_command_override);
    check_not_null(override_json);
    check_true(turbo_agent_state_replan_requested(override_json));
    check_str_eq(turbo_agent_state_replan_reason(override_json), "manual review requested");
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;
    turbo_runtime_data_bind_value_destroy(checkpoint_state);
    checkpoint_state = NULL;
    check_int_eq(turbo_agent_app_get_checkpoint_state_bind(app, NULL, &checkpoint_state), 0);
    override_json = turbo_runtime_data_bind_value_to_json(checkpoint_state);
    check_not_null(override_json);
    check_false(turbo_agent_state_replan_requested(override_json));
    turbo_free_json(&override_json);
    override_json = NULL;

    command = turbo_runtime_data_bind_value_create_object();
    payload = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_not_null(payload);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     payload, "ok", turbo_runtime_data_bind_value_create_bool(1)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     payload, "value", turbo_runtime_data_bind_value_create_int64(11)),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("override_final_output")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_runtime_data_bind_object_set(command, "output_json", payload),
                 TURBO_RUNTIME_DATA_BIND_OK);
    payload = NULL;
    check_int_eq(turbo_agent_app_apply_checkpoint_command_bind(
                     app, turbo_agent_app_last_checkpoint_id(app), command, &final_output_override),
                 0);
    override_json = turbo_runtime_data_bind_value_to_json(final_output_override);
    check_not_null(override_json);
    check_str_eq(turbo_agent_state_final_answer_text(override_json),
                 "{\"ok\":true,\"value\":11}");
    turbo_free_json(&override_json);
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    options.interrupt_before_nodes = NULL;
    options.interrupt_before_count = 0;
    command = turbo_runtime_data_bind_value_create_object();
    check_not_null(command);
    check_int_eq(turbo_runtime_data_bind_object_set(
                     command, "kind",
                     turbo_runtime_data_bind_value_create_string("approve_review")),
                 TURBO_RUNTIME_DATA_BIND_OK);
    check_int_eq(turbo_agent_app_resume_checkpoint_command_bind(
                     app, graph, turbo_agent_app_last_checkpoint_id(app), command, &options,
                     &summary2, &resumed_state),
                 0);
    check_str_eq(turbo_json_get_string(summary2, "status"), "completed");
    turbo_runtime_data_bind_value_destroy(command);
    command = NULL;

    turbo_runtime_data_bind_value_destroy(run_state);
    run_state = NULL;
    check_int_eq(turbo_agent_app_get_run_state_bind(app, NULL, &run_state), 0);
    check_true(turbo_runtime_data_bind_value_as_bool(
        turbo_runtime_data_bind_object_get(run_state, "visited_end"), 0));

    turbo_free_json(&summary2);
    turbo_free_json(&summary);
    turbo_free_json(&lineage);
    turbo_free_json(&branch_tree);
    turbo_runtime_data_bind_value_destroy(prepared_command_override);
    turbo_runtime_data_bind_value_destroy(legacy_state_override);
    turbo_runtime_data_bind_value_destroy(prepared_state_override);
    turbo_runtime_data_bind_value_destroy(state_patch);
    turbo_runtime_data_bind_value_destroy(resumed_state);
    turbo_runtime_data_bind_value_destroy(final_output_override);
    turbo_runtime_data_bind_value_destroy(override);
    turbo_runtime_data_bind_value_destroy(command);
    turbo_runtime_data_bind_value_destroy(payload);
    turbo_runtime_data_bind_value_destroy(checkpoint_state);
    turbo_runtime_data_bind_value_destroy(run_state);
    turbo_runtime_data_bind_value_destroy(thread_head_trace_events);
    turbo_runtime_data_bind_value_destroy(thread_trace_events);
    turbo_runtime_data_bind_value_destroy(thread_head_state);
    turbo_runtime_data_bind_value_destroy(thread_state);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should expose mailbox wrappers through the app surface") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_supervisor_review_state();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *override = NULL;
    json_value_t *summary = NULL;
    json_value_t *inbox = NULL;
    json_value_t *history = NULL;
    json_value_t *override_json = NULL;
    const json_value_t *override_inbox = NULL;
    const json_value_t *override_history = NULL;
    turbo_graph_run_options_t options = {0};
    const char *interrupt_before_review[] = {"review"};

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_app_get_supervisor_inbox(app, &inbox), 0);
    check_int_eq(turbo_agent_app_get_supervisor_handoff_history(app, &history), 0);
    check_not_null(inbox);
    check_not_null(history);
    check_size_eq(turbo_json_array_size(inbox), 0);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "target_agent"),
                 "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "reason"),
                 "delegate execution");

    check_int_eq(turbo_agent_app_append_supervisor_inbox_message_bind(app, "supervisor",
                                                                      "review this", &override),
                 0);
    check_not_null(override);
    override_json = turbo_runtime_data_bind_value_to_json(override);
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
    turbo_runtime_data_bind_value_destroy(override);
    turbo_free_json(&history);
    turbo_free_json(&inbox);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should expose one supervisor inspect bundle through the app surface") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_supervisor_review_state();
    turbo_runtime_data_bind_value_t *result_state = NULL;
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

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_app_get_supervisor_inspect(app, &inspect), 0);
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
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should run one real supervisor handoff loop through the app surface") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_supervisor_handoff_graph();
    turbo_runtime_data_bind_value_t *state = create_app_supervisor_handoff_state();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    turbo_runtime_data_bind_value_t *inspect_timeline = NULL;
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

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    check_int_eq(turbo_agent_app_start_bind_graph_stream(app, graph, state, NULL, NULL, NULL,
                                                         &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_str_eq(turbo_json_get_string(summary, "thread_id"), turbo_agent_app_thread_id(app));
    check_str_eq(turbo_json_get_string(summary, "run_id"), turbo_agent_app_last_run_id(app));
    check_true(turbo_json_type(turbo_json_object_get(summary, "checkpoint_id")) == TURBO_JSON_NULL);
    check_str_eq(turbo_json_get_string(summary, "active_agent"), "executor");
    check_true(turbo_json_type(turbo_json_object_get(summary, "handoff_target_agent")) ==
               TURBO_JSON_NULL);
    check_true(turbo_json_type(turbo_json_object_get(summary, "handoff_reason")) ==
               TURBO_JSON_NULL);
    check_not_null(turbo_agent_app_thread_id(app));
    check_not_null(turbo_agent_app_last_run_id(app));
    check_null(turbo_agent_app_last_checkpoint_id(app));

    state_json = turbo_runtime_data_bind_value_to_json(result_state);
    check_not_null(state_json);
    check_true(turbo_json_get_bool(state_json, "visited_planner", false));
    check_true(turbo_json_get_bool(state_json, "visited_executor", false));
    check_str_eq(turbo_agent_state_active_agent(state_json), "executor");
    check_null(turbo_agent_state_handoff_target_agent(state_json));
    check_null(turbo_agent_state_handoff_reason(state_json));

    check_int_eq(turbo_agent_app_get_supervisor_inbox(app, &inbox), 0);
    check_int_eq(turbo_agent_app_get_supervisor_handoff_history(app, &history), 0);
    check_size_eq(turbo_json_array_size(inbox), 0);
    check_size_eq(turbo_json_array_size(history), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "target_agent"),
                 "executor");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(history, 0), "reason"),
                 "delegate execution");

    check_int_eq(turbo_agent_app_get_supervisor_inspect(app, &supervisor_inspect), 0);
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

    check_int_eq(turbo_agent_app_get_orchestration_inspect(app, &orchestration_inspect), 0);
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
    inspect_timeline = turbo_runtime_data_bind_value_from_json(
        turbo_json_object_get(orchestration_inspect, "thread_timeline"));
    check_not_null(inspect_timeline);
    app_check_thread_timeline_bind(inspect_timeline, turbo_agent_app_thread_id(app),
                                   turbo_agent_app_last_run_id(app), NULL, 0, 1, 0, 0);
    thread_lineage = turbo_json_object_get(orchestration_inspect, "thread_lineage");
    check_not_null(thread_lineage);
    check_str_eq(turbo_json_get_string(thread_lineage, "thread_id"),
                 turbo_agent_app_thread_id(app));
    check_str_eq(turbo_json_get_string(thread_lineage, "latest_run_id"),
                 turbo_agent_app_last_run_id(app));
    check_not_null(turbo_json_object_get(thread_lineage, "pending_run_id"));
    check_not_null(turbo_json_object_get(thread_lineage, "root_checkpoint_id"));
    check_not_null(turbo_json_object_get(thread_lineage, "branches"));
    check_true(turbo_json_type(turbo_json_object_get(thread_lineage, "branches")) ==
               TURBO_JSON_ARRAY);
    child_runs = turbo_json_object_get(orchestration_inspect, "child_runs");
    check_not_null(child_runs);
    check_true(turbo_json_type(child_runs) == TURBO_JSON_ARRAY);
    check_size_eq(turbo_json_array_size(child_runs), 0);

    turbo_runtime_data_bind_value_destroy(inspect_timeline);
    turbo_free_json(&orchestration_inspect);
    turbo_free_json(&supervisor_inspect);
    turbo_free_json(&history);
    turbo_free_json(&inbox);
    turbo_free_json(&state_json);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should expose one orchestration inspect bundle through the app surface") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_supervisor_review_state();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *inspect = NULL;
    turbo_runtime_data_bind_value_t *timeline = NULL;
    const json_value_t *supervisor_inspect = NULL;
    const json_value_t *thread_lineage = NULL;
    const json_value_t *branch_tree = NULL;
    const json_value_t *child_runs = NULL;
    const json_value_t *latest_handoff_event = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_app_get_orchestration_inspect(app, &inspect), 0);
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

    timeline = turbo_runtime_data_bind_value_from_json(
        turbo_json_object_get(inspect, "thread_timeline"));
    check_not_null(timeline);
    app_check_thread_timeline_bind(timeline, turbo_agent_app_thread_id(app),
                                   turbo_agent_app_last_run_id(app),
                                   turbo_agent_app_last_checkpoint_id(app), 1, 1, 1, 1);
    app_check_thread_lineage_bind(thread_lineage, turbo_agent_app_thread_id(app),
                                  turbo_agent_app_last_run_id(app),
                                  turbo_agent_app_last_run_id(app),
                                  turbo_agent_app_last_checkpoint_id(app));
    app_check_branch_tree(branch_tree, turbo_agent_app_thread_id(app),
                          turbo_agent_app_last_run_id(app), turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_checkpoint_id(app), 1, 0);

    turbo_runtime_data_bind_value_destroy(timeline);
    turbo_free_json(&inspect);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should expose one observability index bundle through the app surface") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_supervisor_review_state();
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *index_json = NULL;
    const json_value_t *counts = NULL;
    const json_value_t *history_events = NULL;
    const json_value_t *trace_events = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");

    check_int_eq(turbo_agent_app_get_observability_index(app, &index_json), 0);
    check_not_null(index_json);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(index_json, "thread"), "id"),
                 turbo_agent_app_thread_id(app));
    check_not_null(turbo_json_object_get(index_json, "thread_timeline"));
    check_not_null(turbo_json_object_get(index_json, "thread_lineage"));
    check_not_null(turbo_json_object_get(index_json, "branch_tree"));
    check_str_eq(turbo_json_get_string(index_json, "current_status"), "interrupted");
    check_str_eq(turbo_json_get_string(index_json, "current_interrupt_reason"),
                 "review_required");
    check_str_eq(turbo_json_get_string(index_json, "current_pending_action"), "review");
    check_str_eq(turbo_json_get_string(
                     turbo_json_object_get(index_json, "current_checkpoint_summary"), "id"),
                 turbo_agent_app_last_checkpoint_id(app));
    check_str_eq(turbo_json_get_string(index_json, "latest_run_status"), "interrupted");
    check_not_null(turbo_json_get_string(index_json, "latest_run_updated_at"));
    check_str_eq(turbo_json_get_string(index_json, "pending_run_id"),
                 turbo_agent_app_last_run_id(app));
    check_str_eq(turbo_json_get_string(index_json, "pending_checkpoint_id"),
                 turbo_agent_app_last_checkpoint_id(app));
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
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should query child run records through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *output_item = turbo_json_create_object();
    json_value_t *run_json = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(output_item);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    turbo_json_object_set_string(output_item, "child_run_id", turbo_agent_app_last_run_id(app));

    check_int_eq(turbo_agent_app_get_child_run(app, output_item, &run_json), 0);
    check_str_eq(turbo_json_get_string(run_json, "id"), turbo_agent_app_last_run_id(app));

    turbo_free_json(&run_json);
    turbo_free_json(&output_item);
    turbo_free_json(&summary);
    free(text);
    turbo_agent_app_destroy(app);
  }

  it("should load child history through app wrappers by child run id") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *output_item = turbo_json_create_object();
    turbo_runtime_data_bind_value_t *events = NULL;
    turbo_runtime_data_bind_value_t *trace_events = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(output_item);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    turbo_json_object_set_string(output_item, "child_run_id", turbo_agent_app_last_run_id(app));

    check_int_eq(turbo_agent_app_load_child_history_events_bind(app, output_item, &events), 0);
    check_size_eq(turbo_runtime_data_bind_value_size(events), 0);
    check_int_eq(turbo_agent_app_get_child_trace_events_bind(app, output_item, &trace_events), 0);
    check_size_eq(turbo_runtime_data_bind_value_size(trace_events), 0);

    turbo_runtime_data_bind_value_destroy(trace_events);
    turbo_runtime_data_bind_value_destroy(events);
    turbo_free_json(&output_item);
    turbo_free_json(&summary);
    free(text);
    turbo_agent_app_destroy(app);
  }

  it("should query child checkpoint context through app wrappers by child checkpoint id") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_graph_t *graph = create_app_review_graph();
    turbo_runtime_data_bind_value_t *state = create_app_review_state(0);
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *output_item = turbo_json_create_object();
    json_value_t *run_json = NULL;
    json_value_t *checkpoint_json = NULL;
    json_value_t *checkpoint_context = NULL;
    json_value_t *checkpoints_json = NULL;
    json_value_t *inspect_json = NULL;
    json_value_t *orchestration_inspect = NULL;
    json_value_t *multi_agent_inspect = NULL;
    json_value_t *child_branch_tree = NULL;
    turbo_runtime_data_bind_value_t *inspect_timeline = NULL;
    turbo_runtime_data_bind_value_t *child_timeline = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t options = {0};

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(graph);
    check_not_null(state);
    check_not_null(output_item);

    options.interrupt_before_nodes = interrupt_before_review;
    options.interrupt_before_count = 1;
    check_int_eq(turbo_agent_session_start_bind_graph(turbo_agent_app_session(app), graph, state,
                                                      &options, &summary, &result_state),
                 0);
    check_str_eq(turbo_json_get_string(summary, "status"), "interrupted");
    check_not_null(turbo_agent_app_last_run_id(app));
    check_not_null(turbo_agent_app_last_checkpoint_id(app));

    turbo_json_object_set_string(output_item, "child_run_id", turbo_agent_app_last_run_id(app));
    turbo_json_object_set_string(output_item, "child_checkpoint_id",
                                 turbo_agent_app_last_checkpoint_id(app));
    turbo_json_object_set_string(output_item, "child_thread_id",
                                 turbo_agent_app_thread_id(app));
    turbo_json_object_set_string(output_item, "parent_agent_run_id", "run_parent");
    turbo_json_object_set_string(output_item, "parent_tool_call_id", "call_parent");
    turbo_json_object_set_string(output_item, "parent_tool_name", "delegate");
    turbo_json_object_set_string(output_item, "parent_graph_run_id", "run_graph_parent");
    turbo_json_object_set_string(output_item, "call_frame_id", "frame_parent");

    check_int_eq(turbo_agent_app_get_child_run(app, output_item, &run_json), 0);
    check_int_eq(turbo_agent_app_get_child_checkpoint(app, output_item, &checkpoint_json), 0);
    check_int_eq(turbo_agent_app_get_child_checkpoint_context(app, output_item, &checkpoint_context),
                 0);
    check_int_eq(turbo_agent_app_get_child_thread_timeline_bind(app, output_item, &child_timeline),
                 0);
    check_int_eq(turbo_agent_app_get_child_branch_tree(app, output_item, &child_branch_tree), 0);
    check_int_eq(turbo_agent_app_list_child_checkpoints(app, output_item, &checkpoints_json), 0);
    check_int_eq(turbo_agent_app_get_child_inspect(app, output_item, &inspect_json), 0);
    check_int_eq(turbo_agent_app_get_child_orchestration_inspect(app, output_item,
                                                                 &orchestration_inspect),
                 0);
    check_int_eq(turbo_agent_app_get_child_multi_agent_inspect(app, output_item,
                                                               &multi_agent_inspect),
                 0);
    check_str_eq(turbo_json_get_string(run_json, "id"), turbo_agent_app_last_run_id(app));
    check_str_eq(turbo_json_get_string(checkpoint_json, "id"),
                 turbo_agent_app_last_checkpoint_id(app));
    check_size_eq(turbo_json_array_size(checkpoints_json), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(checkpoints_json, 0), "id"),
                 turbo_agent_app_last_checkpoint_id(app));
    app_check_checkpoint_context(checkpoint_context, turbo_agent_app_thread_id(app),
                                 turbo_agent_app_last_run_id(app),
                                 turbo_agent_app_last_checkpoint_id(app), NULL, 1);
    app_check_thread_timeline_bind(child_timeline, turbo_agent_app_thread_id(app),
                                   turbo_agent_app_last_run_id(app),
                                   turbo_agent_app_last_checkpoint_id(app), 1, 1, 1, 1);
    app_check_branch_tree(child_branch_tree, turbo_agent_app_thread_id(app),
                          turbo_agent_app_last_run_id(app), turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_checkpoint_id(app), 1, 0);
    check_not_null(inspect_json);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(inspect_json, "run"), "id"),
                 turbo_agent_app_last_run_id(app));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(inspect_json, "checkpoints")), 1);
    check_str_eq(
        turbo_json_get_string(turbo_json_object_get(inspect_json, "latest_checkpoint"), "id"),
        turbo_agent_app_last_checkpoint_id(app));
    app_check_checkpoint_context(turbo_json_object_get(inspect_json, "checkpoint_context"),
                                 turbo_agent_app_thread_id(app),
                                 turbo_agent_app_last_run_id(app),
                                 turbo_agent_app_last_checkpoint_id(app), NULL, 1);
    check_true(turbo_json_array_size(turbo_json_object_get(inspect_json, "history_events")) >= 1);
    check_size_eq(turbo_json_array_size(turbo_json_object_get(inspect_json, "trace_events")), 0);
    inspect_timeline = turbo_runtime_data_bind_value_from_json(
        turbo_json_object_get(inspect_json, "thread_timeline"));
    check_not_null(inspect_timeline);
    app_check_thread_timeline_bind(inspect_timeline, turbo_agent_app_thread_id(app),
                                   turbo_agent_app_last_run_id(app),
                                   turbo_agent_app_last_checkpoint_id(app), 1, 1, 1, 1);
    app_check_branch_tree(turbo_json_object_get(inspect_json, "branch_tree"),
                          turbo_agent_app_thread_id(app), turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_run_id(app), turbo_agent_app_last_run_id(app),
                          turbo_agent_app_last_checkpoint_id(app), 1, 0);
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
                 turbo_agent_app_last_run_id(app));
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
    turbo_runtime_data_bind_value_destroy(inspect_timeline);
    turbo_runtime_data_bind_value_destroy(child_timeline);
    turbo_free_json(&orchestration_inspect);
    turbo_free_json(&child_branch_tree);
    turbo_free_json(&inspect_json);
    turbo_free_json(&checkpoints_json);
    turbo_free_json(&checkpoint_context);
    turbo_free_json(&checkpoint_json);
    turbo_free_json(&run_json);
    turbo_free_json(&output_item);
    turbo_free_json(&summary);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(state);
    turbo_graph_destroy(graph);
    turbo_agent_app_destroy(app);
  }

  it("should list child runs through configured parent lineage defaults") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *runs = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session_config.parent_agent_run_id = "run_parent";
    session_config.parent_tool_call_id = "call_parent";
    session_config.parent_tool_name = "delegate";
    session_config.parent_graph_run_id = "run_graph_parent";
    session_config.call_frame_id = "frame_parent";
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    check_str_eq(turbo_json_get_string(summary, "parent_agent_run_id"), "run_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_call_id"), "call_parent");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(summary, "parent_graph_run_id"), "run_graph_parent");
    check_str_eq(turbo_json_get_string(summary, "call_frame_id"), "frame_parent");
    check_int_eq(turbo_agent_app_list_child_runs(app, NULL, &runs), 0);
    check_size_eq(turbo_json_array_size(runs), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "id"),
                 turbo_agent_app_last_run_id(app));
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "parent_graph_run_id"),
                 "run_graph_parent");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "call_frame_id"),
                 "frame_parent");

    turbo_free_json(&runs);
    turbo_free_json(&summary);
    free(text);
    turbo_agent_app_destroy(app);
  }

  it("should expose nullable parent lineage fields on app summaries without parent context") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
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
    free(text);
    turbo_agent_app_destroy(app);
  }

  it("should list child runs through current execution context defaults") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *runs = NULL;
    char *text = NULL;
    turbo_agent_execution_context_t saved_context = {0};
    turbo_agent_execution_context_t current_context = {0};

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    turbo_agent_execution_context_get(&saved_context);
    current_context = saved_context;
    current_context.run_id = "run_parent_auto";
    current_context.tool_call_id = "call_parent_auto";
    current_context.tool_name = "delegate";
    turbo_agent_execution_context_set(&current_context);

    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);
    check_str_eq(turbo_json_get_string(summary, "parent_agent_run_id"), "run_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_call_id"), "call_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(summary, "parent_graph_run_id"), "run_parent_auto");
    check_str_eq(turbo_json_get_string(summary, "call_frame_id"), "call_parent_auto");
    check_int_eq(turbo_agent_app_list_child_runs(app, NULL, &runs), 0);
    check_size_eq(turbo_json_array_size(runs), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "id"),
                 turbo_agent_app_last_run_id(app));
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "parent_graph_run_id"),
                 "run_parent_auto");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(runs, 0), "call_frame_id"),
                 "call_parent_auto");

    turbo_agent_execution_context_set(&saved_context);
    turbo_free_json(&runs);
    turbo_free_json(&summary);
    free(text);
    turbo_agent_app_destroy(app);
  }

  it("should expose session defaults and memory through app wrappers") {
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    json_value_t *records = NULL;
    json_value_t *record_views = NULL;
    json_value_t *queried = NULL;
    char *value_json = NULL;
    char *text = NULL;

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_success_transport;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = turbo_agent_memory_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session_config.memory_namespace = "project";
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_memory_put_context(app, "project/demo", "notes", "project",
                                                    "/tmp/notes.md", "remember"),
                 0);
    check_int_eq(turbo_agent_app_memory_get(app, "project/demo", "notes", &value_json), 0);
    check_int_eq(turbo_agent_app_memory_list(app, "project", &records), 0);
    check_int_eq(turbo_agent_app_memory_list_records(app, "project", &record_views), 0);
    check_int_eq(turbo_agent_app_memory_query_records(app, "project", "context", "no",
                                                      "remember", &queried),
                 0);
    check_int_eq(turbo_agent_app_invoke_text(app, "hello", NULL, &text, &summary), 0);

    check_int_eq(turbo_agent_app_workflow_kind(app), TURBO_AGENT_SESSION_WORKFLOW_LOOP);
    check_str_eq(turbo_agent_app_memory_namespace(app), "project");
    check_not_null(turbo_agent_app_memory_store(app));
    check_not_null(turbo_agent_app_thread_id(app));
    check_not_null(turbo_agent_app_last_run_id(app));
    check_null(turbo_agent_app_last_checkpoint_id(app));
    check_true(strstr(value_json, "\"scope\":\"project\"") != NULL);
    check_size_eq(turbo_json_array_size(records), 1);
    check_size_eq(turbo_json_array_size(record_views), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(record_views, 0), "kind"), "context");
    check_size_eq(turbo_json_array_size(queried), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "id"),
                 "project/demo::notes");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "namespace"),
                 "project/demo");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "kind"), "context");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "key"), "notes");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(queried, 0), "text"),
                 "remember");
    check_str_eq(text, "ok");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");

    free(value_json);
    free(text);
    turbo_free_json(&queried);
    turbo_free_json(&record_views);
    turbo_free_json(&records);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
  }

  it("should run the app default knowledge engineering workflow") {
    char *root = create_app_temp_root();
    char *db_path = create_app_temp_path(root, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t document = {0};
    app_knowledge_transport_state_t transport_state = {0};
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    check_not_null(root);
    check_not_null(db_path);
    check_not_null(store);
    document.id = "app-knowledge";
    document.uri = "memory://app-knowledge";
    document.kind = "note";
    document.title = "App knowledge note";
    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document,
                     "app knowledge marker from local docs for planner context", 128),
                 0);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_knowledge_engineering_transport;
    agent_config.transport_user_data = &transport_state;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = turbo_agent_memory_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING;
    session_config.knowledge_store = store;
    session_config.knowledge_kind = "note";
    session_config.knowledge_limit = 2;
    app_config.session_config = &session_config;

    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_int_eq(turbo_agent_app_workflow_kind(app),
                 TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING);
    check_int_eq(turbo_agent_app_invoke_text(
                     app, "please use local docs for planner context", NULL, &text,
                     &summary),
                 0);
    check_str_eq(text, "knowledge app ok");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_int_eq(transport_state.saw_knowledge_context, 1);
    check_int_eq(transport_state.call_count, 2);

    free(text);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
#ifdef _WIN32
    _rmdir(root);
#else
    rmdir(root);
#endif
    free(root);
  }

  it("should run the app default retriever engineering workflow") {
    app_retriever_transport_state_t transport_state = {0};
    app_retriever_query_state_t query_state = {0};
    turbo_retriever_config_t retriever_config = {0};
    turbo_retriever_t *retriever;
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    retriever_config.query = app_retriever_query;
    retriever_config.user_data = &query_state;
    retriever = turbo_retriever_create(&retriever_config);
    check_not_null(retriever);

    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = app_retriever_engineering_transport;
    agent_config.transport_user_data = &transport_state;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = turbo_agent_memory_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING;
    session_config.retriever = retriever;
    session_config.retriever_kind = "note";
    session_config.retriever_limit = 2;
    session_config.retriever_scope = "retriever";
    app_config.session_config = &session_config;

    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_int_eq(turbo_agent_app_workflow_kind(app),
                 TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING);
    check_int_eq(turbo_agent_app_invoke_text(
                     app, "please use retriever planner context", NULL, &text,
                     &summary),
                 0);
    check_str_eq(text, "retriever app ok");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_int_eq(query_state.call_count, 1);
    check_int_eq(transport_state.saw_retriever_context, 1);
    check_int_eq(transport_state.call_count, 2);

    free(text);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
    turbo_retriever_destroy(retriever);
  }

  it("should expose one query-only long-term memory store through app wrappers") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *record_views = NULL;
    json_value_t *queried = NULL;

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = app_query_only_memory_store_create();
    session_config.memory_namespace = "project";
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_not_null(turbo_agent_app_memory_store(app));
    check_int_eq(turbo_agent_app_memory_list_records(app, "project", &record_views), 0);
    check_size_eq(turbo_json_array_size(record_views), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(record_views, 0), "kind"), "context");

    check_int_eq(turbo_agent_app_memory_query_records(app, "project", "context", "con",
                                                      "remember", &queried),
                 0);
    app_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                          "context_query");

    turbo_free_json(&queried);
    turbo_free_json(&record_views);
    turbo_agent_app_destroy(app);
  }

  it("should expose canonical memory-record helpers through app wrappers") {
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    turbo_agent_memory_query_options_t options = {0};
    json_value_t *record = turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
    json_value_t *loaded = NULL;
    json_value_t *queried = NULL;

    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.memory_store = turbo_agent_memory_store_memory_create();
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_not_null(record);

    check_int_eq(turbo_agent_app_memory_validate_record(record), 0);
    check_int_eq(turbo_agent_app_memory_put_record(app, record), 0);
    check_int_eq(turbo_agent_app_memory_get_record(app, "project/demo", "context", &loaded), 0);
    app_check_memory_record_fixture(loaded, "memory_context_record.golden.json", NULL);

    options.namespace_prefix = "project";
    options.kind = "context";
    options.key_prefix = "con";
    options.text_substring = "remember";
    check_int_eq(turbo_agent_app_memory_query_records_ex(app, &options, &queried), 0);
    app_check_memory_record_array_fixture(queried, "memory_query_results.golden.json",
                                          "context_query");
    turbo_free_json(&queried);
    turbo_free_json(&loaded);
    turbo_free_json(&record);
    turbo_agent_app_destroy(app);
  }

  it("should auto-link child runtime lineage from the current parent tool call") {
    turbo_tool_registry_t *tool_registry = turbo_tool_registry_create();
    turbo_agent_config_t child_agent_config = {0};
    turbo_agent_session_config_t child_session_config = {0};
    turbo_agent_subagent_tool_config_t subagent_tool_config = {0};
    turbo_agent_config_t parent_agent_config = {0};
    turbo_agent_session_config_t parent_session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    app_tool_transport_state_t transport_state = {0};
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    json_value_t *state_json = NULL;
    json_value_t *output_json = NULL;
    json_value_t *child_run_json = NULL;
    const json_value_t *tool_results_event;
    const json_value_t *outputs;
    const json_value_t *output_item;
    const char *parent_run_id;
    const char *child_run_id;
    const char *output_text;
    char *child_root_dir = create_app_temp_root();
    turbo_agent_runtime_store_t inspector_store;
    turbo_agent_runtime_t *inspector_runtime = NULL;

    check_not_null(tool_registry);
    check_not_null(child_root_dir);

    child_agent_config.model = "gpt-5.4";
    child_agent_config.transport_fn = app_success_transport;
    child_session_config.agent_config = child_agent_config;
    child_session_config.runtime_store = turbo_agent_runtime_store_file_create(child_root_dir);
    child_session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    subagent_tool_config.name = "delegate";
    subagent_tool_config.description = "Delegate one task to a child agent.";
    subagent_tool_config.mode = TURBO_AGENT_SUBAGENT_SHARED_APP;
    subagent_tool_config.result_kind = TURBO_AGENT_SUBAGENT_RESULT_TEXT;
    subagent_tool_config.session_config = &child_session_config;
    check_int_eq(turbo_agent_subagent_add_tool_registry(tool_registry, &subagent_tool_config),
                 TURBO_TOOL_OK);

    parent_agent_config.model = "gpt-5.4";
    parent_agent_config.transport_fn = app_parent_tool_transport;
    parent_agent_config.transport_user_data = &transport_state;
    parent_agent_config.tool_registry = tool_registry;
    parent_session_config.agent_config = parent_agent_config;
    parent_session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    parent_session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &parent_session_config;

    app = turbo_agent_app_create(&app_config);
    check_not_null(app);
    check_int_eq(turbo_agent_app_start_text(app, "delegate this", NULL, &summary, &result_state),
                 0);
    check_int_eq(transport_state.call_count, 2);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");

    parent_run_id = turbo_json_get_string(summary, "run_id");
    check_not_null(parent_run_id);
    state_json = turbo_runtime_data_bind_value_to_json(result_state);
    check_not_null(state_json);
    tool_results_event = turbo_agent_state_latest_tool_results_event(state_json);
    outputs = turbo_agent_state_tool_results_outputs(tool_results_event);
    check_not_null(tool_results_event);
    check_not_null(outputs);
    check_size_eq(turbo_json_array_size(outputs), 1);
    output_item = turbo_json_array_get(outputs, 0);
    check_not_null(output_item);

    child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
    check_not_null(child_run_id);
    output_text = turbo_json_get_string(output_item, "output");
    check_not_null(output_text);
    check_int_eq(
        turbo_parse_json((const uint8_t *)output_text, strlen(output_text), &output_json), 0);
    check_str_eq(turbo_json_get_string(output_json, "parent_agent_run_id"), parent_run_id);
    check_str_eq(turbo_json_get_string(output_json, "parent_tool_call_id"), "call_parent_1");
    check_str_eq(turbo_json_get_string(output_json, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(output_json, "parent_graph_run_id"), parent_run_id);
    check_str_eq(turbo_json_get_string(output_json, "call_frame_id"), "call_parent_1");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(output_json, "summary"),
                                       "parent_agent_run_id"),
                 parent_run_id);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(output_json, "summary"),
                                       "call_frame_id"),
                 "call_parent_1");

    inspector_store = turbo_agent_runtime_store_file_create(child_root_dir);
    inspector_runtime = turbo_agent_runtime_create(&inspector_store);
    check_not_null(inspector_runtime);
    check_int_eq(turbo_agent_runtime_get_run(inspector_runtime, child_run_id, &child_run_json), 0);
    check_str_eq(turbo_json_get_string(child_run_json, "parent_agent_run_id"), parent_run_id);
    check_str_eq(turbo_json_get_string(child_run_json, "parent_tool_call_id"), "call_parent_1");
    check_str_eq(turbo_json_get_string(child_run_json, "parent_tool_name"), "delegate");
    check_str_eq(turbo_json_get_string(child_run_json, "parent_graph_run_id"), parent_run_id);
    check_str_eq(turbo_json_get_string(child_run_json, "call_frame_id"), "call_parent_1");

    turbo_free_json(&child_run_json);
    turbo_agent_runtime_destroy(inspector_runtime);
    turbo_free_json(&output_json);
    turbo_free_json(&state_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_free_json(&summary);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(tool_registry);
    free(child_root_dir);
  }
}
