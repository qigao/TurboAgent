#include "tinytest.h"

#include <http_server/http.h>
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_client.h"
#include "turbo_agent_runtime_remote_chttp.h"
#include "turbo_agent_state.h"
#include "turbo_agent_test_support.h"
#include <json_parser.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { REMOTE_CLIENT_METHOD_NOT_FOUND = -32601 };

typedef struct {
  const char *key;
  int value;
} remote_client_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} remote_client_graph_registry_t;

typedef struct {
  chttp_server server;
  int server_initialized;
  int server_started;
  turbo_graph_t *graph;
  turbo_agent_runtime_store_t store;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_runtime_t *runtime;
  turbo_agent_runtime_remote_t *remote;
  turbo_agent_runtime_remote_client_t *client;
  int direct_call_ok;
  int helper_run_ok;
  int inspect_ok;
  int record_helpers_ok;
  int memory_helpers_ok;
  int timeline_ok;
  int branch_tree_ok;
  int observability_ok;
  int list_indexes_ok;
  int supervisor_inspect_ok;
  int orchestration_inspect_ok;
  int requested_handoff_event_ok;
  int committed_handoff_event_ok;
  int child_runs_ok;
  int child_inspect_ok;
  int child_orchestration_inspect_ok;
  int child_multi_agent_inspect_ok;
  int thread_command_resume_ok;
  int thread_command_fork_ok;
  int method_not_found_ok;
} remote_client_test_state_t;

enum {
  REMOTE_CLIENT_TEST_CONNECTIONS = 4,
  REMOTE_CLIENT_TEST_COMMANDS = 32,
  REMOTE_CLIENT_TEST_SEND_BYTES = 64 * 1024,
  REMOTE_CLIENT_TEST_BUFFER_BYTES = 1024 * 1024,
  REMOTE_CLIENT_TEST_TIMEOUT_MS = 5000
};

static chttp_server_config remote_client_test_server_config(void) {
  chttp_server_config config = {0};
  config.host = "127.0.0.1";
  config.port = 0u;
  config.backlog = REMOTE_CLIENT_TEST_CONNECTIONS;
#if defined(_WIN32)
  config.network.backend = NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  config.network.backend = NATIVE_IO_BACKEND_EPOLL;
#else
  config.network.backend = NATIVE_IO_BACKEND_KQUEUE;
#endif
  config.network.connection_capacity = REMOTE_CLIENT_TEST_CONNECTIONS;
  config.network.command_capacity = REMOTE_CLIENT_TEST_COMMANDS;
  config.network.request_capacity = REMOTE_CLIENT_TEST_COMMANDS;
  config.network.completion_batch_capacity = REMOTE_CLIENT_TEST_CONNECTIONS;
  config.network.event_capacity = REMOTE_CLIENT_TEST_COMMANDS;
  config.network.max_send_bytes = REMOTE_CLIENT_TEST_SEND_BYTES;
  config.network.receive_buffer_bytes = REMOTE_CLIENT_TEST_SEND_BYTES;
  config.network.connect_timeout_ms = REMOTE_CLIENT_TEST_TIMEOUT_MS;
  config.network.read_timeout_ms = REMOTE_CLIENT_TEST_TIMEOUT_MS;
  config.network.write_timeout_ms = REMOTE_CLIENT_TEST_TIMEOUT_MS;
  config.route_capacity = 4u;
  config.middleware_capacity = 1u;
  config.max_route_middleware_count = 1u;
  config.max_route_param_count = 1u;
  config.max_route_param_bytes = 256u;
  config.max_target_bytes = 256u;
  config.max_header_count = 32u;
  config.max_header_bytes = 8192u;
  config.max_request_body_bytes = REMOTE_CLIENT_TEST_SEND_BYTES;
  config.max_response_header_count = 32u;
  config.max_response_header_bytes = 8192u;
  config.max_response_body_bytes = REMOTE_CLIENT_TEST_SEND_BYTES;
  config.max_buffered_response_body_bytes = REMOTE_CLIENT_TEST_SEND_BYTES;
  config.buffer_capacity_bytes = REMOTE_CLIENT_TEST_BUFFER_BYTES;
  config.poll_slice_ms = 1u;
  return config;
}

static int remote_client_test_server_start(chttp_server *server, int *initialized,
                                           int *started,
                                           turbo_agent_runtime_remote_t *remote,
                                           char *endpoint_url,
                                           size_t endpoint_capacity) {
  chttp_server_config config;
  uint16_t port = 0u;
  int status;
  int written;

  if (!server || !initialized || !started || !remote || !endpoint_url ||
      endpoint_capacity == 0u) {
    return -1;
  }
  config = remote_client_test_server_config();
  status = chttp_server_init(server, &config);
  if (status != SALTS_OK) return -1;
  *initialized = 1;
  status = turbo_agent_runtime_remote_chttp_mount(
      remote, server, "/v1/runtime/jsonrpc");
  if (status != SALTS_OK) return -1;
  status = chttp_server_start(server);
  if (status != SALTS_OK) return -1;
  *started = 1;
  status = chttp_server_port(server, &port);
  if (status != SALTS_OK || port == 0u) return -1;
  written = snprintf(endpoint_url, endpoint_capacity,
                     "http://127.0.0.1:%u/v1/runtime/jsonrpc",
                     (unsigned int)port);
  return written > 0 && (size_t)written < endpoint_capacity ? 0 : -1;
}

static void remote_client_test_server_cleanup(chttp_server *server,
                                              int *initialized, int *started) {
  if (!server || !initialized || !started) return;
  if (*started) {
    (void)chttp_server_stop(server, REMOTE_CLIENT_TEST_TIMEOUT_MS);
    *started = 0;
  }
  if (*initialized) {
    (void)chttp_server_destroy(server);
    *initialized = 0;
  }
}

