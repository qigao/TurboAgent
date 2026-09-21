#include "tinytest.h"

#include "error_recovery.h"
#include "http_client.h"
#include "iris_app.h"
#include "rpc_client.h"
#include "server.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_iris.h"
#include "turbo_agent_state.h"
#include "turbo_agent_test_support.h"
#include "turbo_prompt.h"
#include <json_parser.h>

#include <stdio.h>
#include <string.h>

#define remote_session_observer_capture_t turbo_agent_test_observer_capture_t
#define remote_session_capture_observer_event turbo_agent_test_capture_observer_event
#define remote_session_check_memory_record_fixture turbo_agent_test_check_memory_record_fixture
#define remote_session_check_memory_record_array_fixture \
  turbo_agent_test_check_memory_record_array_fixture
#define remote_session_check_supervisor_inspect turbo_agent_test_check_supervisor_inspect
#define remote_session_check_thread_lineage_json_value turbo_agent_test_check_thread_lineage_json_value

typedef struct {
  const char *key;
  int value;
} remote_session_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} remote_session_graph_registry_t;

typedef struct {
  coro_context_t *coro_ctx;
  coro_socket_t *server;
  int server_stopped;
  turbo_graph_t *graph;
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_t *remote;
  turbo_agent_runtime_remote_iris_t *bridge;
  http_client_t *http_client;
  rpc_client_t *rpc_client;
  turbo_agent_remote_session_t *session;
  int accessors_ok;
  int read_thread_ok;
  int read_latest_run_ok;
  int read_pending_run_ok;
  int start_text_ok;
  int start_messages_ok;
  int invoke_text_ok;
  int invoke_messages_text_ok;
  int invoke_json_ok;
  int invoke_messages_json_ok;
  int thread_state_ok;
  int context_ok;
  int observability_ok;
  int timeline_ok;
  int thread_history_ok;
  int thread_history_replay_ok;
  int thread_observer_ok;
  int thread_trace_ok;
  int branch_tree_ok;
  int lineage_ok;
  int supervisor_inbox_ok;
  int supervisor_history_ok;
  int supervisor_inspect_ok;
  int requested_handoff_event_ok;
  int committed_handoff_event_ok;
  int child_runs_ok;
  int child_inspect_ok;
  int orchestration_inspect_ok;
  int resume_ok;
  int fork_ok;
} remote_session_test_state_t;