static void remote_client_test_state_cleanup(remote_client_test_state_t *state) {
  if (!state) return;
  if (state->client) {
    turbo_agent_runtime_remote_client_destroy(state->client);
    state->client = NULL;
  }
  remote_client_test_server_cleanup(&state->server, &state->server_initialized,
                                    &state->server_started);
  if (state->remote) {
    turbo_agent_runtime_remote_destroy(state->remote);
    state->remote = NULL;
  }
  turbo_agent_memory_store_destroy(&state->memory_store);
  if (state->runtime) {
    turbo_agent_runtime_destroy(state->runtime);
    state->runtime = NULL;
  }
  state->graph = NULL;
}

static int remote_client_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  remote_client_bool_write_t *write = (remote_client_bool_write_t *)user_data;
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

static turbo_graph_t *remote_client_graph_resolver(const char *graph_name, void *user_data) {
  remote_client_graph_registry_t *registry = (remote_client_graph_registry_t *)user_data;

  if (!registry || !graph_name || !registry->name || !registry->graph) {
    return NULL;
  }
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *create_remote_client_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("remote-client-test");
  static remote_client_bool_write_t start = {"visited_start", 1};
  static remote_client_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_int_eq(
      turbo_graph_add_json_value_node(graph, "start", remote_client_write_bool_json_value_node, &start),
      TURBO_GRAPH_EXEC_OK);
  check_int_eq(
      turbo_graph_add_json_value_node(graph, "end", remote_client_write_bool_json_value_node, &end),
      TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_remote_client_state_json(void) {
  json_value_t *state = turbo_agent_state_create_json_value();
  json_value_t *state_json;

  check_not_null(state);
  state_json = turbo_json_clone(state);
  turbo_runtime_json_destroy(state);
  return state_json;
}

static json_value_t *create_remote_client_start_params_json(void) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *state_json = create_remote_client_state_json();
  json_value_t *options_json = turbo_json_create_object();
  json_value_t *interrupt_nodes = turbo_json_create_array();

  check_not_null(params_json);
  check_not_null(state_json);
  check_not_null(options_json);
  check_not_null(interrupt_nodes);

  turbo_json_object_set_string(params_json, "graph_name", "remote-client");
  turbo_json_object_set_string(params_json, "thread_id", "thr_remote_client");
  turbo_json_object_add(params_json, "state", state_json);
  turbo_json_array_add(interrupt_nodes, turbo_json_create_string("end"));
  turbo_json_object_add(options_json, "interrupt_before_nodes", interrupt_nodes);
  turbo_json_object_add(params_json, "options", options_json);
  return params_json;
}

static json_value_t *create_remote_client_state_json_value(void) {
  json_value_t *state = turbo_agent_state_create_json_value();

  check_not_null(state);
  return state;
}

static json_value_t *create_remote_client_supervisor_state_json_value(void) {
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

static json_value_t *create_remote_client_committed_supervisor_state_json_value(void) {
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

static int remote_client_check_latest_handoff_event(const json_value_t *inspect_json,
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

static int remote_client_check_orchestration_latest_handoff_event(
    const json_value_t *inspect_json, const char *phase, const char *from_agent,
    const char *target_agent, const char *reason, const char *active_agent) {
  const json_value_t *supervisor_inspect;

  if (!inspect_json || turbo_json_type(inspect_json) != TURBO_JSON_OBJECT) {
    return 0;
  }
  supervisor_inspect = turbo_json_object_get(inspect_json, "supervisor_inspect");
  return remote_client_check_latest_handoff_event(supervisor_inspect, phase, from_agent,
                                                  target_agent, reason, active_agent);
}

static json_value_t *create_remote_client_command_json_value(const char *kind,
                                                                          const char *text) {
  json_value_t *command = turbo_json_create_object();

  check_not_null(command);
  check_int_eq(
      turbo_runtime_json_object_set(command, "kind",
                                         turbo_json_create_string(kind)),
      TURBO_RUNTIME_JSON_OK);
  if (text) {
    check_int_eq(
        turbo_runtime_json_object_set(command, "text",
                                           turbo_json_create_string(text)),
        TURBO_RUNTIME_JSON_OK);
  }
  return command;
}

static json_value_t *create_remote_client_child_output_item(const char *child_run_id,
                                                            const char *child_checkpoint_id,
                                                            const char *child_thread_id,
                                                            const char *parent_agent_run_id,
                                                            const char *parent_tool_call_id,
                                                            const char *parent_tool_name,
                                                            const char *parent_graph_run_id,
                                                            const char *call_frame_id) {
  json_value_t *output_item = turbo_json_create_object();

  check_not_null(output_item);
  turbo_json_object_set_string(output_item, "child_run_id", child_run_id);
  turbo_json_object_set_string(output_item, "child_checkpoint_id", child_checkpoint_id);
  turbo_json_object_set_string(output_item, "child_thread_id", child_thread_id);
  turbo_json_object_set_string(output_item, "child_status", "completed");
  turbo_json_object_set_string(output_item, "parent_agent_run_id", parent_agent_run_id);
  turbo_json_object_set_string(output_item, "parent_tool_call_id", parent_tool_call_id);
  turbo_json_object_set_string(output_item, "parent_tool_name", parent_tool_name);
  turbo_json_object_set_string(output_item, "parent_graph_run_id", parent_graph_run_id);
  turbo_json_object_set_string(output_item, "call_frame_id", call_frame_id);
  return output_item;
}

static char *remote_client_strdup(const char *text) {
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

static json_value_t *remote_client_make_memory_record_variant(const json_value_t *record_fixture,
                                                             const char *key, const char *text) {
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

static void remote_runtime_remote_client_test_coro(void *arg) {
  remote_client_test_state_t *state = (remote_client_test_state_t *)arg;
  remote_client_graph_registry_t registry = {0};
  turbo_agent_runtime_remote_config_t remote_config = {0};
  turbo_agent_runtime_remote_client_config_t client_config = {0};
  json_value_t *state_json_value = NULL;
  json_value_t *thread_state = NULL;
  json_value_t *resumed_state = NULL;
  json_value_t *forked_state = NULL;
  json_value_t *timeline = NULL;
  json_value_t *history_events = NULL;
  json_value_t *run_trace_events = NULL;
  json_value_t *checkpoint_trace_events = NULL;
  json_value_t *command = NULL;
  json_value_t *params_json = NULL;
  json_value_t *result_json = NULL;
  json_value_t *error_json = NULL;
  json_value_t *summary_json = NULL;
  json_value_t *resumed_summary = NULL;
  json_value_t *forked_summary = NULL;
  json_value_t *context_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *checkpoint_json = NULL;
  json_value_t *checkpoints_json = NULL;
  json_value_t *index_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *indexes_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *child_multi_agent_inspect_json = NULL;
  json_value_t *filters_json = NULL;
  json_value_t *state_json;
  json_value_t *empty_params_json = NULL;
  json_value_t *memory_record_fixture = NULL;
  json_value_t *memory_record_json = NULL;
  json_value_t *memory_record_variant_json = NULL;
  json_value_t *memory_invalid_record_json = NULL;
  json_value_t *memory_query_records_json = NULL;
  json_value_t *memory_list_records_json = NULL;
  json_value_t *memory_records_after_delete_json = NULL;
  int memory_record_valid = 0;
  int memory_invalid_record_valid = 1;
  turbo_graph_run_options_t options = {0};
  turbo_graph_run_options_t interrupt_options = {0};
  turbo_agent_memory_query_options_t memory_query_options = {0};
  char endpoint_url[256];
  const char *status;
  char *checkpoint_id = NULL;
  char *original_run_id = NULL;
  const char *thread_command_run_id;
  const char *thread_command_fork_run_id;
  const char *error_message;
  const char *child_thread_id = "thr_remote_client_helper";
  static const char *interrupt_before_end[] = {"end"};


  state->store = turbo_agent_runtime_store_memory_create();
  state->memory_store = turbo_agent_memory_store_memory_create();
  check_not_null(state->memory_store.get);
  check_not_null(state->memory_store.put);
  state->runtime = turbo_agent_runtime_create(&state->store);
  state->graph = create_remote_client_graph();
  if (!state->runtime || !state->graph) {
    return;
  }

  registry.name = "remote-client";
  registry.graph = state->graph;
  remote_config.runtime = state->runtime;
  remote_config.memory_store = &state->memory_store;
  remote_config.graph_resolver = remote_client_graph_resolver;
  remote_config.graph_resolver_user_data = &registry;
  state->remote = turbo_agent_runtime_remote_create(&remote_config);
  if (!state->remote) {
    return;
  }

  if (remote_client_test_server_start(
          &state->server, &state->server_initialized, &state->server_started,
          state->remote, endpoint_url, sizeof(endpoint_url)) != 0) {
    return;
  }

  client_config.url = endpoint_url;
  state->client = turbo_agent_runtime_remote_client_create(&client_config);
  if (!state->client) {
    return;
  }

  options.interrupt_before_nodes = interrupt_before_end;
  options.interrupt_before_count = 1;
  interrupt_options.interrupt_before_nodes = interrupt_before_end;
  interrupt_options.interrupt_before_count = 1;
  state_json_value = create_remote_client_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(state->client, "remote-client", state_json_value,
                                                         &options, "thr_remote_client_helper",
                                                         &summary_json, &resumed_state,
                                                         &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    free(checkpoint_id);
    checkpoint_id = remote_client_strdup(turbo_json_get_string(summary_json, "checkpoint_id"));
    free(original_run_id);
    original_run_id = remote_client_strdup(turbo_json_get_string(summary_json, "run_id"));
    if (checkpoint_id && original_run_id &&
        strcmp(turbo_json_get_string(summary_json, "status"), "interrupted") == 0 &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(resumed_state, "visited_start"), 0) &&
        !turbo_runtime_json_value_as_bool(
            turbo_json_object_get(resumed_state, "visited_end"), 0) &&
        turbo_agent_runtime_remote_client_resume_json_value_graph(state->client, "remote-client",
                                                            checkpoint_id, NULL, NULL,
                                                            &resumed_summary, &thread_state,
                                                            &error_json) == 0 &&
        resumed_summary && thread_state &&
        strcmp(turbo_json_get_string(resumed_summary, "status"), "completed") == 0 &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(thread_state, "visited_end"), 0) &&
        turbo_agent_runtime_remote_client_fork_json_value_graph(state->client, "remote-client",
                                                          checkpoint_id, NULL, NULL,
                                                          &forked_summary, &forked_state,
                                                          &error_json) == 0 &&
        forked_summary && forked_state &&
        strcmp(turbo_json_get_string(forked_summary, "status"), "completed") == 0 &&
        strcmp(turbo_json_get_string(forked_summary, "run_id"), original_run_id) != 0) {
      state->helper_run_ok = 1;
    }

    turbo_runtime_json_destroy(forked_state);
    forked_state = NULL;
    turbo_free_json(&forked_summary);
    turbo_runtime_json_destroy(thread_state);
    thread_state = NULL;
    turbo_free_json(&resumed_summary);
    turbo_runtime_json_destroy(resumed_state);
    resumed_state = NULL;

    if (checkpoint_id &&
        turbo_agent_runtime_remote_client_get_thread_state_json_value(
            state->client, "thr_remote_client_helper", &thread_state, &error_json) == 0 &&
        thread_state &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(thread_state, "visited_end"), 0) &&
        turbo_agent_runtime_remote_client_get_checkpoint_context(state->client, checkpoint_id,
                                                                 &context_json, &error_json) == 0 &&
        context_json &&
        strcmp(turbo_json_get_string(turbo_json_object_get(context_json, "thread"), "id"),
               "thr_remote_client_helper") == 0 &&
        strcmp(turbo_json_get_string(turbo_json_object_get(context_json, "run"), "id"),
               original_run_id) == 0) {
      state->inspect_ok = 1;
    }
    if (checkpoint_id && original_run_id &&
        turbo_agent_runtime_remote_client_get_run(state->client, original_run_id, &run_json,
                                                  &error_json) == 0 &&
        run_json &&
        strcmp(turbo_json_get_string(run_json, "id"), original_run_id) == 0 &&
        strcmp(turbo_json_get_string(run_json, "thread_id"), "thr_remote_client_helper") == 0 &&
        turbo_agent_runtime_remote_client_get_checkpoint(state->client, checkpoint_id,
                                                         &checkpoint_json, &error_json) == 0 &&
        checkpoint_json &&
        strcmp(turbo_json_get_string(checkpoint_json, "id"), checkpoint_id) == 0 &&
        strcmp(turbo_json_get_string(checkpoint_json, "run_id"), original_run_id) == 0 &&
        turbo_agent_runtime_remote_client_list_checkpoints(state->client, original_run_id,
                                                           &checkpoints_json, &error_json) == 0 &&
        checkpoints_json && turbo_json_type(checkpoints_json) == TURBO_JSON_ARRAY &&
        turbo_json_array_size(checkpoints_json) == 1 &&
        strcmp(turbo_json_get_string(turbo_json_array_get(checkpoints_json, 0), "id"),
               checkpoint_id) == 0 &&
        turbo_agent_runtime_remote_client_load_history_events_json_value(
            state->client, NULL, checkpoint_id, &history_events, &error_json) == 0 &&
        history_events &&
        turbo_json_type(history_events) == TURBO_JSON_ARRAY &&
        turbo_json_array_get(history_events, 0) != NULL &&
        turbo_agent_runtime_remote_client_get_run_trace_events_json_value(
            state->client, original_run_id, &run_trace_events, &error_json) == 0 &&
        run_trace_events &&
        turbo_json_type(run_trace_events) ==
            TURBO_JSON_ARRAY &&
        turbo_json_array_get(run_trace_events, 0) == NULL &&
        turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value(
            state->client, checkpoint_id, &checkpoint_trace_events, &error_json) == 0 &&
        checkpoint_trace_events &&
        turbo_json_type(checkpoint_trace_events) ==
            TURBO_JSON_ARRAY &&
        turbo_json_array_get(checkpoint_trace_events, 0) == NULL) {
      state->record_helpers_ok = 1;
    }
  }
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(thread_state);
  thread_state = NULL;
  turbo_runtime_json_destroy(history_events);
  history_events = NULL;
  turbo_runtime_json_destroy(run_trace_events);
  run_trace_events = NULL;
  turbo_runtime_json_destroy(checkpoint_trace_events);
  checkpoint_trace_events = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_runtime_json_destroy(forked_state);
  forked_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&resumed_summary);
  turbo_free_json(&forked_summary);
  turbo_free_json(&context_json);
  turbo_free_json(&run_json);
  turbo_free_json(&checkpoint_json);
  turbo_free_json(&checkpoints_json);
  turbo_free_json(&error_json);

  memory_record_fixture = turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
  check_not_null(memory_record_fixture);
  memory_query_options.namespace_prefix = "project";
  memory_query_options.kind = "context";
  memory_query_options.key_prefix = "con";
  memory_query_options.text_substring = "remember";
  memory_invalid_record_json = turbo_json_create_object();
  check_not_null(memory_invalid_record_json);
  turbo_json_object_set_string(memory_invalid_record_json, "id", "project/demo::broken");
  turbo_json_object_set_string(memory_invalid_record_json, "namespace", "project/demo");
  turbo_json_object_set_string(memory_invalid_record_json, "kind", "context");
  turbo_json_object_set_string(memory_invalid_record_json, "key", "broken");
  turbo_json_object_set_string(memory_invalid_record_json, "text", "missing value json");
  turbo_json_object_set_null(memory_invalid_record_json, "metadata");
  turbo_json_object_set_null(memory_invalid_record_json, "created_at");
  if (turbo_agent_runtime_remote_client_validate_memory_record(
          state->client, memory_record_fixture, &memory_record_valid, &error_json) == 0 &&
      memory_record_valid && !error_json &&
      turbo_agent_runtime_remote_client_validate_memory_record(
          state->client, memory_invalid_record_json, &memory_invalid_record_valid,
          &error_json) == 0 &&
      !memory_invalid_record_valid && !error_json &&
      turbo_agent_runtime_remote_client_put_memory_record(state->client, memory_record_fixture,
                                                          &memory_record_json, &error_json) == 0 &&
      memory_record_json && !error_json) {
    turbo_agent_test_check_memory_record_fixture(memory_record_json,
                                                 "memory_context_record.golden.json", NULL);
    turbo_free_json(&memory_record_json);
    memory_record_json = NULL;

    memory_record_variant_json =
        remote_client_make_memory_record_variant(memory_record_fixture, "zeta",
                                                 "remember this too");
    check_not_null(memory_record_variant_json);
    turbo_json_object_set_string(memory_record_variant_json, "created_at",
                                 "2026-02-15T12:00:00Z");
    if (turbo_agent_runtime_remote_client_put_memory_record(state->client,
                                                            memory_record_variant_json,
                                                            &memory_record_json, &error_json) == 0 &&
        memory_record_json && !error_json) {
      turbo_free_json(&memory_record_json);
      memory_record_json = NULL;

      if (turbo_agent_runtime_remote_client_get_memory_record(state->client, "project/demo",
                                                              "context", &memory_record_json,
                                                              &error_json) == 0 &&
          memory_record_json && !error_json) {
        turbo_agent_test_check_memory_record_fixture(memory_record_json,
                                                     "memory_context_record.golden.json", NULL);
        turbo_free_json(&memory_record_json);
        memory_record_json = NULL;

        if (turbo_agent_runtime_remote_client_query_memory_records_ex(
                state->client, &memory_query_options, &memory_query_records_json,
                &error_json) == 0 &&
            memory_query_records_json && !error_json) {
          turbo_agent_test_check_memory_record_array_fixture(
              memory_query_records_json, "memory_query_results.golden.json", "context_query");
          turbo_free_json(&memory_query_records_json);
          memory_query_records_json = NULL;

          memory_query_options.key_prefix = NULL;
          memory_query_options.id_prefix = "project/demo::z";
          memory_query_options.metadata_scope = "project";
          memory_query_options.metadata_path_prefix = "/tmp";
          memory_query_options.created_after = "2026-02-01T00:00:00Z";
          memory_query_options.created_before = "2026-02-28T23:59:59Z";
          memory_query_options.sort_by = "key";
          memory_query_options.sort_order = "desc";
          memory_query_options.limit = 1;
          if (turbo_agent_runtime_remote_client_query_memory_records_ex(
                  state->client, &memory_query_options, &memory_query_records_json,
                  &error_json) == 0 &&
              memory_query_records_json && !error_json) {
            check_true(turbo_json_type(memory_query_records_json) == TURBO_JSON_ARRAY);
            check_size_eq(turbo_json_array_size(memory_query_records_json), 1);
            check_str_eq(turbo_json_get_string(turbo_json_array_get(memory_query_records_json, 0),
                                               "key"),
                         "zeta");
            turbo_free_json(&memory_query_records_json);
            memory_query_records_json = NULL;

            if (turbo_agent_runtime_remote_client_query_memory_records(
                    state->client, "project", "context", "con", "remember",
                    &memory_list_records_json, &error_json) == 0 &&
                memory_list_records_json && !error_json) {
              turbo_agent_test_check_memory_record_array_fixture(
                  memory_list_records_json, "memory_query_results.golden.json", "context_query");
              turbo_free_json(&memory_list_records_json);
              memory_list_records_json = NULL;
              if (turbo_agent_runtime_remote_client_list_memory_records(
                      state->client, "project", &memory_query_records_json, &error_json) == 0 &&
                  memory_query_records_json && !error_json) {
                check_true(turbo_json_type(memory_query_records_json) == TURBO_JSON_ARRAY);
                check_size_eq(turbo_json_array_size(memory_query_records_json), 2);
                turbo_free_json(&memory_query_records_json);
                memory_query_records_json = NULL;
                if (turbo_agent_runtime_remote_client_delete_memory_record(
                        state->client, "project/demo", "zeta", &error_json) == 0 &&
                    !error_json &&
                    turbo_agent_runtime_remote_client_list_memory_records(
                        state->client, "project", &memory_records_after_delete_json,
                        &error_json) == 0 &&
                    memory_records_after_delete_json && !error_json &&
                    turbo_json_array_size(memory_records_after_delete_json) == 1 &&
                    turbo_agent_runtime_remote_client_get_memory_record(
                        state->client, "project/demo", "zeta", &memory_record_json,
                        &error_json) != 0 &&
                    !memory_record_json && error_json) {
                  turbo_free_json(&error_json);
                  error_json = NULL;
                  state->memory_helpers_ok = 1;
                }
              }
            }
          }
        }
      }
    }
  }
  turbo_free_json(&memory_record_json);
  turbo_free_json(&memory_record_variant_json);
  turbo_free_json(&memory_query_records_json);
  turbo_free_json(&memory_list_records_json);
  turbo_free_json(&memory_records_after_delete_json);
  turbo_free_json(&memory_invalid_record_json);
  turbo_free_json(&memory_record_fixture);
  turbo_free_json(&error_json);

  state_json_value = create_remote_client_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-client", state_json_value, &interrupt_options,
          "thr_remote_client_observability", &summary_json, &resumed_state, &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    free(original_run_id);
    original_run_id = remote_client_strdup(turbo_json_get_string(summary_json, "run_id"));
    if (original_run_id &&
        turbo_agent_runtime_remote_client_get_thread_observability_index(
            state->client, "thr_remote_client_observability", &index_json, &error_json) == 0 &&
        index_json &&
        strcmp(turbo_json_get_string(turbo_json_object_get(index_json, "thread"), "id"),
               "thr_remote_client_observability") == 0 &&
        strcmp(turbo_json_get_string(turbo_json_object_get(index_json, "latest_run"), "id"),
               original_run_id) == 0 &&
        strcmp(turbo_json_get_string(turbo_json_object_get(index_json, "pending_run"), "id"),
               original_run_id) == 0 &&
        turbo_json_object_get(index_json, "thread_timeline") &&
        turbo_json_object_get(index_json, "branch_tree") &&
        turbo_json_object_get(index_json, "counts")) {
      state->observability_ok = 1;
    }
  }
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&index_json);
  turbo_free_json(&error_json);

  if (turbo_agent_runtime_remote_client_get_thread_timeline_json_value(
          state->client, "thr_remote_client_observability", &timeline, &error_json) == 0 &&
      timeline &&
      turbo_json_type(timeline) == TURBO_JSON_OBJECT &&
      strcmp(turbo_runtime_json_value_as_string(
                 turbo_json_object_get(
                     turbo_json_object_get(timeline, "thread"), "id")),
             "thr_remote_client_observability") == 0 &&
      turbo_json_type(
          turbo_json_object_get(timeline, "runs")) ==
          TURBO_JSON_ARRAY &&
      turbo_json_type(
          turbo_json_object_get(timeline, "history_events")) ==
          TURBO_JSON_ARRAY) {
    state->timeline_ok = 1;
  }
  turbo_runtime_json_destroy(timeline);
  timeline = NULL;
  turbo_free_json(&error_json);

  if (turbo_agent_runtime_remote_client_get_branch_tree(
          state->client, "thr_remote_client_observability", &branch_tree_json, &error_json) == 0 &&
      branch_tree_json &&
      strcmp(turbo_json_get_string(branch_tree_json, "thread_id"),
             "thr_remote_client_observability") == 0 &&
      turbo_json_type(turbo_json_object_get(branch_tree_json, "branches")) == TURBO_JSON_ARRAY &&
      turbo_json_type(turbo_json_object_get(branch_tree_json, "edges")) == TURBO_JSON_ARRAY &&
      turbo_json_object_get(branch_tree_json, "current_branch") &&
      turbo_json_object_get(branch_tree_json, "current_checkpoint_summary")) {
    state->branch_tree_ok = 1;
  }
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&error_json);

  filters_json = turbo_json_create_object();
  check_not_null(filters_json);
  turbo_json_object_set_string(filters_json, "thread_id_prefix", "thr_remote_client_");
  turbo_json_object_set_string(filters_json, "status", "interrupted");
  if (turbo_agent_runtime_remote_client_list_observability_indexes_filtered(
          state->client, filters_json, &indexes_json, &error_json) == 0 &&
      indexes_json && turbo_json_type(indexes_json) == TURBO_JSON_ARRAY &&
      turbo_json_array_size(indexes_json) >= 1 &&
      turbo_json_object_get(turbo_json_array_get(indexes_json, 0), "thread") &&
      turbo_json_object_get(turbo_json_array_get(indexes_json, 0), "counts")) {
    state->list_indexes_ok = 1;
  }
  turbo_free_json(&filters_json);
  turbo_free_json(&indexes_json);
  turbo_free_json(&error_json);

  if (original_run_id &&
      turbo_agent_runtime_remote_client_list_child_runs(state->client, original_run_id,
                                                        &child_runs_json, &error_json) == 0 &&
      child_runs_json && turbo_json_type(child_runs_json) == TURBO_JSON_ARRAY &&
      turbo_json_array_size(child_runs_json) == 0) {
    state->child_runs_ok = 1;
  }
  turbo_free_json(&child_runs_json);
  turbo_free_json(&error_json);

  if (original_run_id && checkpoint_id) {
    json_value_t *child_output_item = create_remote_client_child_output_item(
        original_run_id, checkpoint_id, child_thread_id, "run_parent", "call_parent", "delegate",
        "run_graph_parent", "frame_parent");
    json_value_t *child_inspect_json = NULL;
    json_value_t *child_orchestration_inspect_json = NULL;
    const json_value_t *child_run_json = NULL;
    const json_value_t *child_checkpoints_json = NULL;
    const json_value_t *child_latest_checkpoint_json = NULL;
    const json_value_t *child_checkpoint_context_json = NULL;
    const json_value_t *child_history_events_json = NULL;
    const json_value_t *child_trace_events_json = NULL;
    const json_value_t *child_thread_timeline_json = NULL;
    const json_value_t *child_branch_tree_json = NULL;
    const json_value_t *child_orchestration_child_inspect_json = NULL;

    if (child_output_item &&
        turbo_agent_runtime_remote_client_get_child_inspect(state->client, child_output_item,
                                                            &child_inspect_json) == 0 &&
        child_inspect_json) {
      child_run_json = turbo_json_object_get(child_inspect_json, "run");
      child_checkpoints_json = turbo_json_object_get(child_inspect_json, "checkpoints");
      child_latest_checkpoint_json = turbo_json_object_get(child_inspect_json, "latest_checkpoint");
      child_checkpoint_context_json =
          turbo_json_object_get(child_inspect_json, "checkpoint_context");
      child_history_events_json = turbo_json_object_get(child_inspect_json, "history_events");
      child_trace_events_json = turbo_json_object_get(child_inspect_json, "trace_events");
      child_thread_timeline_json = turbo_json_object_get(child_inspect_json, "thread_timeline");
      child_branch_tree_json = turbo_json_object_get(child_inspect_json, "branch_tree");
      if (child_run_json && child_checkpoints_json && child_latest_checkpoint_json &&
          child_checkpoint_context_json && child_history_events_json && child_trace_events_json &&
          child_thread_timeline_json && child_branch_tree_json &&
          strcmp(turbo_json_get_string(child_run_json, "id"), original_run_id) == 0 &&
          turbo_json_type(child_checkpoints_json) == TURBO_JSON_ARRAY &&
          turbo_json_array_size(child_checkpoints_json) == 1 &&
          strcmp(turbo_json_get_string(child_latest_checkpoint_json, "id"), checkpoint_id) == 0 &&
          strcmp(turbo_json_get_string(turbo_json_object_get(child_checkpoint_context_json, "thread"),
                                       "id"),
                 child_thread_id) == 0 &&
          turbo_json_type(child_history_events_json) == TURBO_JSON_ARRAY &&
          turbo_json_array_size(child_history_events_json) >= 1 &&
          turbo_json_type(child_trace_events_json) == TURBO_JSON_ARRAY) {
        state->child_inspect_ok = 1;
      }
    }

    if (child_output_item &&
        turbo_agent_runtime_remote_client_get_child_orchestration_inspect(
            state->client, child_output_item, &child_orchestration_inspect_json) == 0 &&
        child_orchestration_inspect_json &&
        strcmp(turbo_json_get_string(child_orchestration_inspect_json, "parent_agent_run_id"),
               "run_parent") == 0 &&
        strcmp(turbo_json_get_string(child_orchestration_inspect_json, "parent_tool_call_id"),
               "call_parent") == 0 &&
        strcmp(turbo_json_get_string(child_orchestration_inspect_json, "parent_tool_name"),
               "delegate") == 0 &&
        strcmp(turbo_json_get_string(child_orchestration_inspect_json, "parent_graph_run_id"),
               "run_graph_parent") == 0 &&
        strcmp(turbo_json_get_string(child_orchestration_inspect_json, "call_frame_id"),
               "frame_parent") == 0 &&
        (child_orchestration_child_inspect_json =
             turbo_json_object_get(child_orchestration_inspect_json, "child_inspect")) != NULL &&
        strcmp(turbo_json_get_string(turbo_json_object_get(child_orchestration_child_inspect_json,
                                                           "run"),
                                     "id"),
               original_run_id) == 0) {
      state->child_orchestration_inspect_ok = 1;
    }

    turbo_free_json(&child_orchestration_inspect_json);
    turbo_free_json(&child_inspect_json);
    turbo_free_json(&child_output_item);
  }

  state_json_value = create_remote_client_supervisor_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-client", state_json_value, &interrupt_options,
          "thr_remote_client_multi_agent", &summary_json, &resumed_state, &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    const char *supervisor_run_id = turbo_json_get_string(summary_json, "run_id");
    const char *supervisor_checkpoint_id = turbo_json_get_string(summary_json, "checkpoint_id");
    json_value_t *child_output_item = NULL;

    if (supervisor_run_id && supervisor_checkpoint_id &&
        turbo_agent_runtime_remote_client_get_supervisor_inspect(
            state->client, "thr_remote_client_multi_agent", &supervisor_inspect_json) == 0 &&
        supervisor_inspect_json) {
      turbo_agent_test_check_supervisor_inspect(supervisor_inspect_json, "planner", "executor",
                                                "delegate execution", 0, 1);
      state->supervisor_inspect_ok = 1;
      state->requested_handoff_event_ok = remote_client_check_latest_handoff_event(
          supervisor_inspect_json, "requested", "planner", "executor", "delegate execution",
          "planner");
    }
    if (state->supervisor_inspect_ok &&
        turbo_agent_runtime_remote_client_get_orchestration_inspect(
            state->client, "thr_remote_client_multi_agent", &orchestration_inspect_json) == 0 &&
        orchestration_inspect_json) {
      turbo_agent_test_check_orchestration_inspect(
          orchestration_inspect_json, "thr_remote_client_multi_agent", supervisor_run_id,
          supervisor_checkpoint_id, 0, "planner", "executor", "delegate execution");
      state->orchestration_inspect_ok = remote_client_check_orchestration_latest_handoff_event(
          orchestration_inspect_json, "requested", "planner", "executor", "delegate execution",
          "planner");
    }

    child_output_item = create_remote_client_child_output_item(
        supervisor_run_id, supervisor_checkpoint_id, "thr_remote_client_multi_agent", "run_parent",
        "call_parent", "delegate", "run_parent", "call_parent");
    if (state->orchestration_inspect_ok && child_output_item &&
        turbo_agent_runtime_remote_client_get_child_multi_agent_inspect(
            state->client, "thr_remote_client_multi_agent", child_output_item,
            &child_multi_agent_inspect_json) == 0 &&
        child_multi_agent_inspect_json) {
      const json_value_t *nested_supervisor_inspect =
          turbo_json_object_get(child_multi_agent_inspect_json, "supervisor_inspect");
      const json_value_t *nested_orchestration_inspect =
          turbo_json_object_get(child_multi_agent_inspect_json, "orchestration_inspect");
      const json_value_t *nested_child_orchestration_inspect =
          turbo_json_object_get(child_multi_agent_inspect_json, "child_orchestration_inspect");
      const json_value_t *nested_child_inspect =
          nested_child_orchestration_inspect
              ? turbo_json_object_get(nested_child_orchestration_inspect, "child_inspect")
              : NULL;

      if (nested_supervisor_inspect && nested_orchestration_inspect &&
          nested_child_orchestration_inspect && nested_child_inspect) {
        check_str_eq(
            turbo_json_get_string(nested_child_orchestration_inspect, "parent_agent_run_id"),
            "run_parent");
        check_str_eq(
            turbo_json_get_string(nested_child_orchestration_inspect, "parent_tool_call_id"),
            "call_parent");
        check_str_eq(turbo_json_get_string(nested_child_orchestration_inspect, "parent_tool_name"),
                     "delegate");
        check_str_eq(
            turbo_json_get_string(nested_child_orchestration_inspect, "parent_graph_run_id"),
            "run_parent");
        check_str_eq(turbo_json_get_string(nested_child_orchestration_inspect, "call_frame_id"),
                     "call_parent");
        check_str_eq(turbo_json_get_string(turbo_json_object_get(nested_child_inspect, "run"), "id"),
                     supervisor_run_id);
        check_str_eq(turbo_json_get_string(
                         turbo_json_object_get(nested_child_inspect, "latest_checkpoint"), "id"),
                     supervisor_checkpoint_id);
        state->child_multi_agent_inspect_ok = 1;
      }
    }
    turbo_free_json(&child_output_item);
  }
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&supervisor_inspect_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&child_multi_agent_inspect_json);
  turbo_free_json(&error_json);

  state_json_value = create_remote_client_committed_supervisor_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-client", state_json_value, &interrupt_options,
          "thr_remote_client_multi_agent_committed", &summary_json, &resumed_state, &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    const char *committed_run_id = turbo_json_get_string(summary_json, "run_id");
    const char *committed_checkpoint_id = turbo_json_get_string(summary_json, "checkpoint_id");

    if (committed_run_id && committed_checkpoint_id &&
        turbo_agent_runtime_remote_client_get_supervisor_inspect(
            state->client, "thr_remote_client_multi_agent_committed", &supervisor_inspect_json) == 0 &&
        supervisor_inspect_json &&
        remote_client_check_latest_handoff_event(supervisor_inspect_json, "committed", "planner",
                                                 "executor", "delegate execution", "executor")) {
      state->committed_handoff_event_ok = 1;
    }
    if (state->committed_handoff_event_ok &&
        turbo_agent_runtime_remote_client_get_orchestration_inspect(
            state->client, "thr_remote_client_multi_agent_committed",
            &orchestration_inspect_json) == 0 &&
        orchestration_inspect_json) {
      turbo_agent_test_check_orchestration_inspect(
          orchestration_inspect_json, "thr_remote_client_multi_agent_committed", committed_run_id,
          committed_checkpoint_id, 0, "executor", "", "");
      state->committed_handoff_event_ok =
          remote_client_check_orchestration_latest_handoff_event(
              orchestration_inspect_json, "committed", "planner", "executor",
              "delegate execution", "executor");
    }
  }
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&supervisor_inspect_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&error_json);

  state_json_value = create_remote_client_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-client", state_json_value, &interrupt_options,
          "thr_remote_client_thread_command_resume", &summary_json, &resumed_state, &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    thread_command_run_id = turbo_json_get_string(summary_json, "run_id");
    command = create_remote_client_command_json_value("append_user_message", "resume from thread command");
    if (thread_command_run_id &&
        turbo_agent_runtime_remote_client_resume_thread_command_json_value(
            state->client, "remote-client", "thr_remote_client_thread_command_resume", command,
            NULL, &resumed_summary, &thread_state, &error_json) == 0 &&
        resumed_summary && thread_state &&
        strcmp(turbo_json_get_string(resumed_summary, "status"), "completed") == 0 &&
        strcmp(turbo_json_get_string(resumed_summary, "run_id"), thread_command_run_id) == 0 &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(thread_state, "visited_end"), 0)) {
      state->thread_command_resume_ok = 1;
    }
  }
  turbo_runtime_json_destroy(command);
  command = NULL;
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(thread_state);
  thread_state = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&resumed_summary);
  turbo_free_json(&error_json);

  state_json_value = create_remote_client_state_json_value();
  if (turbo_agent_runtime_remote_client_start_json_value_graph(
          state->client, "remote-client", state_json_value, &interrupt_options,
          "thr_remote_client_thread_command_fork", &summary_json, &resumed_state, &error_json) == 0 &&
      summary_json && resumed_state && !error_json) {
    thread_command_fork_run_id = turbo_json_get_string(summary_json, "run_id");
    command = create_remote_client_command_json_value("append_user_message", "fork from thread command");
    if (thread_command_fork_run_id &&
        turbo_agent_runtime_remote_client_fork_thread_command_json_value(
            state->client, "remote-client", "thr_remote_client_thread_command_fork", command,
            NULL, &forked_summary, &forked_state, &error_json) == 0 &&
        forked_summary && forked_state &&
        strcmp(turbo_json_get_string(forked_summary, "status"), "completed") == 0 &&
        strcmp(turbo_json_get_string(forked_summary, "run_id"), thread_command_fork_run_id) != 0 &&
        turbo_runtime_json_value_as_bool(
            turbo_json_object_get(forked_state, "visited_end"), 0)) {
      state->thread_command_fork_ok = 1;
    }
  }
  turbo_runtime_json_destroy(command);
  command = NULL;
  turbo_runtime_json_destroy(state_json_value);
  state_json_value = NULL;
  turbo_runtime_json_destroy(forked_state);
  forked_state = NULL;
  turbo_runtime_json_destroy(resumed_state);
  resumed_state = NULL;
  turbo_free_json(&summary_json);
  turbo_free_json(&forked_summary);
  turbo_free_json(&error_json);

  params_json = create_remote_client_start_params_json();
  if (turbo_agent_runtime_remote_client_call_json(state->client, "runtime.start", params_json,
                                                  &result_json, &error_json) == 0 &&
      result_json && !error_json) {
    summary_json = turbo_json_object_get(result_json, "summary");
    state_json = turbo_json_object_get(result_json, "state");
    status = summary_json ? turbo_json_get_string(summary_json, "status") : NULL;
    if (summary_json && state_json && status && strcmp(status, "interrupted") == 0 &&
        turbo_json_get_bool(state_json, "visited_start", false) &&
        !turbo_json_get_bool(state_json, "visited_end", false)) {
      state->direct_call_ok = 1;
    }
  }
  turbo_free_json(&params_json);
  turbo_free_json(&result_json);
  turbo_free_json(&error_json);

  empty_params_json = turbo_json_create_object();
  check_not_null(empty_params_json);
  if (turbo_agent_runtime_remote_client_call_json(state->client, "runtime.unknownMethod",
                                                  empty_params_json, &result_json, &error_json) == 0 &&
      !result_json && error_json) {
    error_message = turbo_json_get_string(error_json, "message");
    if (turbo_json_get_int(error_json, "code", 0) == REMOTE_CLIENT_METHOD_NOT_FOUND &&
        error_message && strcmp(error_message, "Method not found") == 0 &&
        !turbo_json_get_bool(error_json, "transport_error", true)) {
      state->method_not_found_ok = 1;
    }
  }
  turbo_free_json(&empty_params_json);
  turbo_free_json(&result_json);
  turbo_free_json(&error_json);
  free(original_run_id);
  free(checkpoint_id);

  remote_client_test_state_cleanup(state);
}

spec("turbo agent runtime remote client api") {
  remote_client_test_state_t state = {0};

  before_each() {
    memset(&state, 0, sizeof(state));
  }

  after_each() {
    remote_client_test_state_cleanup(&state);
  }

  it("should call one mounted CHTTP runtime endpoint through the remote client bridge") {
    remote_runtime_remote_client_test_coro(&state);

    check_true(state.direct_call_ok);
    check_true(state.helper_run_ok);
    check_true(state.inspect_ok);
    check_true(state.record_helpers_ok);
    check_true(state.memory_helpers_ok);
    check_true(state.timeline_ok);
    check_true(state.branch_tree_ok);
    check_true(state.observability_ok);
    check_true(state.list_indexes_ok);
    check_true(state.supervisor_inspect_ok);
    check_true(state.orchestration_inspect_ok);
    check_true(state.requested_handoff_event_ok);
    check_true(state.committed_handoff_event_ok);
    check_true(state.child_runs_ok);
    check_true(state.child_inspect_ok);
    check_true(state.child_orchestration_inspect_ok);
    check_true(state.child_multi_agent_inspect_ok);
    check_true(state.thread_command_resume_ok);
    check_true(state.thread_command_fork_ok);
    check_true(state.method_not_found_ok);
  }

  it("should keep remote multi-agent inspect golden fixtures parseable") {
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
}