static void remote_session_test_state_cleanup(remote_session_test_state_t *state) {
  if (!state) {
    return;
  }
  if (state->session) {
    turbo_agent_remote_session_destroy(state->session);
    state->session = NULL;
  }
  if (state->rpc_client) {
    rpc_client_destroy(state->rpc_client);
    state->rpc_client = NULL;
  }
  if (state->http_client) {
    http_client_destroy(state->http_client);
    state->http_client = NULL;
  }
  if (!state->server_stopped && state->server) {
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  if (state->bridge) {
    turbo_agent_runtime_remote_iris_destroy(state->bridge);
    state->bridge = NULL;
  }
  if (state->remote) {
    turbo_agent_runtime_remote_destroy(state->remote);
    state->remote = NULL;
  }
  if (state->runtime) {
    turbo_agent_runtime_destroy(state->runtime);
    state->runtime = NULL;
  }
  state->graph = NULL;
}

static int remote_session_test_state_configure_rpc_client(
    remote_session_test_state_t *state, turbo_agent_remote_session_config_t *session_config,
    const char *endpoint_url) {
  rpc_client_config_t rpc_config = {0};

  if (!state || !session_config || !endpoint_url || !endpoint_url[0]) {
    return -1;
  }
  state->http_client = http_client_create(endpoint_url);
  if (!state->http_client) {
    return -1;
  }
  http_client_set_connect_timeout(state->http_client, 1000);
  http_client_set_read_timeout(state->http_client, 5000);
  http_client_set_timeout(state->http_client, 5000);

  rpc_config.url = endpoint_url;
  rpc_config.http_client = state->http_client;
  state->rpc_client = rpc_client_create(&rpc_config);
  if (!state->rpc_client) {
    return -1;
  }
  session_config->client_config.rpc_client = state->rpc_client;
  return 0;
}

static const char *REMOTE_SESSION_FINAL_OUTPUT_JSON = "{\"ok\":true,\"value\":42}";

static json_value_t *remote_session_make_memory_record_variant(const json_value_t *record_fixture,
                                                               const char *key,
                                                               const char *text) {
  json_value_t *record_json;
  char record_id[128];
  const char *record_namespace;

  check_not_null(record_fixture);
  check_not_null(key);
  check_not_null(text);
  record_namespace = turbo_json_get_string(record_fixture, "namespace");
  check_not_null(record_namespace);
  record_json = turbo_json_clone(record_fixture);
  check_not_null(record_json);
  check_true(snprintf(record_id, sizeof(record_id), "%s::%s", record_namespace, key) > 0);
  turbo_json_object_set_string(record_json, "id", record_id);
  turbo_json_object_set_string(record_json, "key", key);
  turbo_json_object_set_string(record_json, "text", text);
  return record_json;
}

static int remote_session_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  remote_session_bool_write_t *write = (remote_session_bool_write_t *)user_data;
  json_value_t *value;

  value = turbo_json_create_bool(write->value);
  if (!value) {
    return -1;
  }
  return turbo_runtime_json_object_set(ctx->json_value_state, write->key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static int remote_session_finalize_json_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *output_text = (const char *)user_data;

  if (!ctx || !ctx->state || !output_text) {
    return -1;
  }
  turbo_json_object_set_bool(ctx->state, "visited_end", true);
  return turbo_agent_state_set_final_answer(ctx->state, output_text);
}

static turbo_graph_t *remote_session_graph_resolver(const char *graph_name, void *user_data) {
  remote_session_graph_registry_t *registry = (remote_session_graph_registry_t *)user_data;

  if (!registry || !graph_name || !registry->name || !registry->graph) {
    return NULL;
  }
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *create_remote_session_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("remote-session-test");
  static remote_session_bool_write_t start = {"visited_start", 1};

  check_not_null(graph);
  check_int_eq(
      turbo_graph_add_json_value_node(graph, "start", remote_session_write_bool_json_value_node, &start),
      TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_node(graph, "end", remote_session_finalize_json_node,
                                    (void *)REMOTE_SESSION_FINAL_OUTPUT_JSON),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_remote_session_state_json_value(void) {
  json_value_t *state = turbo_agent_state_create_json_value();

  check_not_null(state);
  return state;
}

static json_value_t *create_remote_session_supervisor_state_json_value(void) {
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

static json_value_t *create_remote_session_committed_supervisor_state_json_value(void) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *bound;

  check_not_null(state);
  check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
  check_int_eq(turbo_agent_state_request_handoff(state, "executor", "delegate execution"), 0);
  check_int_eq(turbo_agent_state_commit_handoff(state), 0);
  check_int_eq(turbo_agent_state_request_review(state, "need approval"), 0);
  check_int_eq(turbo_agent_state_set_review_approved(state, 0), 0);
  bound = turbo_json_clone(state);
  turbo_free_json(&state);
  return bound;
}

static int remote_session_check_latest_handoff_event(const json_value_t *inspect_json,
                                                     const char *phase,
                                                     const char *from_agent,
                                                     const char *target_agent,
                                                     const char *reason,
                                                     const char *active_agent) {
  const json_value_t *handoff_event;
  const char *actual_kind;
  const char *actual_phase;
  const char *actual_from_agent;
  const char *actual_target_agent;
  const char *actual_reason;
  const char *actual_active_agent;

  if (!inspect_json || turbo_json_type(inspect_json) != TURBO_JSON_OBJECT) {
    return 0;
  }
  handoff_event = turbo_json_object_get(inspect_json, "latest_handoff_event");
  if (!handoff_event || turbo_json_type(handoff_event) != TURBO_JSON_OBJECT) {
    return 0;
  }
  actual_kind = turbo_json_get_string(handoff_event, "kind");
  if (!actual_kind || strcmp(actual_kind, "handoff") != 0) {
    return 0;
  }
  actual_phase = turbo_agent_state_handoff_event_phase(handoff_event);
  actual_from_agent = turbo_agent_state_handoff_event_from_agent(handoff_event);
  actual_target_agent = turbo_agent_state_handoff_event_target_agent(handoff_event);
  actual_reason = turbo_agent_state_handoff_event_reason(handoff_event);
  actual_active_agent = turbo_agent_state_handoff_event_active_agent(handoff_event);
  return actual_phase && actual_from_agent && actual_target_agent && actual_reason &&
         actual_active_agent && strcmp(actual_phase, phase) == 0 &&
         strcmp(actual_from_agent, from_agent) == 0 &&
         strcmp(actual_target_agent, target_agent) == 0 &&
         strcmp(actual_reason, reason) == 0 &&
         strcmp(actual_active_agent, active_agent) == 0;
}

static int remote_session_check_orchestration_latest_handoff_event(
    const json_value_t *inspect_json, const char *phase, const char *from_agent,
    const char *target_agent, const char *reason, const char *active_agent) {
  const json_value_t *supervisor_inspect;

  if (!inspect_json || turbo_json_type(inspect_json) != TURBO_JSON_OBJECT) {
    return 0;
  }
  supervisor_inspect = turbo_json_object_get(inspect_json, "supervisor_inspect");
  return remote_session_check_latest_handoff_event(supervisor_inspect, phase, from_agent,
                                                   target_agent, reason, active_agent);
}

static json_value_t *create_remote_session_command_json_value(const char *text) {
  json_value_t *command = turbo_json_create_object();

  check_not_null(command);
  check_int_eq(
      turbo_runtime_json_object_set(
          command, "kind", turbo_json_create_string("append_user_message")),
      TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(command, "text",
                                         turbo_json_create_string(text)),
      TURBO_RUNTIME_JSON_OK);
  return command;
}

static json_value_t *create_remote_session_messages_json_value(void) {
  json_value_t *messages = turbo_prompt_messages_create_json_value();

  check_not_null(messages);
  check_int_eq(turbo_prompt_messages_append_json_value(messages, "user", "hello remote session"),
               TURBO_PROMPT_OK);
  return messages;
}

static void remote_session_count_event_sink(const json_value_t *event,
                                            void *user_data) {
  int *count = (int *)user_data;

  if (!event || !count) {
    return;
  }
  *count += 1;
}

static void remote_session_test_coro(coro_t *co, void *arg) {
  remote_session_test_state_t *state = (remote_session_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  remote_session_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  json_value_t *input_state = NULL;
  json_value_t *thread_state = NULL;
  json_value_t *timeline = NULL;
  json_value_t *history_events = NULL;
  json_value_t *trace_events = NULL;
  json_value_t *result_state = NULL;
  json_value_t *command = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *context_json = NULL;
  json_value_t *index_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *lineage_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *output_item = NULL;
  json_value_t *child_run_json = NULL;
  json_value_t *child_checkpoint_json = NULL;
  json_value_t *child_checkpoint_context_json = NULL;
  json_value_t *child_branch_tree_json = NULL;
  json_value_t *child_checkpoints_json = NULL;
  json_value_t *child_inspect_json = NULL;
  json_value_t *child_orchestration_inspect_json = NULL;
  json_value_t *child_multi_agent_inspect_json = NULL;
  json_value_t *fork_summary_json = NULL;
  json_value_t *resume_summary_json = NULL;
  turbo_graph_run_options_t interrupt_options = {0};
  remote_session_observer_capture_t observer_capture = {0};
  turbo_agent_observer_json_value_sink_t observer_sink = {0};
  char endpoint_url[256];
  const char *thread_id = NULL;
  const char *run_id = NULL;
  const char *checkpoint_id = NULL;
  const char *fork_run_id;
  const char *resume_status;
  const char *fork_status;
  const json_value_t *history_entry;
  const json_value_t *visited_end_value;
  json_value_t *child_timeline = NULL;
  json_value_t *child_history_events = NULL;
  json_value_t *child_trace_events = NULL;
  int replayed_event_count = 0;
  int written;
  const unsigned short port = 29885;
  static const char *interrupt_before_end[] = {"end"};

  (void)co;

  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_session_graph();
  if (!state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-session";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = remote_session_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    return;
  }

  if (remote_session_test_state_configure_rpc_client(state, &session_config, endpoint_url) != 0) {
    return;
  }
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    return;
  }

  interrupt_options.interrupt_before_nodes = interrupt_before_end;
  interrupt_options.interrupt_before_count = 1;

  input_state = create_remote_session_supervisor_state_json_value();
  if (turbo_agent_remote_session_start_json_value_graph(state->session, "remote-session", input_state,
                                                  &interrupt_options, &summary_json,
                                                  &result_state) == 0 &&
      summary_json && result_state) {
    thread_id = turbo_json_get_string(summary_json, "thread_id");
    run_id = turbo_json_get_string(summary_json, "run_id");
    checkpoint_id = turbo_json_get_string(summary_json, "checkpoint_id");
    if (thread_id && run_id && checkpoint_id &&
        strcmp(thread_id, turbo_agent_remote_session_thread_id(state->session)) == 0 &&
        strcmp(run_id, turbo_agent_remote_session_last_run_id(state->session)) == 0 &&
        strcmp(checkpoint_id,
               turbo_agent_remote_session_last_checkpoint_id(state->session)) == 0 &&
        turbo_agent_remote_session_client(state->session)) {
      state->accessors_ok = 1;
    }
  }
  turbo_runtime_json_destroy(input_state);
  input_state = NULL;
  turbo_runtime_json_destroy(result_state);
  result_state = NULL;

  if (state->accessors_ok &&
      turbo_agent_remote_session_get_thread_state_json_value(state->session, &thread_state) == 0 &&
      thread_state &&
      turbo_json_type(thread_state) == TURBO_JSON_OBJECT &&
      ((visited_end_value = turbo_json_object_get(thread_state, "visited_end")) == NULL ||
       !turbo_runtime_json_value_as_bool(visited_end_value, 0))) {
    state->thread_state_ok = 1;
  }
  if (state->thread_state_ok &&
      turbo_agent_remote_session_get_checkpoint_context(state->session, NULL, &context_json) == 0 &&
      context_json) {
    state->context_ok = 1;
  }
  if (state->context_ok &&
      turbo_agent_remote_session_get_observability_index(state->session, &index_json) == 0 &&
      index_json) {
    state->observability_ok = 1;
  }
  if (state->observability_ok &&
      turbo_agent_remote_session_get_thread_timeline_json_value(state->session, &timeline) == 0 &&
      timeline &&
      turbo_json_type(timeline) == TURBO_JSON_OBJECT) {
    state->timeline_ok = 1;
  }
  if (state->timeline_ok &&
      turbo_agent_remote_session_load_thread_history_events_json_value(state->session,
                                                                 &history_events) == 0 &&
      history_events &&
      turbo_json_type(history_events) == TURBO_JSON_ARRAY &&
      turbo_json_array_get(history_events, 0) != NULL) {
    state->thread_history_ok = 1;
  }
  if (state->thread_history_ok &&
      turbo_agent_remote_session_replay_thread_history_json_value(state->session,
                                                            remote_session_count_event_sink,
                                                            &replayed_event_count) == 0 &&
      replayed_event_count >= 1) {
    state->thread_history_replay_ok = 1;
  }
  observer_sink.callback = remote_session_capture_observer_event;
  observer_sink.user_data = &observer_capture;
  if (state->thread_history_replay_ok &&
      turbo_agent_remote_session_observe_thread_history_json_value(state->session, &observer_sink) == 0 &&
      observer_capture.count >= 1 && observer_capture.interrupted_count >= 1) {
    state->thread_observer_ok = 1;
  }
  if (state->thread_observer_ok &&
      turbo_agent_remote_session_get_thread_trace_events_json_value(state->session, &trace_events) == 0 &&
      trace_events &&
      turbo_json_type(trace_events) == TURBO_JSON_ARRAY) {
    state->thread_trace_ok = 1;
  }
  if (state->thread_trace_ok &&
      turbo_agent_remote_session_get_branch_tree(state->session, &branch_tree_json) == 0 &&
      branch_tree_json &&
      turbo_json_object_get(branch_tree_json, "current_checkpoint_summary")) {
    state->branch_tree_ok = 1;
  }
  if (state->branch_tree_ok &&
      turbo_agent_remote_session_list_thread_lineage(state->session, &lineage_json) == 0 &&
      lineage_json) {
    remote_session_check_thread_lineage_json_value(lineage_json, thread_id, run_id, run_id,
                                             checkpoint_id);
    state->lineage_ok = 1;
  }
  if (state->lineage_ok &&
      turbo_agent_remote_session_get_supervisor_inbox(state->session, &inbox_json) == 0 &&
      inbox_json && turbo_json_type(inbox_json) == TURBO_JSON_ARRAY &&
      turbo_json_array_size(inbox_json) == 0) {
    state->supervisor_inbox_ok = 1;
  }
  history_entry = NULL;
  if (state->supervisor_inbox_ok &&
      turbo_agent_remote_session_get_supervisor_handoff_history(state->session, &history_json) == 0 &&
      history_json && turbo_json_type(history_json) == TURBO_JSON_ARRAY &&
      turbo_json_array_size(history_json) == 1 &&
      (history_entry = turbo_json_array_get(history_json, 0)) != NULL &&
      strcmp(turbo_json_get_string(history_entry, "from_agent"), "planner") == 0 &&
      strcmp(turbo_json_get_string(history_entry, "target_agent"), "executor") == 0 &&
      strcmp(turbo_json_get_string(history_entry, "reason"), "delegate execution") == 0) {
    state->supervisor_history_ok = 1;
  }
  if (state->supervisor_history_ok &&
      turbo_agent_remote_session_get_supervisor_inspect(state->session,
                                                        &supervisor_inspect_json) == 0 &&
      supervisor_inspect_json) {
    remote_session_check_supervisor_inspect(supervisor_inspect_json, "planner", "executor",
                                            "delegate execution", 0, 1);
    state->supervisor_inspect_ok = 1;
    state->requested_handoff_event_ok = remote_session_check_latest_handoff_event(
        supervisor_inspect_json, "requested", "planner", "executor", "delegate execution",
        "planner");
  }
  if (state->supervisor_inspect_ok &&
      turbo_agent_remote_session_list_child_runs(state->session, NULL, &child_runs_json) == 0 &&
      child_runs_json && turbo_json_type(child_runs_json) == TURBO_JSON_ARRAY &&
      turbo_json_array_size(child_runs_json) == 0) {
    state->child_runs_ok = 1;
  }
  if (state->child_runs_ok &&
      turbo_agent_remote_session_get_orchestration_inspect(state->session,
                                                           &orchestration_inspect_json) == 0 &&
      orchestration_inspect_json) {
    turbo_agent_test_check_orchestration_inspect(orchestration_inspect_json, thread_id, run_id,
                                                 checkpoint_id, 0, "planner", "executor",
                                                 "delegate execution");
    state->orchestration_inspect_ok = remote_session_check_orchestration_latest_handoff_event(
        orchestration_inspect_json, "requested", "planner", "executor", "delegate execution",
        "planner");
  }
  output_item = turbo_json_create_object();
  check_not_null(output_item);
  turbo_json_object_set_string(output_item, "child_run_id",
                               turbo_agent_remote_session_last_run_id(state->session));
  turbo_json_object_set_string(output_item, "child_checkpoint_id",
                               turbo_agent_remote_session_last_checkpoint_id(state->session));
  turbo_json_object_set_string(output_item, "child_thread_id",
                               turbo_agent_remote_session_thread_id(state->session));
  turbo_json_object_set_string(output_item, "parent_agent_run_id", "run_parent");
  turbo_json_object_set_string(output_item, "parent_tool_call_id", "call_parent");
  turbo_json_object_set_string(output_item, "parent_tool_name", "delegate");
  turbo_json_object_set_string(output_item, "parent_graph_run_id", "run_parent");
  turbo_json_object_set_string(output_item, "call_frame_id", "call_parent");

  check_int_eq(turbo_agent_remote_session_get_child_run(state->session, output_item,
                                                        &child_run_json),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_checkpoint(state->session, output_item,
                                                               &child_checkpoint_json),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_checkpoint_context(
                   state->session, output_item, &child_checkpoint_context_json),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_thread_timeline_json_value(state->session,
                                                                         output_item,
                                                                         &child_timeline),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_branch_tree(state->session, output_item,
                                                                &child_branch_tree_json),
               0);
  check_int_eq(turbo_agent_remote_session_list_child_checkpoints(state->session, output_item,
                                                                 &child_checkpoints_json),
               0);
  check_int_eq(turbo_agent_remote_session_load_child_history_events_json_value(state->session,
                                                                         output_item,
                                                                         &child_history_events),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_trace_events_json_value(state->session, output_item,
                                                                      &child_trace_events),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_inspect(state->session, output_item,
                                                            &child_inspect_json),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_orchestration_inspect(
                   state->session, output_item, &child_orchestration_inspect_json),
               0);
  check_int_eq(turbo_agent_remote_session_get_child_multi_agent_inspect(
                   state->session, output_item, &child_multi_agent_inspect_json),
               0);
  check_str_eq(turbo_json_get_string(child_run_json, "id"),
               turbo_agent_remote_session_last_run_id(state->session));
  check_str_eq(turbo_json_get_string(child_checkpoint_json, "id"),
               turbo_agent_remote_session_last_checkpoint_id(state->session));
  check_size_eq(turbo_json_array_size(child_checkpoints_json), 1);
  check_str_eq(turbo_json_get_string(turbo_json_array_get(child_checkpoints_json, 0), "id"),
               turbo_agent_remote_session_last_checkpoint_id(state->session));
  turbo_agent_test_check_checkpoint_context(
      child_checkpoint_context_json, turbo_agent_remote_session_thread_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_checkpoint_id(state->session), NULL, 1);
  turbo_agent_test_check_thread_timeline_json_value(
      child_timeline, turbo_agent_remote_session_thread_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_checkpoint_id(state->session), 1, 1, 1, 1);
  turbo_agent_test_check_branch_tree(
      child_branch_tree_json, turbo_agent_remote_session_thread_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_checkpoint_id(state->session), 1, 0);
  check_not_null(child_history_events);
  check_true(turbo_json_type(child_history_events) ==
             TURBO_JSON_ARRAY);
  check_true(turbo_json_array_get(child_history_events, 0) != NULL);
  check_not_null(child_trace_events);
  check_true(turbo_json_type(child_trace_events) ==
             TURBO_JSON_ARRAY);
  turbo_agent_test_check_child_inspect(child_inspect_json,
                                       turbo_agent_remote_session_thread_id(state->session),
                                       turbo_agent_remote_session_last_run_id(state->session),
                                       turbo_agent_remote_session_last_checkpoint_id(state->session),
                                       1);
  turbo_agent_test_check_child_orchestration_inspect(
      child_orchestration_inspect_json, "run_parent", "call_parent", "delegate",
      turbo_agent_remote_session_thread_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_checkpoint_id(state->session), 1);
  turbo_agent_test_check_child_multi_agent_inspect(
      child_multi_agent_inspect_json, turbo_agent_remote_session_thread_id(state->session),
      turbo_agent_remote_session_last_run_id(state->session),
      turbo_agent_remote_session_last_checkpoint_id(state->session), "run_parent",
      "call_parent", "delegate");
  state->child_inspect_ok = 1;
  turbo_runtime_json_destroy(thread_state);
  thread_state = NULL;
  turbo_runtime_json_destroy(timeline);
  timeline = NULL;
  turbo_runtime_json_destroy(history_events);
  history_events = NULL;
  turbo_runtime_json_destroy(trace_events);
  trace_events = NULL;
  turbo_free_json(&context_json);
  turbo_free_json(&index_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&lineage_json);
  turbo_free_json(&inbox_json);
  turbo_free_json(&history_json);
  turbo_free_json(&supervisor_inspect_json);
  turbo_free_json(&child_runs_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&child_inspect_json);
  turbo_free_json(&child_orchestration_inspect_json);
  turbo_free_json(&child_multi_agent_inspect_json);
  turbo_free_json(&child_branch_tree_json);
  turbo_free_json(&child_checkpoints_json);
  turbo_free_json(&child_checkpoint_json);
  turbo_free_json(&child_checkpoint_context_json);
  turbo_free_json(&child_run_json);
  turbo_free_json(&output_item);
  turbo_runtime_json_destroy(child_timeline);
  turbo_runtime_json_destroy(child_history_events);
  turbo_runtime_json_destroy(child_trace_events);
  turbo_free_json(&summary_json);

  command = create_remote_session_command_json_value("resume from remote session");
  if (state->branch_tree_ok &&
      turbo_agent_remote_session_resume_thread_command_json_value(state->session, "remote-session",
                                                            command, NULL, &resume_summary_json,
                                                            &result_state) == 0 &&
      resume_summary_json && result_state &&
      turbo_json_type(result_state) == TURBO_JSON_OBJECT &&
      (resume_status = turbo_json_get_string(resume_summary_json, "status")) &&
      strcmp(resume_status, "completed") == 0 &&
      turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0)) {
    state->resume_ok = 1;
  }
  turbo_runtime_json_destroy(command);
  command = NULL;
  turbo_runtime_json_destroy(result_state);
  result_state = NULL;
  turbo_free_json(&resume_summary_json);

  input_state = create_remote_session_state_json_value();
  if (turbo_agent_remote_session_start_json_value_graph(state->session, "remote-session", input_state,
                                                  &interrupt_options, &summary_json,
                                                  &result_state) == 0 &&
      summary_json && result_state) {
    fork_run_id = turbo_json_get_string(summary_json, "run_id");
    command = create_remote_session_command_json_value("fork from remote session");
    if (fork_run_id &&
        turbo_agent_remote_session_fork_thread_command_json_value(state->session, "remote-session",
                                                            command, NULL, &fork_summary_json,
                                                            &thread_state) == 0 &&
        fork_summary_json && thread_state &&
        turbo_json_type(thread_state) == TURBO_JSON_OBJECT &&
        (fork_status = turbo_json_get_string(fork_summary_json, "status")) &&
        strcmp(fork_status, "completed") == 0 &&
        strcmp(turbo_json_get_string(fork_summary_json, "run_id"), fork_run_id) != 0 &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(thread_state, "visited_end"), 0)) {
      state->fork_ok = 1;
    }
  }

  turbo_runtime_json_destroy(command);
  turbo_runtime_json_destroy(input_state);
  turbo_runtime_json_destroy(result_state);
  turbo_runtime_json_destroy(thread_state);
  command = NULL;
  input_state = NULL;
  result_state = NULL;
  thread_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&fork_summary_json);

  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_session_test_state_cleanup(state);
}

static void remote_session_committed_handoff_coro(coro_t *co, void *arg) {
  remote_session_test_state_t *state = (remote_session_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  remote_session_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  json_value_t *input_state = NULL;
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  char endpoint_url[256];
  int written;
  const unsigned short port = 29890;
  static const char *interrupt_before_end[] = {"end"};

  (void)co;

  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_session_graph();
  if (!state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-session";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = remote_session_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    return;
  }

  if (remote_session_test_state_configure_rpc_client(state, &session_config, endpoint_url) != 0) {
    return;
  }
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    return;
  }

  {
    const char *committed_thread_id;
    const char *committed_run_id;
    const char *committed_checkpoint_id;
    turbo_graph_run_options_t interrupt_options = {0};

    interrupt_options.interrupt_before_nodes = interrupt_before_end;
    interrupt_options.interrupt_before_count = 1;

    input_state = create_remote_session_committed_supervisor_state_json_value();
    if (turbo_agent_remote_session_start_json_value_graph(state->session, "remote-session", input_state,
                                                    &interrupt_options, &summary_json,
                                                    &result_state) != 0 ||
        !summary_json || !result_state) {
      goto cleanup;
    }

    committed_thread_id = turbo_json_get_string(summary_json, "thread_id");
    committed_run_id = turbo_json_get_string(summary_json, "run_id");
    committed_checkpoint_id = turbo_json_get_string(summary_json, "checkpoint_id");
    if (!committed_thread_id || !committed_run_id || !committed_checkpoint_id) {
      goto cleanup;
    }

    if (turbo_agent_remote_session_get_supervisor_inspect(state->session,
                                                          &supervisor_inspect_json) != 0 ||
        !supervisor_inspect_json ||
        !remote_session_check_latest_handoff_event(supervisor_inspect_json, "committed", "planner",
                                                   "executor", "delegate execution",
                                                   "executor")) {
      goto cleanup;
    }

    if (turbo_agent_remote_session_get_orchestration_inspect(state->session,
                                                             &orchestration_inspect_json) != 0 ||
        !orchestration_inspect_json) {
      goto cleanup;
    }

    turbo_agent_test_check_orchestration_inspect(orchestration_inspect_json, committed_thread_id,
                                                 committed_run_id, committed_checkpoint_id, 0,
                                                 "executor", "", "");
    state->committed_handoff_event_ok = remote_session_check_orchestration_latest_handoff_event(
        orchestration_inspect_json, "committed", "planner", "executor", "delegate execution",
        "executor");
  }

cleanup:
  turbo_runtime_json_destroy(input_state);
  turbo_runtime_json_destroy(result_state);
  turbo_free_json(&summary_json);
  turbo_free_json(&supervisor_inspect_json);
  turbo_free_json(&orchestration_inspect_json);

  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_session_test_state_cleanup(state);
}

static void remote_session_read_helpers_coro(coro_t *co, void *arg) {
  remote_session_test_state_t *state = (remote_session_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  remote_session_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  json_value_t *input_state = NULL;
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  turbo_graph_run_options_t interrupt_options = {0};
  char endpoint_url[256];
  const char *thread_id;
  const char *run_id;
  const char *loaded_thread_id;
  const char *latest_run_id;
  const char *latest_run_status;
  const char *pending_run_id;
  const char *pending_run_status;
  int written;
  const unsigned short port = 29886;
  static const char *interrupt_before_end[] = {"end"};

  (void)co;

  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_session_graph();
  if (!state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-session";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = remote_session_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    return;
  }

  if (remote_session_test_state_configure_rpc_client(state, &session_config, endpoint_url) != 0) {
    return;
  }
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    return;
  }

  interrupt_options.interrupt_before_nodes = interrupt_before_end;
  interrupt_options.interrupt_before_count = 1;

  input_state = create_remote_session_state_json_value();
  if (turbo_agent_remote_session_start_json_value_graph(state->session, "remote-session", input_state,
                                                  &interrupt_options, &summary_json,
                                                  &result_state) == 0 &&
      summary_json && result_state) {
    thread_id = turbo_json_get_string(summary_json, "thread_id");
    run_id = turbo_json_get_string(summary_json, "run_id");

    if (thread_id && turbo_agent_remote_session_get_thread(state->session, &thread_json) == 0 &&
        thread_json &&
        (loaded_thread_id = turbo_json_get_string(thread_json, "id")) != NULL &&
        strcmp(loaded_thread_id, thread_id) == 0) {
      state->read_thread_ok = 1;
    }
    if (run_id &&
        turbo_agent_remote_session_get_latest_run(state->session, &latest_run_json) == 0 &&
        latest_run_json &&
        (latest_run_id = turbo_json_get_string(latest_run_json, "id")) != NULL &&
        strcmp(latest_run_id, run_id) == 0 &&
        (latest_run_status = turbo_json_get_string(latest_run_json, "status")) != NULL &&
        strcmp(latest_run_status, "interrupted") == 0) {
      state->read_latest_run_ok = 1;
    }
    if (run_id &&
        turbo_agent_remote_session_get_pending_run(state->session, &pending_run_json) == 0 &&
        pending_run_json &&
        (pending_run_id = turbo_json_get_string(pending_run_json, "id")) != NULL &&
        strcmp(pending_run_id, run_id) == 0 &&
        (pending_run_status = turbo_json_get_string(pending_run_json, "status")) != NULL &&
        strcmp(pending_run_status, "interrupted") == 0) {
      state->read_pending_run_ok = 1;
    }
  }

  turbo_runtime_json_destroy(input_state);
  turbo_runtime_json_destroy(result_state);
  turbo_free_json(&summary_json);
  turbo_free_json(&thread_json);
  turbo_free_json(&latest_run_json);
  turbo_free_json(&pending_run_json);

  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_session_test_state_cleanup(state);
}

static void remote_session_high_level_helpers_coro(coro_t *co, void *arg) {
  remote_session_test_state_t *state = (remote_session_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  remote_session_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  json_value_t *messages = NULL;
  json_value_t *result_state = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *result_json = NULL;
  char *text = NULL;
  char endpoint_url[256];
  int written;
  const unsigned short port = 29887;

  (void)co;

  state->store = turbo_agent_runtime_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_session_graph();
  if (!state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-session";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.graph_resolver = remote_session_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    return;
  }

  if (remote_session_test_state_configure_rpc_client(state, &session_config, endpoint_url) != 0) {
    return;
  }
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    return;
  }

  if (turbo_agent_remote_session_start_text(state->session, "remote-session",
                                            "hello remote session", NULL, &summary_json,
                                            &result_state) == 0 &&
      summary_json && result_state &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0 &&
      turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0)) {
    state->start_text_ok = 1;
  }
  turbo_runtime_json_destroy(result_state);
  result_state = NULL;
  turbo_free_json(&summary_json);

  messages = create_remote_session_messages_json_value();
  if (turbo_agent_remote_session_start_messages(state->session, "remote-session", messages, NULL,
                                                &summary_json, &result_state) == 0 &&
      summary_json && result_state &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0 &&
      turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0)) {
    state->start_messages_ok = 1;
  }
  turbo_runtime_json_destroy(result_state);
  result_state = NULL;
  turbo_free_json(&summary_json);
  turbo_runtime_json_destroy(messages);
  messages = NULL;

  if (turbo_agent_remote_session_invoke_text(state->session, "remote-session",
                                             "hello remote session", NULL, &text,
                                             &summary_json) == 0 &&
      text && strcmp(text, REMOTE_SESSION_FINAL_OUTPUT_JSON) == 0 && summary_json &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0) {
    state->invoke_text_ok = 1;
  }
  free(text);
  text = NULL;
  turbo_free_json(&summary_json);

  messages = create_remote_session_messages_json_value();
  if (turbo_agent_remote_session_invoke_messages_text(state->session, "remote-session", messages,
                                                      NULL, &text, &summary_json) == 0 &&
      text && strcmp(text, REMOTE_SESSION_FINAL_OUTPUT_JSON) == 0 && summary_json &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0) {
    state->invoke_messages_text_ok = 1;
  }
  free(text);
  text = NULL;
  turbo_free_json(&summary_json);
  turbo_runtime_json_destroy(messages);
  messages = NULL;

  if (turbo_agent_remote_session_invoke_json(state->session, "remote-session",
                                             "hello remote session", NULL, &result_json,
                                             &summary_json) == 0 &&
      result_json && turbo_json_get_bool(result_json, "ok", false) &&
      turbo_json_get_int(result_json, "value", 0) == 42 && summary_json &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0) {
    state->invoke_json_ok = 1;
  }
  turbo_free_json(&result_json);
  result_json = NULL;
  turbo_free_json(&summary_json);

  messages = create_remote_session_messages_json_value();
  if (turbo_agent_remote_session_invoke_messages_json(state->session, "remote-session", messages,
                                                      NULL, &result_json, &summary_json) == 0 &&
      result_json && turbo_json_get_bool(result_json, "ok", false) &&
      turbo_json_get_int(result_json, "value", 0) == 42 && summary_json &&
      strcmp(turbo_json_get_string(summary_json, "status"), "completed") == 0) {
    state->invoke_messages_json_ok = 1;
  }

  turbo_runtime_json_destroy(messages);
  turbo_free_json(&result_json);
  turbo_free_json(&summary_json);

  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_session_test_state_cleanup(state);
}

typedef struct {
  coro_context_t *coro_ctx;
  coro_socket_t *server;
  int server_stopped;
  turbo_agent_runtime_store_t runtime_store;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_t *remote;
  turbo_agent_runtime_remote_iris_t *bridge;
  http_client_t *http_client;
  rpc_client_t *rpc_client;
  turbo_agent_remote_session_t *session;
  int memory_helpers_ok;
} remote_session_memory_test_state_t;

static void remote_session_memory_test_state_cleanup(remote_session_memory_test_state_t *state) {
  if (!state) {
    return;
  }
  if (state->session) {
    turbo_agent_remote_session_destroy(state->session);
    state->session = NULL;
  }
  if (state->rpc_client) {
    rpc_client_destroy(state->rpc_client);
    state->rpc_client = NULL;
  }
  if (state->http_client) {
    http_client_destroy(state->http_client);
    state->http_client = NULL;
  }
  if (!state->server_stopped && state->server) {
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  if (state->bridge) {
    turbo_agent_runtime_remote_iris_destroy(state->bridge);
    state->bridge = NULL;
  }
  if (state->remote) {
    turbo_agent_runtime_remote_destroy(state->remote);
    state->remote = NULL;
  }
  turbo_agent_memory_store_destroy(&state->memory_store);
  if (state->runtime) {
    turbo_agent_runtime_destroy(state->runtime);
    state->runtime = NULL;
  }
}

static int remote_session_memory_test_state_configure_rpc_client(
    remote_session_memory_test_state_t *state,
    turbo_agent_remote_session_config_t *session_config, const char *endpoint_url) {
  rpc_client_config_t rpc_config = {0};

  if (!state || !session_config || !endpoint_url || !endpoint_url[0]) {
    return -1;
  }
  state->http_client = http_client_create(endpoint_url);
  if (!state->http_client) {
    return -1;
  }
  http_client_set_connect_timeout(state->http_client, 1000);
  http_client_set_read_timeout(state->http_client, 5000);
  http_client_set_timeout(state->http_client, 5000);

  rpc_config.url = endpoint_url;
  rpc_config.http_client = state->http_client;
  state->rpc_client = rpc_client_create(&rpc_config);
  if (!state->rpc_client) {
    return -1;
  }
  session_config->client_config.rpc_client = state->rpc_client;
  return 0;
}

static void remote_session_memory_test_coro(coro_t *co, void *arg) {
  remote_session_memory_test_state_t *state = (remote_session_memory_test_state_t *)arg;
  iris_app_t *app = iris_app_default();
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_iris_config_t bridge_config = {0};
  turbo_agent_remote_session_config_t session_config = {0};
  turbo_agent_memory_query_options_t options = {0};
  json_value_t *record = NULL;
  json_value_t *record_variant = NULL;
  json_value_t *invalid_record = NULL;
  json_value_t *loaded_record = NULL;
  json_value_t *deleted_record = NULL;
  json_value_t *listed_records = NULL;
  json_value_t *listed_records_after_delete = NULL;
  json_value_t *queried_records = NULL;
  json_value_t *queried_records_ex = NULL;
  int record_valid = 0;
  int invalid_record_valid = 1;
  char endpoint_url[256];
  int written;
  const unsigned short port = 29886;

  (void)co;

  state->runtime_store = turbo_agent_runtime_store_memory_create();
  state->memory_store = turbo_agent_memory_store_memory_create();
  state->runtime = turbo_agent_runtime_create(&state->runtime_store);
  if (!state->runtime) {
    return;
  }

  remote_config.runtime = state->runtime;
  remote_config.memory_store = &state->memory_store;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  bridge_config.remote = state->remote;
  bridge_config.path = "/v1/runtime/jsonrpc";
  state->bridge = turbo_agent_runtime_remote_iris_create(&bridge_config);
  if (!state->bridge || turbo_agent_runtime_remote_iris_mount(state->bridge, app) != 0) {
    return;
  }

  init_router();
  state->server = iris_server_start(app, state->coro_ctx, port);
  if (!state->server) {
    return;
  }

  coro_yield();
  coro_sleep(state->coro_ctx, 50);

  written = snprintf(endpoint_url, sizeof(endpoint_url), "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned)port);
  if (written <= 0 || (size_t)written >= sizeof(endpoint_url)) {
    return;
  }

  if (remote_session_memory_test_state_configure_rpc_client(state, &session_config, endpoint_url) !=
      0) {
    return;
  }
  state->session = turbo_agent_remote_session_create(&session_config);
  if (!state->session) {
    return;
  }

  record = turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
  if (!record) {
    return;
  }

  invalid_record = turbo_json_create_object();
  if (!invalid_record) {
    return;
  }
  turbo_json_object_set_string(invalid_record, "id", "project/demo::broken");
  turbo_json_object_set_string(invalid_record, "namespace", "project/demo");
  turbo_json_object_set_string(invalid_record, "kind", "context");
  turbo_json_object_set_string(invalid_record, "key", "broken");
  turbo_json_object_set_string(invalid_record, "text", "missing value json");
  turbo_json_object_set_null(invalid_record, "metadata");
  turbo_json_object_set_null(invalid_record, "created_at");

  if (turbo_agent_remote_session_memory_validate_record(state->session, record, &record_valid) ==
          0 &&
      record_valid &&
      turbo_agent_remote_session_memory_validate_record(state->session, invalid_record,
                                                        &invalid_record_valid) == 0 &&
      !invalid_record_valid &&
      turbo_agent_remote_session_memory_put_record(state->session, record) == 0 &&
      turbo_agent_remote_session_memory_get_record(state->session, "project/demo", "context",
                                                   &loaded_record) == 0 &&
      loaded_record &&
      turbo_agent_remote_session_memory_list_records(state->session, "project", &listed_records) ==
          0 &&
      listed_records && turbo_json_array_size(listed_records) == 1 &&
      turbo_agent_remote_session_memory_query_records(state->session, "project", "context", "con",
                                                      "remember", &queried_records) == 0 &&
      queried_records && turbo_json_array_size(queried_records) == 1) {
    record_variant =
        remote_session_make_memory_record_variant(record, "zeta", "remember this too");
    if (record_variant) {
      turbo_json_object_set_string(record_variant, "created_at", "2026-02-15T12:00:00Z");
    }
    if (record_variant &&
        turbo_agent_remote_session_memory_put_record(state->session, record_variant) == 0) {
      options.namespace_prefix = "project";
      options.kind = "context";
      options.key_prefix = "con";
      options.text_substring = "remember";
      if (turbo_agent_remote_session_memory_query_records_ex(state->session, &options,
                                                             &queried_records_ex) == 0 &&
          queried_records_ex && turbo_json_array_size(queried_records_ex) == 1) {
        remote_session_check_memory_record_fixture(loaded_record,
                                                   "memory_context_record.golden.json", NULL);
        remote_session_check_memory_record_array_fixture(listed_records,
                                                         "memory_query_results.golden.json",
                                                         "context_query");
        remote_session_check_memory_record_array_fixture(queried_records,
                                                         "memory_query_results.golden.json",
                                                         "context_query");
        remote_session_check_memory_record_array_fixture(queried_records_ex,
                                                         "memory_query_results.golden.json",
                                                         "context_query");
        turbo_free_json(&queried_records_ex);
        queried_records_ex = NULL;

        options.key_prefix = NULL;
        options.id_prefix = "project/demo::z";
        options.metadata_scope = "project";
        options.metadata_path_prefix = "/tmp";
        options.created_after = "2026-02-01T00:00:00Z";
        options.created_before = "2026-02-28T23:59:59Z";
        options.sort_by = "key";
        options.sort_order = "desc";
        options.limit = 1;
        if (turbo_agent_remote_session_memory_query_records_ex(state->session, &options,
                                                               &queried_records_ex) == 0 &&
            queried_records_ex && turbo_json_array_size(queried_records_ex) == 1 &&
            strcmp(turbo_json_get_string(turbo_json_array_get(queried_records_ex, 0), "key"),
                   "zeta") == 0 &&
            turbo_agent_remote_session_memory_delete_record(state->session, "project/demo",
                                                            "zeta") == 0 &&
            turbo_agent_remote_session_memory_list_records(state->session, "project",
                                                           &listed_records_after_delete) == 0 &&
            listed_records_after_delete &&
            turbo_json_array_size(listed_records_after_delete) == 1 &&
            turbo_agent_remote_session_memory_get_record(state->session, "project/demo", "zeta",
                                                         &deleted_record) != 0 &&
            !deleted_record) {
          state->memory_helpers_ok = 1;
        }
      }
    }
  }

  turbo_free_json(&record_variant);
  turbo_free_json(&queried_records_ex);
  turbo_free_json(&queried_records);
  turbo_free_json(&listed_records_after_delete);
  turbo_free_json(&listed_records);
  turbo_free_json(&deleted_record);
  turbo_free_json(&loaded_record);
  turbo_free_json(&invalid_record);
  turbo_free_json(&record);

  if (state->server) {
    state->server_stopped = 1;
    coro_socket_destroy(state->server);
    state->server = NULL;
  }
  remote_session_memory_test_state_cleanup(state);
}

spec("turbo agent remote session api") {
  remote_session_test_state_t state = {0};

  before_each() {
    memset(&state, 0, sizeof(state));
    iris_app_reset_default();
    reset_router();
    iris_error_recovery_init();
  }

  after_each() {
    remote_session_test_state_cleanup(&state);
    if (state.coro_ctx) {
      coro_context_destroy(state.coro_ctx);
      state.coro_ctx = NULL;
    }
    reset_router();
    iris_app_reset_default();
  }

  it("should wrap remote runtime threads through a session facade") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, remote_session_test_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);

    check_true(state.accessors_ok);
    check_true(state.thread_state_ok);
    check_true(state.context_ok);
    check_true(state.observability_ok);
    check_true(state.timeline_ok);
    check_true(state.thread_history_ok);
    check_true(state.thread_history_replay_ok);
    check_true(state.thread_observer_ok);
    check_true(state.thread_trace_ok);
    check_true(state.branch_tree_ok);
    check_true(state.lineage_ok);
    check_true(state.supervisor_inbox_ok);
    check_true(state.supervisor_history_ok);
    check_true(state.supervisor_inspect_ok);
    check_true(state.requested_handoff_event_ok);
    check_true(state.child_runs_ok);
    check_true(state.child_inspect_ok);
    check_true(state.orchestration_inspect_ok);
    check_true(state.resume_ok);
    check_true(state.fork_ok);
  }

  it("should expose committed handoff events through the remote session facade") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, remote_session_committed_handoff_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);

    check_true(state.committed_handoff_event_ok);
  }

  it("should read thread and run records through the observability bundle") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, remote_session_read_helpers_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);

    check_true(state.read_thread_ok);
    check_true(state.read_latest_run_ok);
    check_true(state.read_pending_run_ok);
  }

  it("should expose start and invoke helpers through the remote session facade") {
    state.coro_ctx = coro_context_create(NULL);
    check_not_null(state.coro_ctx);

    coro_context_spawn(state.coro_ctx, remote_session_high_level_helpers_coro, &state);
    coro_context_run(state.coro_ctx, TURBO_RUN_DEFAULT);

    check_true(state.start_text_ok);
    check_true(state.start_messages_ok);
    check_true(state.invoke_text_ok);
    check_true(state.invoke_messages_text_ok);
    check_true(state.invoke_json_ok);
    check_true(state.invoke_messages_json_ok);
  }

  it("should expose memory helpers through the remote session facade") {
    remote_session_memory_test_state_t memory_state = {0};

    memory_state.coro_ctx = coro_context_create(NULL);
    check_not_null(memory_state.coro_ctx);

    coro_context_spawn(memory_state.coro_ctx, remote_session_memory_test_coro, &memory_state);
    coro_context_run(memory_state.coro_ctx, TURBO_RUN_DEFAULT);

    check_true(memory_state.memory_helpers_ok);
    remote_session_memory_test_state_cleanup(&memory_state);
    if (memory_state.coro_ctx) {
      coro_context_destroy(memory_state.coro_ctx);
      memory_state.coro_ctx = NULL;
    }
    reset_router();
    iris_app_reset_default();
  }
}
