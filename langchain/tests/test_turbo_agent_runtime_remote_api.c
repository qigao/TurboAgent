#include "tinytest.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_state.h"
#include "turbo_agent_test_support.h"
#include <json_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *key;
  int value;
} remote_bool_write_t;

typedef struct {
  const char *name;
  turbo_graph_t *graph;
} remote_graph_registry_t;

static char *remote_strdup(const char *text) {
  size_t len;
  char *copy;

  if (!text) {
    return NULL;
  }
  len = strlen(text);
  copy = (char *)calloc(len + 1, sizeof(*copy));
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len);
  copy[len] = '\0';
  return copy;
}

static int remote_write_bool_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  remote_bool_write_t *write = (remote_bool_write_t *)user_data;
  json_value_t *value;

  value = json_create_bool(write->value);
  check_not_null(value);
  return turbo_runtime_json_object_set(ctx->json_value_state, write->key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static turbo_graph_t *remote_graph_resolver(const char *graph_name, void *user_data) {
  remote_graph_registry_t *registry = (remote_graph_registry_t *)user_data;

  if (!registry || !graph_name || !registry->name || !registry->graph) {
    return NULL;
  }
  return strcmp(graph_name, registry->name) == 0 ? registry->graph : NULL;
}

static turbo_graph_t *create_remote_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("remote-skeleton");
  static remote_bool_write_t start = {"visited_start", 1};
  static remote_bool_write_t end = {"visited_end", 1};

  check_not_null(graph);
  check_equal(turbo_graph_add_json_value_node(graph, "start", remote_write_bool_json_value_node, &start),
               TURBO_GRAPH_EXEC_OK);
  check_equal(turbo_graph_add_json_value_node(graph, "end", remote_write_bool_json_value_node, &end),
               TURBO_GRAPH_EXEC_OK);
  check_equal(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL),
               TURBO_GRAPH_EXEC_OK);
  check_equal(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
  return graph;
}

static json_value_t *create_remote_state_json(void) {
  json_value_t *state = turbo_agent_state_create_json_value();
  json_value_t *state_json;

  check_not_null(state);
  state_json = json_clone(state);
  turbo_runtime_json_destroy(state);
  return state_json;
}

static json_value_t *create_remote_memory_record_variant(const json_value_t *record_fixture,
                                                        const char *key, const char *text) {
  json_value_t *record_json;
  char record_id[128];
  const char *record_namespace;

  check_not_null(record_fixture);
  check_not_null(key);
  check_not_null(text);
  record_namespace = json_get_string(record_fixture, "namespace");
  check_not_null(record_namespace);
  record_json = json_clone(record_fixture);
  check_not_null(record_json);
  check_true(snprintf(record_id, sizeof(record_id), "%s::%s", record_namespace, key) > 0);
  json_object_set_string(record_json, "id", record_id);
  json_object_set_string(record_json, "key", key);
  json_object_set_string(record_json, "text", text);
  return record_json;
}

static json_value_t *create_remote_jsonrpc_request(const char *id, const char *method,
                                                   const json_value_t *params_json) {
  json_value_t *request_json = json_create_object();
  json_value_t *params_clone;

  check_not_null(request_json);
  json_object_set_string(request_json, "jsonrpc", "2.0");
  json_object_set_string(request_json, "id", id);
  json_object_set_string(request_json, "method", method);
  if (params_json) {
    params_clone = json_clone(params_json);
    check_not_null(params_clone);
    json_object_add(request_json, "params", params_clone);
  }
  return request_json;
}

static json_value_t *parse_remote_json_text(const char *json_text) {
  json_value_t *json = NULL;

  check_not_null(json_text);
  json = json_parse(json_text, strlen(json_text));
    check_not_null(json);
  return json;
}

static json_value_t *create_remote_jsonrpc_request_from_fixture(const char *fixture_name,
                                                                const char *fixture_entry_name) {
  json_value_t *fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  const json_value_t *fixture =
      turbo_agent_test_find_named_fixture_entry(fixture_root, fixture_entry_name);
  const json_value_t *params_json;
  json_value_t *request_json;

  check_not_null(fixture);
  check_true(json_type(fixture) == JSON_OBJECT);
  params_json = json_object_get(fixture, "params");
  request_json = create_remote_jsonrpc_request(json_get_string(fixture, "id"),
                                               json_get_string(fixture, "method"),
                                               params_json);
  json_free(fixture_root); fixture_root = NULL;
  return request_json;
}

static void check_remote_jsonrpc_success(const json_value_t *response_json, const char *id) {
  check_not_null(response_json);
  check_true(json_type(response_json) == JSON_OBJECT);
  check_equal(json_get_string(response_json, "jsonrpc"), "2.0");
  check_equal(json_get_string(response_json, "id"), id);
  check_not_null(json_object_get(response_json, "result"));
  check_true(json_is_null(json_object_get(response_json, "error")));
}

static void check_remote_jsonrpc_error(const json_value_t *response_json, const char *id,
                                       int code) {
  const json_value_t *error_json;

  check_not_null(response_json);
  check_true(json_type(response_json) == JSON_OBJECT);
  check_equal(json_get_string(response_json, "jsonrpc"), "2.0");
  check_equal(json_get_string(response_json, "id"), id);
  check_true(json_is_null(json_object_get(response_json, "result")));
  error_json = json_object_get(response_json, "error");
  check_not_null(error_json);
  check_equal(json_get_int(error_json, "code", 0), code);
}

static void check_remote_jsonrpc_request_fixture(const json_value_t *request_json,
                                                 const char *fixture_name,
                                                 const char *fixture_entry_name) {
  json_value_t *fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  const json_value_t *fixture =
      turbo_agent_test_find_named_fixture_entry(fixture_root, fixture_entry_name);
  const json_value_t *params_fixture;
  const json_value_t *params_json;
  const json_value_t *options_fixture;
  const json_value_t *filters_fixture;
  const json_value_t *command_fixture;
  const json_value_t *state_fixture;
  const json_value_t *state_patch_fixture;

  check_not_null(request_json);
  check_not_null(fixture);
  check_true(json_type(fixture) == JSON_OBJECT);
  check_equal(json_get_string(request_json, "jsonrpc"), "2.0");
  check_equal(json_get_string(request_json, "id"), json_get_string(fixture, "id"));
  check_equal(json_get_string(request_json, "method"),
               json_get_string(fixture, "method"));

  params_fixture = json_object_get(fixture, "params");
  params_json = json_object_get(request_json, "params");
  if (params_fixture) {
    check_true(json_type(params_json) == JSON_OBJECT);
    if (json_get_string(params_fixture, "graph_name")) {
      check_equal(json_get_string(params_json, "graph_name"),
                   json_get_string(params_fixture, "graph_name"));
    }
    if (json_get_string(params_fixture, "thread_id")) {
      check_equal(json_get_string(params_json, "thread_id"),
                   json_get_string(params_fixture, "thread_id"));
    }
    if (json_get_string(params_fixture, "checkpoint_id")) {
      check_equal(json_get_string(params_json, "checkpoint_id"),
                   json_get_string(params_fixture, "checkpoint_id"));
    }
    if (json_get_bool(params_fixture, "checkpoint_id_required", false)) {
      check_not_null(json_get_string(params_json, "checkpoint_id"));
    }
    if (json_get_bool(params_fixture, "run_id_required", false)) {
      check_not_null(json_get_string(params_json, "run_id"));
    }

    options_fixture = json_object_get(params_fixture, "options");
    if (options_fixture && json_type(options_fixture) == JSON_OBJECT) {
      const json_value_t *expected_nodes =
          json_object_get(options_fixture, "interrupt_before_nodes");
      const json_value_t *actual_options = json_object_get(params_json, "options");
      const json_value_t *actual_nodes =
          actual_options ? json_object_get(actual_options, "interrupt_before_nodes") : NULL;
      if (expected_nodes) {
        check_true(json_type(actual_nodes) == JSON_ARRAY);
        check_equal(json_array_size(actual_nodes), json_array_size(expected_nodes));
        check_equal(json_string(json_array_get(actual_nodes, 0)),
                     json_string(json_array_get(expected_nodes, 0)));
      }
    }

    filters_fixture = json_object_get(params_fixture, "filters");
    if (filters_fixture && json_type(filters_fixture) == JSON_OBJECT) {
      const json_value_t *actual_filters = json_object_get(params_json, "filters");
      check_true(json_type(actual_filters) == JSON_OBJECT);
      if (json_get_string(filters_fixture, "status")) {
        check_equal(json_get_string(actual_filters, "status"),
                     json_get_string(filters_fixture, "status"));
      }
      if (json_get_string(filters_fixture, "thread_id_prefix")) {
        check_equal(json_get_string(actual_filters, "thread_id_prefix"),
                     json_get_string(filters_fixture, "thread_id_prefix"));
      }
      if (json_object_get(filters_fixture, "limit")) {
        check_equal(json_get_int(actual_filters, "limit", 0),
                     json_get_int(filters_fixture, "limit", 0));
      }
    }

    command_fixture = json_object_get(params_fixture, "command");
    if (command_fixture && json_type(command_fixture) == JSON_OBJECT) {
      const json_value_t *actual_command = json_object_get(params_json, "command");
      check_true(json_type(actual_command) == JSON_OBJECT);
      if (json_get_string(command_fixture, "kind")) {
        check_equal(json_get_string(actual_command, "kind"),
                     json_get_string(command_fixture, "kind"));
      }
      if (json_get_string(command_fixture, "text")) {
        check_equal(json_get_string(actual_command, "text"),
                     json_get_string(command_fixture, "text"));
      }
    }

    state_fixture = json_object_get(params_fixture, "state");
    if (state_fixture && json_type(state_fixture) == JSON_OBJECT) {
      const json_value_t *actual_state = json_object_get(params_json, "state");
      check_true(json_type(actual_state) == JSON_OBJECT);
      if (json_object_get(state_fixture, "patched")) {
        check_true(json_get_bool(actual_state, "patched", false) ==
                   json_get_bool(state_fixture, "patched", false));
      }
      if (json_object_get(state_fixture, "patched_via_alias")) {
        check_true(json_get_bool(actual_state, "patched_via_alias", false) ==
                   json_get_bool(state_fixture, "patched_via_alias", false));
      }
      if (json_object_get(state_fixture, "patched_via_state_patch")) {
        check_true(json_get_bool(actual_state, "patched_via_state_patch", false) ==
                   json_get_bool(state_fixture, "patched_via_state_patch", false));
      }
    }

    state_patch_fixture = json_object_get(params_fixture, "state_patch");
    if (state_patch_fixture && json_type(state_patch_fixture) == JSON_OBJECT) {
      const json_value_t *actual_state_patch = json_object_get(params_json, "state_patch");
      check_true(json_type(actual_state_patch) == JSON_OBJECT);
      if (json_object_get(state_patch_fixture, "patched")) {
        check_true(json_get_bool(actual_state_patch, "patched", false) ==
                   json_get_bool(state_patch_fixture, "patched", false));
      }
      if (json_object_get(state_patch_fixture, "patched_via_alias")) {
        check_true(json_get_bool(actual_state_patch, "patched_via_alias", false) ==
                   json_get_bool(state_patch_fixture, "patched_via_alias", false));
      }
      if (json_object_get(state_patch_fixture, "patched_via_state_patch")) {
        check_true(json_get_bool(actual_state_patch, "patched_via_state_patch", false) ==
                   json_get_bool(state_patch_fixture, "patched_via_state_patch", false));
      }
    }
  }

  json_free(fixture_root); fixture_root = NULL;
}

static void check_remote_jsonrpc_success_fixture(const json_value_t *response_json,
                                                 const char *fixture_name,
                                                 const char *fixture_entry_name) {
  json_value_t *fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  const json_value_t *fixture =
      turbo_agent_test_find_named_fixture_entry(fixture_root, fixture_entry_name);
  const json_value_t *result_json;
  const json_value_t *summary_fixture;
  const json_value_t *state_fixture;
  const json_value_t *run_fixture;
  const json_value_t *checkpoint_fixture;
  const json_value_t *checkpoints_fixture;
  const json_value_t *events_fixture;
  const json_value_t *index_fixture;
  const json_value_t *indexes_fixture;
  const json_value_t *context_fixture;

  check_not_null(fixture);
  check_true(json_type(fixture) == JSON_OBJECT);
  check_remote_jsonrpc_success(response_json, json_get_string(fixture, "id"));
  result_json = json_object_get(response_json, "result");
  check_true(json_type(result_json) == JSON_OBJECT);

  summary_fixture = json_object_get(fixture, "summary");
  if (summary_fixture && json_type(summary_fixture) == JSON_OBJECT) {
    const json_value_t *summary_json = json_object_get(result_json, "summary");
    check_true(json_type(summary_json) == JSON_OBJECT);
    if (json_get_string(summary_fixture, "thread_id")) {
      check_equal(json_get_string(summary_json, "thread_id"),
                   json_get_string(summary_fixture, "thread_id"));
    }
    if (json_get_string(summary_fixture, "status")) {
      check_equal(json_get_string(summary_json, "status"),
                   json_get_string(summary_fixture, "status"));
    }
  }

  state_fixture = json_object_get(fixture, "state");
  if (state_fixture && json_type(state_fixture) == JSON_OBJECT) {
    const json_value_t *state_json = json_object_get(result_json, "state");
    check_true(json_type(state_json) == JSON_OBJECT);
    if (json_object_get(state_fixture, "visited_start")) {
      check_true(json_get_bool(state_json, "visited_start", false) ==
                 json_get_bool(state_fixture, "visited_start", false));
    }
    if (json_object_get(state_fixture, "visited_end")) {
      check_true(json_get_bool(state_json, "visited_end", false) ==
                 json_get_bool(state_fixture, "visited_end", false));
    }
    if (json_object_get(state_fixture, "patched")) {
      check_true(json_get_bool(state_json, "patched", false) ==
                 json_get_bool(state_fixture, "patched", false));
    }
    if (json_object_get(state_fixture, "patched_via_alias")) {
      check_true(json_get_bool(state_json, "patched_via_alias", false) ==
                 json_get_bool(state_fixture, "patched_via_alias", false));
    }
    if (json_object_get(state_fixture, "input_count")) {
      const json_value_t *input_json = json_object_get(state_json, "input");
      const json_value_t *last_message_fixture = json_object_get(state_fixture, "last_message");
      const json_value_t *last_message_json;
      check_true(json_type(input_json) == JSON_ARRAY);
      check_equal(json_array_size(input_json),
                    (size_t)json_get_int(state_fixture, "input_count", 0));
      if (last_message_fixture && json_type(last_message_fixture) == JSON_OBJECT) {
        last_message_json = json_array_get(input_json, json_array_size(input_json) - 1);
        check_true(json_type(last_message_json) == JSON_OBJECT);
        check_equal(json_get_string(last_message_json, "role"),
                     json_get_string(last_message_fixture, "role"));
        check_equal(json_get_string(last_message_json, "content"),
                     json_get_string(last_message_fixture, "content"));
      }
    }
  }

  run_fixture = json_object_get(fixture, "run");
  if (run_fixture && json_type(run_fixture) == JSON_OBJECT) {
    const json_value_t *run_json = json_object_get(result_json, "run");

    check_true(json_type(run_json) == JSON_OBJECT);
    if (json_get_bool(run_fixture, "id_required", false)) {
      check_not_null(json_get_string(run_json, "id"));
    }
    if (json_get_string(run_fixture, "id")) {
      check_equal(json_get_string(run_json, "id"),
                   json_get_string(run_fixture, "id"));
    }
    if (json_get_bool(run_fixture, "thread_id_required", false)) {
      check_not_null(json_get_string(run_json, "thread_id"));
    }
    if (json_get_string(run_fixture, "thread_id")) {
      check_equal(json_get_string(run_json, "thread_id"),
                   json_get_string(run_fixture, "thread_id"));
    }
  }

  checkpoint_fixture = json_object_get(fixture, "checkpoint");
  if (checkpoint_fixture && json_type(checkpoint_fixture) == JSON_OBJECT) {
    const json_value_t *checkpoint_json = json_object_get(result_json, "checkpoint");

    check_true(json_type(checkpoint_json) == JSON_OBJECT);
    if (json_get_bool(checkpoint_fixture, "id_required", false)) {
      check_not_null(json_get_string(checkpoint_json, "id"));
    }
    if (json_get_string(checkpoint_fixture, "id")) {
      check_equal(json_get_string(checkpoint_json, "id"),
                   json_get_string(checkpoint_fixture, "id"));
    }
    if (json_get_bool(checkpoint_fixture, "run_id_required", false)) {
      check_not_null(json_get_string(checkpoint_json, "run_id"));
    }
    if (json_get_string(checkpoint_fixture, "run_id")) {
      check_equal(json_get_string(checkpoint_json, "run_id"),
                   json_get_string(checkpoint_fixture, "run_id"));
    }
  }

  checkpoints_fixture = json_object_get(fixture, "checkpoints");
  if (checkpoints_fixture && json_type(checkpoints_fixture) == JSON_OBJECT) {
    const json_value_t *checkpoints_json = json_object_get(result_json, "checkpoints");
    const json_value_t *expected_checkpoint_ids =
        json_object_get(checkpoints_fixture, "expected_checkpoint_ids");
    size_t i;

    check_true(json_type(checkpoints_json) == JSON_ARRAY);
    if (json_object_get(checkpoints_fixture, "expected_count")) {
      check_equal(json_array_size(checkpoints_json),
                    (size_t)json_get_int(checkpoints_fixture, "expected_count", 0));
    }
    if (json_get_bool(checkpoints_fixture, "first_id_required", false)) {
      check_true(json_array_size(checkpoints_json) > 0);
      check_not_null(json_get_string(json_array_get(checkpoints_json, 0), "id"));
    }
    if (json_get_bool(checkpoints_fixture, "first_run_id_required", false)) {
      check_true(json_array_size(checkpoints_json) > 0);
      check_not_null(json_get_string(json_array_get(checkpoints_json, 0), "run_id"));
    }
    if (expected_checkpoint_ids && json_type(expected_checkpoint_ids) == JSON_ARRAY) {
      check_equal(json_array_size(checkpoints_json), json_array_size(expected_checkpoint_ids));
      for (i = 0; i < json_array_size(expected_checkpoint_ids); ++i) {
        const json_value_t *checkpoint_json = json_array_get(checkpoints_json, i);
        check_true(json_type(checkpoint_json) == JSON_OBJECT);
        check_equal(json_get_string(checkpoint_json, "id"),
                     json_string(json_array_get(expected_checkpoint_ids, i)));
      }
    }
  }

  events_fixture = json_object_get(fixture, "events");
  if (events_fixture && json_type(events_fixture) == JSON_OBJECT) {
    const json_value_t *events_json = json_object_get(result_json, "events");

    check_true(json_type(events_json) == JSON_ARRAY);
    if (json_object_get(events_fixture, "count")) {
      check_equal(json_array_size(events_json),
                    (size_t)json_get_int(events_fixture, "count", 0));
    }
    if (json_object_get(events_fixture, "min")) {
      check_true(json_array_size(events_json) >=
                 (size_t)json_get_int(events_fixture, "min", 0));
    }
  }

  index_fixture = json_object_get(fixture, "index");
  if (index_fixture && json_type(index_fixture) == JSON_OBJECT) {
    const json_value_t *index_json = json_object_get(result_json, "index");
    const json_value_t *thread_json;
    check_true(json_type(index_json) == JSON_OBJECT);
    if (json_get_string(index_fixture, "thread_id")) {
      thread_json = json_object_get(index_json, "thread");
      check_true(json_type(thread_json) == JSON_OBJECT);
      check_equal(json_get_string(thread_json, "id"),
                   json_get_string(index_fixture, "thread_id"));
    }
    if (json_get_string(index_fixture, "current_status")) {
      check_equal(json_get_string(index_json, "current_status"),
                   json_get_string(index_fixture, "current_status"));
    }
    if (json_get_bool(index_fixture, "current_checkpoint_summary_required", false)) {
      const json_value_t *checkpoint_summary =
          json_object_get(index_json, "current_checkpoint_summary");
      check_true(json_type(checkpoint_summary) == JSON_OBJECT);
      check_not_null(json_get_string(checkpoint_summary, "id"));
    }
  }

  indexes_fixture = json_object_get(fixture, "indexes");
  if (indexes_fixture && json_type(indexes_fixture) == JSON_OBJECT) {
    const json_value_t *indexes_json = json_object_get(result_json, "indexes");
    const json_value_t *expected_thread_ids =
        json_object_get(indexes_fixture, "expected_thread_ids");
    size_t i;
    check_true(json_type(indexes_json) == JSON_ARRAY);
    check_true(json_type(expected_thread_ids) == JSON_ARRAY);
    check_equal(json_array_size(indexes_json), json_array_size(expected_thread_ids));
    for (i = 0; i < json_array_size(expected_thread_ids); ++i) {
      const json_value_t *entry = json_array_get(indexes_json, i);
      const json_value_t *thread_json = entry ? json_object_get(entry, "thread") : NULL;
      check_true(json_type(entry) == JSON_OBJECT);
      check_true(json_type(thread_json) == JSON_OBJECT);
      check_equal(json_get_string(thread_json, "id"),
                   json_string(json_array_get(expected_thread_ids, i)));
    }
  }

  context_fixture = json_object_get(fixture, "context");
  if (context_fixture && json_type(context_fixture) == JSON_OBJECT) {
    const json_value_t *context_json = json_object_get(result_json, "context");
    const json_value_t *thread_json;
    const json_value_t *run_json;
    const json_value_t *checkpoint_summary_json;
    const json_value_t *history_events_json;
    check_true(json_type(context_json) == JSON_OBJECT);
    if (json_get_string(context_fixture, "thread_id")) {
      thread_json = json_object_get(context_json, "thread");
      check_true(json_type(thread_json) == JSON_OBJECT);
      check_equal(json_get_string(thread_json, "id"),
                   json_get_string(context_fixture, "thread_id"));
    }
    if (json_get_bool(context_fixture, "checkpoint_summary_required", false)) {
      checkpoint_summary_json = json_object_get(context_json, "checkpoint_summary");
      check_true(json_type(checkpoint_summary_json) == JSON_OBJECT);
      check_not_null(json_get_string(checkpoint_summary_json, "id"));
    }
    if (json_get_bool(context_fixture, "run_id_required", false)) {
      run_json = json_object_get(context_json, "run");
      check_true(json_type(run_json) == JSON_OBJECT);
      check_not_null(json_get_string(run_json, "id"));
    }
    if (json_object_get(context_fixture, "history_events_min")) {
      history_events_json = json_object_get(context_json, "history_events");
      check_true(json_type(history_events_json) == JSON_ARRAY);
      check_true(json_array_size(history_events_json) >=
                 (size_t)json_get_int(context_fixture, "history_events_min", 0));
    }
  }

  json_free(fixture_root); fixture_root = NULL;
}

static void check_remote_jsonrpc_error_fixture(const json_value_t *response_json,
                                               const char *fixture_name,
                                               const char *fixture_entry_name) {
  json_value_t *fixture_root = turbo_agent_test_load_fixture_json(fixture_name);
  const json_value_t *fixture =
      turbo_agent_test_find_named_fixture_entry(fixture_root, fixture_entry_name);
  const json_value_t *error_json;

  check_not_null(fixture);
  check_true(json_type(fixture) == JSON_OBJECT);
  check_remote_jsonrpc_error(response_json, json_get_string(fixture, "id"),
                             json_get_int(fixture, "error_code", 0));
  error_json = json_object_get(response_json, "error");
  if (json_get_string(fixture, "error_message")) {
    check_equal(json_get_string(error_json, "message"),
                 json_get_string(fixture, "error_message"));
  }
  json_free(fixture_root); fixture_root = NULL;
}

spec("turbo agent runtime remote api") {
  it("should report startup diagnostics for remote graph resolution") {
    static const char *const graph_name = "remote-skeleton";
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_graph_t *graph = create_remote_graph();
    remote_graph_registry_t registry = {graph_name, graph};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *params_json = json_create_object();
    json_value_t *request_json = NULL;
    json_value_t *response_json = NULL;
    const json_value_t *result_json;
    const json_value_t *errors_json;
    const json_value_t *graph_json;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(params_json);
    config.runtime = runtime;
    config.graph_resolver = remote_graph_resolver;
    config.graph_resolver_user_data = &registry;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_string(params_json, "graph_name", graph_name);
    request_json = create_remote_jsonrpc_request(
        "req-startup-diagnostics", "runtime.getStartupDiagnostics", params_json);
    check_not_null(request_json);
    json_free(params_json); params_json = NULL;
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json,
                                                             &response_json),
                 0);
    result_json = json_object_get(response_json, "result");
    check_true(json_type(result_json) == JSON_OBJECT);
    errors_json = json_object_get(result_json, "errors");
    graph_json = json_object_get(result_json, "graph");
    check_equal(json_get_int(result_json, "schema_version", -1), 1);
    check_true(json_get_bool(result_json, "ok", false));
    check_equal(json_array_size(errors_json), 0);
    check_true(json_get_bool(json_object_get(result_json, "runtime"), "ok",
                                   false));
    check_equal(json_get_string(graph_json, "name"), graph_name);
    check_true(json_get_bool(graph_json, "resolvable", false));

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should dispatch start apply resume fork and observability requests over local json-rpc") {
    static const char *const graph_name = "remote-skeleton";
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    turbo_graph_t *graph = create_remote_graph();
    remote_graph_registry_t registry = {graph_name, graph};
    json_value_t *start_params = json_create_object();
    json_value_t *start_options = json_create_object();
    json_value_t *start_interrupt_before_nodes = json_create_array();
    json_value_t *start_interrupt_before_node = json_create_string("end");
    json_value_t *start_state_json = create_remote_state_json();
    json_value_t *request_json = NULL;
    json_value_t *response_json = NULL;
    const json_value_t *result_json;
    const json_value_t *summary_json;
    const json_value_t *state_json;
    const char *checkpoint_id;
    const char *run_id;
    const char *thread_id = "remote-thread-1";
    json_value_t *result_state = NULL;
    const char *thread2_id = "remote-thread-2";
    char *thread2_run_id = NULL;
    json_value_t *thread2_start_params = NULL;
    json_value_t *thread2_start_options = NULL;
    json_value_t *thread2_interrupt_before_nodes = NULL;
    json_value_t *thread2_interrupt_before_node = NULL;
    json_value_t *thread2_state_json = NULL;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(start_params);
    check_not_null(start_options);
    check_not_null(start_interrupt_before_nodes);
    check_not_null(start_interrupt_before_node);
    check_not_null(start_state_json);

    config.runtime = runtime;
    config.graph_resolver = remote_graph_resolver;
    config.graph_resolver_user_data = &registry;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_string(start_params, "graph_name", graph_name);
    json_object_set_string(start_params, "thread_id", thread_id);
    json_object_add(start_params, "state", start_state_json);
    json_array_add(start_interrupt_before_nodes, start_interrupt_before_node);
    json_object_add(start_options, "interrupt_before_nodes", start_interrupt_before_nodes);
    json_object_add(start_params, "options", start_options);

    request_json = create_remote_jsonrpc_request("req-start", "runtime.start", start_params);
    check_not_null(request_json);
    check_remote_jsonrpc_request_fixture(request_json,
                                         "runtime_remote_thread_control.golden.json",
                                         "start_request");
    json_free(start_params); start_params = NULL;
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success_fixture(response_json,
                                         "runtime_remote_thread_control.golden.json",
                                         "start_response");
    result_json = json_object_get(response_json, "result");
    check_true(json_type(result_json) == JSON_OBJECT);
    summary_json = json_object_get(result_json, "summary");
    state_json = json_object_get(result_json, "state");
    check_not_null(summary_json);
    check_not_null(state_json);
    check_equal(json_get_string(summary_json, "thread_id"), thread_id);
    check_equal(json_get_string(summary_json, "status"), "interrupted");
    check_not_null(json_get_string(summary_json, "checkpoint_id"));
    check_not_null(json_get_string(summary_json, "run_id"));
    check_true(json_get_bool(state_json, "visited_start", false));
    check_true(!json_get_bool(state_json, "visited_end", false));
    checkpoint_id = remote_strdup(json_get_string(summary_json, "checkpoint_id"));
    run_id = remote_strdup(json_get_string(summary_json, "run_id"));
    check_not_null(checkpoint_id);
    check_not_null(run_id);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;

    {
      json_value_t *state_params = json_create_object();

      check_not_null(state_params);
      json_object_set_string(state_params, "thread_id", thread_id);
      request_json = create_remote_jsonrpc_request("req-thread-state",
                                                   "runtime.getThreadState",
                                                   state_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "get_thread_state_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "get_thread_state_response");
      result_json = json_object_get(response_json, "result");
      state_json = json_object_get(result_json, "state");
      check_not_null(state_json);
      check_true(json_get_bool(state_json, "visited_start", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(state_params); state_params = NULL;
    }

    {
      json_value_t *update_params = json_create_object();
      json_value_t *state_patch_json = json_create_object();

      check_not_null(update_params);
      check_not_null(state_patch_json);
      json_object_set_string(update_params, "thread_id", thread_id);
      json_object_set_bool(state_patch_json, "patched", true);
      json_object_add(update_params, "state_patch", state_patch_json);
      request_json = create_remote_jsonrpc_request("req-thread-update",
                                                   "runtime.updateThreadState",
                                                   update_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "update_thread_state_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "update_thread_state_response");
      result_json = json_object_get(response_json, "result");
      state_json = json_object_get(result_json, "state");
      check_not_null(state_json);
      check_true(json_get_bool(state_json, "patched", false));
      check_true(json_get_bool(state_json, "visited_start", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(update_params); update_params = NULL;
    }

    {
      json_value_t *patch_params = json_create_object();
      json_value_t *state_patch_json = json_create_object();

      check_not_null(patch_params);
      check_not_null(state_patch_json);
      json_object_set_string(patch_params, "thread_id", thread_id);
      json_object_set_bool(state_patch_json, "patched_via_alias", true);
      json_object_add(patch_params, "state_patch", state_patch_json);
      request_json = create_remote_jsonrpc_request("req-thread-patch",
                                                   "runtime.applyThreadStatePatch",
                                                   patch_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "apply_thread_state_patch_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "apply_thread_state_patch_response");
      result_json = json_object_get(response_json, "result");
      state_json = json_object_get(result_json, "state");
      check_not_null(state_json);
      check_true(json_get_bool(state_json, "patched_via_alias", false));
      check_true(json_get_bool(state_json, "visited_start", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(patch_params); patch_params = NULL;
    }

    {
      json_value_t *apply_params = json_create_object();
      json_value_t *command_json = json_create_object();

      check_not_null(apply_params);
      check_not_null(command_json);
      json_object_set_string(apply_params, "checkpoint_id", checkpoint_id);
      json_object_set_string(command_json, "kind", "append_user_message");
      json_object_set_string(command_json, "text", "hello from remote");
      json_object_add(apply_params, "command", command_json);

      request_json = create_remote_jsonrpc_request("req-apply", "runtime.applyCommand",
                                                   apply_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "apply_command_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_thread_control.golden.json",
                                           "apply_command_response");
      result_json = json_object_get(response_json, "result");
      check_true(json_type(result_json) == JSON_OBJECT);
      state_json = json_object_get(result_json, "state");
      check_not_null(state_json);
      check_true(json_type(json_object_get(state_json, "input")) == JSON_ARRAY);
      check_equal(json_array_size(json_object_get(state_json, "input")), 1);
      check_true(json_get_bool(state_json, "visited_start", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(apply_params); apply_params = NULL;
    }

    {
      json_value_t *index_params = json_create_object();

      check_not_null(index_params);
      json_object_set_string(index_params, "thread_id", thread_id);
      request_json = create_remote_jsonrpc_request("req-index",
                                                   "runtime.getThreadObservabilityIndex",
                                                   index_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "thread_observability_index_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_inspect.golden.json",
                                           "thread_observability_index_response");
      result_json = json_object_get(response_json, "result");
      check_true(json_type(result_json) == JSON_OBJECT);
      state_json = json_object_get(result_json, "index");
      check_not_null(state_json);
      check_equal(json_get_string(json_object_get(state_json, "thread"), "id"),
                   thread_id);
      check_equal(json_get_string(state_json, "current_status"), "interrupted");
      check_equal(json_get_string(json_object_get(state_json,
                                                               "current_checkpoint_summary"),
                                         "id"),
                   checkpoint_id);

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(index_params); index_params = NULL;
    }

    {
      json_value_t *list_params = json_create_object();
      json_value_t *filters_json = json_create_object();

      check_not_null(list_params);
      check_not_null(filters_json);
      json_object_set_string(filters_json, "status", "interrupted");
      json_object_set_string(filters_json, "thread_id_prefix", "remote-thread-");
      json_object_set_number(filters_json, "limit", 1);
      json_object_add(list_params, "filters", filters_json);
      request_json = create_remote_jsonrpc_request("req-list",
                                                   "runtime.listObservabilityIndexesFiltered",
                                                   list_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "list_observability_indexes_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_inspect.golden.json",
                                           "list_observability_indexes_response");
      result_json = json_object_get(response_json, "result");
      check_true(json_type(result_json) == JSON_OBJECT);
      state_json = json_object_get(result_json, "indexes");
      check_not_null(state_json);
      check_true(json_type(state_json) == JSON_ARRAY);
      check_equal(json_array_size(state_json), 1);
      check_equal(json_get_string(json_object_get(json_array_get(state_json, 0),
                                                               "thread"),
                                         "id"),
                   thread_id);

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(list_params); list_params = NULL;
    }

    {
      json_value_t *resume_params = json_create_object();

      check_not_null(resume_params);
      json_object_set_string(resume_params, "graph_name", graph_name);
      json_object_set_string(resume_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-resume", "runtime.resume", resume_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "resume_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "resume_response");
      result_json = json_object_get(response_json, "result");
      check_true(json_type(result_json) == JSON_OBJECT);
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_equal(json_get_string(summary_json, "run_id"), run_id);
      check_true(json_get_bool(state_json, "visited_start", false));
      check_true(json_get_bool(state_json, "visited_end", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(resume_params); resume_params = NULL;
    }

    {
      json_value_t *fork_params = json_create_object();

      check_not_null(fork_params);
      json_object_set_string(fork_params, "graph_name", graph_name);
      json_object_set_string(fork_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-fork", "runtime.fork", fork_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "fork_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "fork_response");
      result_json = json_object_get(response_json, "result");
      check_true(json_type(result_json) == JSON_OBJECT);
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(strcmp(json_get_string(summary_json, "run_id"), run_id) != 0);
      check_true(json_get_bool(state_json, "visited_start", false));
      check_true(json_get_bool(state_json, "visited_end", false));

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(fork_params); fork_params = NULL;
    }

    {
      json_value_t *context_params = json_create_object();

      check_not_null(context_params);
      json_object_set_string(context_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-context",
                                                   "runtime.getCheckpointContext",
                                                   context_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "checkpoint_context_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_inspect.golden.json",
                                           "checkpoint_context_response");
      result_json = json_object_get(response_json, "result");
      state_json = json_object_get(result_json, "context");
      check_not_null(state_json);
      check_equal(json_get_string(json_object_get(state_json, "checkpoint_summary"),
                                         "id"),
                   checkpoint_id);
      check_equal(json_get_string(json_object_get(state_json, "thread"), "id"),
                   thread_id);
      check_true(json_type(json_object_get(state_json, "history_events")) ==
                 JSON_ARRAY);

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(context_params); context_params = NULL;
    }

    {
      json_value_t *run_params = json_create_object();

      check_not_null(run_params);
      json_object_set_string(run_params, "run_id", run_id);
      request_json = create_remote_jsonrpc_request("req-get-run", "runtime.getRun", run_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "get_run_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "get_run_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(run_params); run_params = NULL;
    }

    {
      json_value_t *checkpoint_params = json_create_object();

      check_not_null(checkpoint_params);
      json_object_set_string(checkpoint_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-get-checkpoint",
                                                   "runtime.getCheckpoint",
                                                   checkpoint_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "get_checkpoint_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "get_checkpoint_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(checkpoint_params); checkpoint_params = NULL;
    }

    {
      json_value_t *list_checkpoints_params = json_create_object();

      check_not_null(list_checkpoints_params);
      json_object_set_string(list_checkpoints_params, "run_id", run_id);
      request_json = create_remote_jsonrpc_request("req-list-checkpoints",
                                                   "runtime.listCheckpoints",
                                                   list_checkpoints_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "list_checkpoints_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "list_checkpoints_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(list_checkpoints_params); list_checkpoints_params = NULL;
    }

    {
      json_value_t *history_params = json_create_object();

      check_not_null(history_params);
      json_object_set_string(history_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-history",
                                                   "runtime.loadHistoryEvents",
                                                   history_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "load_history_events_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "load_history_events_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(history_params); history_params = NULL;
    }

    {
      json_value_t *trace_params = json_create_object();

      check_not_null(trace_params);
      json_object_set_string(trace_params, "checkpoint_id", checkpoint_id);
      request_json = create_remote_jsonrpc_request("req-checkpoint-trace",
                                                   "runtime.getCheckpointTraceEvents",
                                                   trace_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "get_checkpoint_trace_events_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "get_checkpoint_trace_events_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(trace_params); trace_params = NULL;
    }

    {
      json_value_t *trace_params = json_create_object();

      check_not_null(trace_params);
      json_object_set_string(trace_params, "run_id", run_id);
      request_json = create_remote_jsonrpc_request("req-run-trace",
                                                   "runtime.getRunTraceEvents",
                                                   trace_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_inspect.golden.json",
                                           "get_run_trace_events_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json, "runtime_remote_inspect.golden.json",
                                           "get_run_trace_events_response");

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(trace_params); trace_params = NULL;
    }

    thread2_start_params = json_create_object();
    thread2_start_options = json_create_object();
    thread2_interrupt_before_nodes = json_create_array();
    thread2_interrupt_before_node = json_create_string("end");
    thread2_state_json = create_remote_state_json();
    check_not_null(thread2_start_params);
    check_not_null(thread2_start_options);
    check_not_null(thread2_interrupt_before_nodes);
    check_not_null(thread2_interrupt_before_node);
    check_not_null(thread2_state_json);

    json_object_set_string(thread2_start_params, "graph_name", graph_name);
    json_object_set_string(thread2_start_params, "thread_id", thread2_id);
    json_object_add(thread2_start_params, "state", thread2_state_json);
    json_array_add(thread2_interrupt_before_nodes, thread2_interrupt_before_node);
    json_object_add(thread2_start_options, "interrupt_before_nodes",
                          thread2_interrupt_before_nodes);
    json_object_add(thread2_start_params, "options", thread2_start_options);
    request_json = create_remote_jsonrpc_request("req-start-2", "runtime.start",
                                                 thread2_start_params);
    check_not_null(request_json);
    json_free(thread2_start_params); thread2_start_params = NULL;
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-start-2");
    result_json = json_object_get(response_json, "result");
    summary_json = json_object_get(result_json, "summary");
    state_json = json_object_get(result_json, "state");
    check_not_null(summary_json);
    check_not_null(state_json);
    check_equal(json_get_string(summary_json, "thread_id"), thread2_id);
    check_equal(json_get_string(summary_json, "status"), "interrupted");
    thread2_run_id = remote_strdup(json_get_string(summary_json, "run_id"));
    check_not_null(thread2_run_id);
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;

    {
      json_value_t *thread_command_params = json_create_object();
      json_value_t *thread_command_json = json_create_object();

      check_not_null(thread_command_params);
      check_not_null(thread_command_json);
      json_object_set_string(thread_command_params, "graph_name", graph_name);
      json_object_set_string(thread_command_params, "thread_id", thread2_id);
      json_object_set_string(thread_command_json, "kind", "append_user_message");
      json_object_set_string(thread_command_json, "text", "fork from thread command");
      json_object_add(thread_command_params, "command", thread_command_json);
      request_json = create_remote_jsonrpc_request("req-thread-fork",
                                                   "runtime.forkThreadCommandJsonValueGraph",
                                                   thread_command_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_command_fork_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_command_fork_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread2_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(strcmp(json_get_string(summary_json, "run_id"), thread2_run_id) != 0);
      check_true(json_get_bool(state_json, "visited_end", false));
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread_command_params); thread_command_params = NULL;
    }

    {
      json_value_t *thread_command_params = json_create_object();
      json_value_t *thread_command_json = json_create_object();

      check_not_null(thread_command_params);
      check_not_null(thread_command_json);
      json_object_set_string(thread_command_params, "graph_name", graph_name);
      json_object_set_string(thread_command_params, "thread_id", thread2_id);
      json_object_set_string(thread_command_json, "kind", "append_user_message");
      json_object_set_string(thread_command_json, "text", "resume from thread command");
      json_object_add(thread_command_params, "command", thread_command_json);
      request_json = create_remote_jsonrpc_request("req-thread-resume",
                                                   "runtime.resumeThreadCommandJsonValueGraph",
                                                   thread_command_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_command_resume_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_command_resume_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread2_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_equal(json_get_string(summary_json, "run_id"), thread2_run_id);
      check_true(json_get_bool(state_json, "visited_end", false));
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread_command_params); thread_command_params = NULL;
    }

    {
      const char *thread3_id = "remote-thread-3";
      json_value_t *thread3_start_params = json_create_object();
      json_value_t *thread3_start_options = json_create_object();
      json_value_t *thread3_interrupt_before_nodes = json_create_array();
      json_value_t *thread3_interrupt_before_node = json_create_string("end");
      json_value_t *thread3_state_json = create_remote_state_json();
      json_value_t *thread3_replay_params = json_create_object();
      json_value_t *thread3_replay_state = json_create_object();

      check_not_null(thread3_start_params);
      check_not_null(thread3_start_options);
      check_not_null(thread3_interrupt_before_nodes);
      check_not_null(thread3_interrupt_before_node);
      check_not_null(thread3_state_json);
      check_not_null(thread3_replay_params);
      check_not_null(thread3_replay_state);

      json_object_set_string(thread3_start_params, "graph_name", graph_name);
      json_object_set_string(thread3_start_params, "thread_id", thread3_id);
      json_object_add(thread3_start_params, "state", thread3_state_json);
      json_array_add(thread3_interrupt_before_nodes, thread3_interrupt_before_node);
      json_object_add(thread3_start_options, "interrupt_before_nodes",
                            thread3_interrupt_before_nodes);
      json_object_add(thread3_start_params, "options", thread3_start_options);
      request_json = create_remote_jsonrpc_request("req-start-3", "runtime.start",
                                                   thread3_start_params);
      check_not_null(request_json);
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success(response_json, "req-start-3");
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread3_start_params); thread3_start_params = NULL;

      json_object_set_string(thread3_replay_params, "graph_name", graph_name);
      json_object_set_string(thread3_replay_params, "thread_id", thread3_id);
      json_object_set_bool(thread3_replay_state, "patched", true);
      json_object_add(thread3_replay_params, "state", thread3_replay_state);
      request_json = create_remote_jsonrpc_request("req-thread-resume-state",
                                                   "runtime.resumeThreadJsonValueGraph",
                                                   thread3_replay_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_state_resume_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_state_resume_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread3_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(json_get_bool(state_json, "patched", false));
      check_true(json_get_bool(state_json, "visited_end", false));
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread3_replay_params); thread3_replay_params = NULL;
    }

    {
      const char *thread4_id = "remote-thread-4";
      json_value_t *thread4_start_params = json_create_object();
      json_value_t *thread4_start_options = json_create_object();
      json_value_t *thread4_interrupt_before_nodes = json_create_array();
      json_value_t *thread4_interrupt_before_node = json_create_string("end");
      json_value_t *thread4_state_json = create_remote_state_json();
      json_value_t *thread4_replay_params = json_create_object();
      json_value_t *thread4_replay_state = json_create_object();
      const char *forked_run_id;

      check_not_null(thread4_start_params);
      check_not_null(thread4_start_options);
      check_not_null(thread4_interrupt_before_nodes);
      check_not_null(thread4_interrupt_before_node);
      check_not_null(thread4_state_json);
      check_not_null(thread4_replay_params);
      check_not_null(thread4_replay_state);

      json_object_set_string(thread4_start_params, "graph_name", graph_name);
      json_object_set_string(thread4_start_params, "thread_id", thread4_id);
      json_object_add(thread4_start_params, "state", thread4_state_json);
      json_array_add(thread4_interrupt_before_nodes, thread4_interrupt_before_node);
      json_object_add(thread4_start_options, "interrupt_before_nodes",
                            thread4_interrupt_before_nodes);
      json_object_add(thread4_start_params, "options", thread4_start_options);
      request_json = create_remote_jsonrpc_request("req-start-4", "runtime.start",
                                                   thread4_start_params);
      check_not_null(request_json);
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success(response_json, "req-start-4");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      check_not_null(summary_json);
      forked_run_id = remote_strdup(json_get_string(summary_json, "run_id"));
      check_not_null(forked_run_id);
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread4_start_params); thread4_start_params = NULL;

      json_object_set_string(thread4_replay_params, "graph_name", graph_name);
      json_object_set_string(thread4_replay_params, "thread_id", thread4_id);
      json_object_set_bool(thread4_replay_state, "patched", true);
      json_object_add(thread4_replay_params, "state", thread4_replay_state);
      request_json = create_remote_jsonrpc_request("req-thread-fork-state",
                                                   "runtime.forkThreadJsonValueGraph",
                                                   thread4_replay_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_state_fork_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_state_fork_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread4_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(strcmp(json_get_string(summary_json, "run_id"), forked_run_id) != 0);
      check_true(json_get_bool(state_json, "patched", false));
      check_true(json_get_bool(state_json, "visited_end", false));
      free((void *)forked_run_id);
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread4_replay_params); thread4_replay_params = NULL;
    }

    {
      const char *thread5_id = "remote-thread-5";
      json_value_t *thread5_start_params = json_create_object();
      json_value_t *thread5_start_options = json_create_object();
      json_value_t *thread5_interrupt_before_nodes = json_create_array();
      json_value_t *thread5_interrupt_before_node = json_create_string("end");
      json_value_t *thread5_state_json = create_remote_state_json();
      json_value_t *thread5_patch_params = json_create_object();
      json_value_t *thread5_patch_json = json_create_object();

      check_not_null(thread5_start_params);
      check_not_null(thread5_start_options);
      check_not_null(thread5_interrupt_before_nodes);
      check_not_null(thread5_interrupt_before_node);
      check_not_null(thread5_state_json);
      check_not_null(thread5_patch_params);
      check_not_null(thread5_patch_json);

      json_object_set_string(thread5_start_params, "graph_name", graph_name);
      json_object_set_string(thread5_start_params, "thread_id", thread5_id);
      json_object_add(thread5_start_params, "state", thread5_state_json);
      json_array_add(thread5_interrupt_before_nodes, thread5_interrupt_before_node);
      json_object_add(thread5_start_options, "interrupt_before_nodes",
                            thread5_interrupt_before_nodes);
      json_object_add(thread5_start_params, "options", thread5_start_options);
      request_json = create_remote_jsonrpc_request("req-start-5", "runtime.start",
                                                   thread5_start_params);
      check_not_null(request_json);
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success(response_json, "req-start-5");
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread5_start_params); thread5_start_params = NULL;

      json_object_set_string(thread5_patch_params, "graph_name", graph_name);
      json_object_set_string(thread5_patch_params, "thread_id", thread5_id);
      json_object_set_bool(thread5_patch_json, "patched_via_state_patch", true);
      json_object_add(thread5_patch_params, "state_patch", thread5_patch_json);
      request_json = create_remote_jsonrpc_request("req-thread-resume-patch",
                                                   "runtime.resumeThreadStatePatchJsonValueGraph",
                                                   thread5_patch_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_state_patch_resume_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_state_patch_resume_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread5_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(json_get_bool(state_json, "patched_via_state_patch", false));
      check_true(json_get_bool(state_json, "visited_end", false));
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread5_patch_params); thread5_patch_params = NULL;
    }

    {
      const char *thread6_id = "remote-thread-6";
      json_value_t *thread6_start_params = json_create_object();
      json_value_t *thread6_start_options = json_create_object();
      json_value_t *thread6_interrupt_before_nodes = json_create_array();
      json_value_t *thread6_interrupt_before_node = json_create_string("end");
      json_value_t *thread6_state_json = create_remote_state_json();
      json_value_t *thread6_patch_params = json_create_object();
      json_value_t *thread6_patch_json = json_create_object();
      char *thread6_run_id = NULL;

      check_not_null(thread6_start_params);
      check_not_null(thread6_start_options);
      check_not_null(thread6_interrupt_before_nodes);
      check_not_null(thread6_interrupt_before_node);
      check_not_null(thread6_state_json);
      check_not_null(thread6_patch_params);
      check_not_null(thread6_patch_json);

      json_object_set_string(thread6_start_params, "graph_name", graph_name);
      json_object_set_string(thread6_start_params, "thread_id", thread6_id);
      json_object_add(thread6_start_params, "state", thread6_state_json);
      json_array_add(thread6_interrupt_before_nodes, thread6_interrupt_before_node);
      json_object_add(thread6_start_options, "interrupt_before_nodes",
                            thread6_interrupt_before_nodes);
      json_object_add(thread6_start_params, "options", thread6_start_options);
      request_json = create_remote_jsonrpc_request("req-start-6", "runtime.start",
                                                   thread6_start_params);
      check_not_null(request_json);
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success(response_json, "req-start-6");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      check_not_null(summary_json);
      thread6_run_id = remote_strdup(json_get_string(summary_json, "run_id"));
      check_not_null(thread6_run_id);
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread6_start_params); thread6_start_params = NULL;

      json_object_set_string(thread6_patch_params, "graph_name", graph_name);
      json_object_set_string(thread6_patch_params, "thread_id", thread6_id);
      json_object_set_bool(thread6_patch_json, "patched_via_state_patch", true);
      json_object_add(thread6_patch_params, "state_patch", thread6_patch_json);
      request_json = create_remote_jsonrpc_request("req-thread-fork-patch",
                                                   "runtime.forkThreadStatePatchJsonValueGraph",
                                                   thread6_patch_params);
      check_not_null(request_json);
      check_remote_jsonrpc_request_fixture(request_json, "runtime_remote_graph_runs.golden.json",
                                           "thread_state_patch_fork_request");
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_success_fixture(response_json,
                                           "runtime_remote_graph_runs.golden.json",
                                           "thread_state_patch_fork_response");
      result_json = json_object_get(response_json, "result");
      summary_json = json_object_get(result_json, "summary");
      state_json = json_object_get(result_json, "state");
      check_not_null(summary_json);
      check_not_null(state_json);
      check_equal(json_get_string(summary_json, "thread_id"), thread6_id);
      check_equal(json_get_string(summary_json, "status"), "completed");
      check_true(strcmp(json_get_string(summary_json, "run_id"), thread6_run_id) != 0);
      check_true(json_get_bool(state_json, "patched_via_state_patch", false));
      check_true(json_get_bool(state_json, "visited_end", false));
      free(thread6_run_id);
      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      json_free(thread6_patch_params); thread6_patch_params = NULL;
    }

    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
    turbo_graph_destroy(graph);
    free((void *)checkpoint_id);
    free((void *)run_id);
    free(thread2_run_id);
  }

  it("should dispatch json-rpc text requests and return serialized responses") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_graph_t *graph = create_remote_graph();
    remote_graph_registry_t registry = {"remote-skeleton", graph};
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *request_json =
        create_remote_jsonrpc_request_from_fixture("runtime_remote_thread_control.golden.json",
                                                   "start_request");
    char *request_json_text = NULL;
    char *response_json_text = NULL;
    json_value_t *response_json;

    check_not_null(runtime);
    check_not_null(graph);
    check_not_null(request_json);

    config.runtime = runtime;
    config.graph_resolver = remote_graph_resolver;
    config.graph_resolver_user_data = &registry;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    request_json_text = json_serialize(request_json, NULL);
    check_not_null(request_json_text);
    check_equal(
        turbo_agent_runtime_remote_dispatch_jsonrpc_text(remote, request_json_text,
                                                         &response_json_text),
        0);
    check_not_null(response_json_text);
    response_json = parse_remote_json_text(response_json_text);
    check_remote_jsonrpc_success_fixture(response_json,
                                         "runtime_remote_thread_control.golden.json",
                                         "start_response");

    json_free(response_json); response_json = NULL;
    json_serialize_free(response_json_text);
    json_serialize_free(request_json_text);
    json_free(request_json); request_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_graph_destroy(graph);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return invalid request envelopes for malformed json-rpc text") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    char *response_json_text = NULL;
    json_value_t *response_json;

    check_not_null(runtime);
    config.runtime = runtime;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc_text(remote, "{\"jsonrpc\":",
                                                                  &response_json_text),
                 0);
    check_not_null(response_json_text);
    response_json = parse_remote_json_text(response_json_text);
    check_true(json_is_null(json_object_get(response_json, "id")));
    check_not_null(json_object_get(response_json, "error"));
    check_equal(json_get_int(json_object_get(response_json, "error"), "code", 0),
                 -32600);

    json_free(response_json); response_json = NULL;
    json_serialize_free(response_json_text);
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
  }

        it("should return json-rpc invalid params for malformed start requests") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *params_json = json_create_object();
    json_value_t *request_json;
    json_value_t *response_json = NULL;

    check_not_null(runtime);
    check_not_null(params_json);

    config.runtime = runtime;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_string(params_json, "thread_id", "remote-thread-2");
    request_json = create_remote_jsonrpc_request("req-invalid", "runtime.start", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error_fixture(response_json, "runtime_remote_errors.golden.json",
                                       "invalid_params_response");

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return json-rpc invalid request for malformed envelopes") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *request_json = json_create_object();
    json_value_t *response_json = NULL;

    check_not_null(runtime);
    check_not_null(request_json);

    config.runtime = runtime;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_string(request_json, "jsonrpc", "1.0");
    json_object_set_string(request_json, "id", "req-invalid-request");
    json_object_set_string(request_json, "method", "runtime.start");

    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error_fixture(response_json, "runtime_remote_errors.golden.json",
                                       "invalid_request_response");

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return json-rpc method not found for unknown methods") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *request_json;
    json_value_t *response_json = NULL;

    check_not_null(runtime);

    config.runtime = runtime;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    request_json = create_remote_jsonrpc_request("req-unknown", "runtime.noSuchMethod", NULL);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error_fixture(response_json, "runtime_remote_errors.golden.json",
                                       "method_not_found_response");

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return json-rpc internal error when a graph-bound method cannot resolve the graph") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *params_json = json_create_object();
    json_value_t *request_json;
    json_value_t *response_json = NULL;

    check_not_null(runtime);
    check_not_null(params_json);

    config.runtime = runtime;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_string(params_json, "graph_name", "remote-skeleton");
    request_json = create_remote_jsonrpc_request("req-internal", "runtime.start", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error_fixture(response_json, "runtime_remote_errors.golden.json",
                                       "internal_error_response");

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should dispatch memory methods through the borrowed memory store") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_memory_store_t memory_store = turbo_agent_memory_store_memory_create();
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *record_fixture =
        turbo_agent_test_load_fixture_json("memory_context_record.golden.json");
    json_value_t *request_json;
    json_value_t *params_json;
    json_value_t *response_json = NULL;
    json_value_t *result_json;
    const json_value_t *record_json;
    const json_value_t *records_json;
    const json_value_t *valid_json;
    json_value_t *record_clone;
    json_value_t *record_variant_clone;
    json_value_t *invalid_record;
    const json_value_t *deleted_json;

    check_not_null(runtime);
    check_not_null(record_fixture);
    check_not_null(memory_store.get);
    check_not_null(memory_store.put);
    config.runtime = runtime;
    config.memory_store = &memory_store;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    params_json = json_create_object();
    check_not_null(params_json);
    record_clone = json_clone(record_fixture);
    check_not_null(record_clone);
    json_object_add(params_json, "record", record_clone);
    request_json = create_remote_jsonrpc_request("req-memory-put", "memory.putRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-put");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    record_json = json_object_get(result_json, "record");
    turbo_agent_test_check_memory_record_fixture(record_json, "memory_context_record.golden.json",
                                                 NULL);
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    record_clone = json_clone(record_fixture);
    check_not_null(record_clone);
    json_object_add(params_json, "record", record_clone);
    request_json =
        create_remote_jsonrpc_request("req-memory-validate", "memory.validateRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-validate");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    valid_json = json_object_get(result_json, "valid");
    check_not_null(valid_json);
    check_true(json_get_bool(result_json, "valid", false));
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    invalid_record = json_create_object();
    check_not_null(invalid_record);
    json_object_set_string(invalid_record, "id", "project/demo::broken");
    json_object_set_string(invalid_record, "namespace", "project/demo");
    json_object_set_string(invalid_record, "kind", "context");
    json_object_set_string(invalid_record, "key", "broken");
    json_object_set_string(invalid_record, "text", "missing value json");
    json_object_set_null(invalid_record, "metadata");
    json_object_set_null(invalid_record, "created_at");
    json_object_add(params_json, "record", invalid_record);
    request_json = create_remote_jsonrpc_request("req-memory-validate-invalid",
                                                 "memory.validateRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-validate-invalid");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    valid_json = json_object_get(result_json, "valid");
    check_not_null(valid_json);
    check_false(json_get_bool(result_json, "valid", true));
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    record_variant_clone =
        create_remote_memory_record_variant(record_fixture, "zeta", "remember this too");
    check_not_null(record_variant_clone);
    json_object_set_string(record_variant_clone, "created_at", "2026-02-15T12:00:00Z");
    json_object_add(params_json, "record", record_variant_clone);
    request_json = create_remote_jsonrpc_request("req-memory-put-variant", "memory.putRecord",
                                                 params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-put-variant");
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "memory_namespace", "project/demo");
    json_object_set_string(params_json, "key", "context");
    request_json = create_remote_jsonrpc_request("req-memory-get", "memory.getRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-get");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    record_json = json_object_get(result_json, "record");
    turbo_agent_test_check_memory_record_fixture(record_json, "memory_context_record.golden.json",
                                                 NULL);
    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace_prefix", "project/");
    json_object_set_string(params_json, "kind", "context");
    json_object_set_string(params_json, "text_substring", "remember");
    json_object_set_string(params_json, "id_prefix", "project/demo::z");
    json_object_set_string(params_json, "metadata_scope", "project");
    json_object_set_string(params_json, "metadata_path_prefix", "/tmp");
    json_object_set_string(params_json, "created_after", "2026-02-01T00:00:00Z");
    json_object_set_string(params_json, "created_before", "2026-02-28T23:59:59Z");
    json_object_set_string(params_json, "sort_by", "key");
    json_object_set_string(params_json, "sort_order", "desc");
    json_object_set_number(params_json, "limit", 1);
    request_json =
        create_remote_jsonrpc_request("req-memory-query", "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-query");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    records_json = json_object_get(result_json, "records");
    check_true(json_type(records_json) == JSON_ARRAY);
    check_equal(json_array_size(records_json), 1);
    check_equal(json_get_string(json_array_get(records_json, 0), "key"),
                 "zeta");

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace_prefix", "project/");
    request_json =
        create_remote_jsonrpc_request("req-memory-list", "memory.listRecords", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-list");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    records_json = json_object_get(result_json, "records");
    check_true(json_type(records_json) == JSON_ARRAY);
    check_equal(json_array_size(records_json), 2);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace", "project/demo");
    json_object_set_string(params_json, "key", "zeta");
    request_json =
        create_remote_jsonrpc_request("req-memory-delete", "memory.deleteRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_success(response_json, "req-memory-delete");
    result_json = json_object_get(response_json, "result");
    check_not_null(result_json);
    deleted_json = json_object_get(result_json, "deleted");
    check_not_null(deleted_json);
    check_true(json_get_bool(result_json, "deleted", false));

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "memory_namespace", "project/demo");
    json_object_set_string(params_json, "key", "zeta");
    request_json = create_remote_jsonrpc_request("req-memory-get-missing", "memory.getRecord",
                                                 params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-get-missing", -32603);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;
    json_free(record_fixture); record_fixture = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_memory_store_destroy(&memory_store);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return json-rpc invalid params for malformed memory payloads") {
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_memory_store_t memory_store = turbo_agent_memory_store_memory_create();
    turbo_agent_runtime_remote_config_t config = {0};
    turbo_agent_runtime_remote_t *remote;
    json_value_t *params_json = json_create_object();
    json_value_t *request_json;
    json_value_t *response_json = NULL;

    check_not_null(runtime);
    check_not_null(params_json);
    check_not_null(memory_store.get);
    check_not_null(memory_store.put);
    config.runtime = runtime;
    config.memory_store = &memory_store;
    remote = turbo_agent_runtime_remote_create(&config);
    check_not_null(remote);

    json_object_set_number(params_json, "memory_namespace", 1);
    request_json = create_remote_jsonrpc_request("req-memory-invalid", "memory.getRecord",
                                                 params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    request_json = create_remote_jsonrpc_request("req-memory-validate-missing-record",
                                                 "memory.validateRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-validate-missing-record", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "record", "not-object");
    request_json = create_remote_jsonrpc_request("req-memory-validate-record-not-object",
                                                 "memory.validateRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-validate-record-not-object", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "key", "context");
    request_json = create_remote_jsonrpc_request("req-memory-delete-missing-namespace",
                                                 "memory.deleteRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-delete-missing-namespace", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace", "project/demo");
    request_json = create_remote_jsonrpc_request("req-memory-delete-missing-key",
                                                 "memory.deleteRecord", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-delete-missing-key", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace_prefix", "project/");
    json_object_set_string(params_json, "sort_by", "bogus");
    request_json = create_remote_jsonrpc_request("req-memory-invalid-sort-by",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-sort-by", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_number(params_json, "id_prefix", 1);
    request_json = create_remote_jsonrpc_request("req-memory-invalid-id-prefix",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-id-prefix", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_bool(params_json, "metadata_scope", true);
    request_json = create_remote_jsonrpc_request("req-memory-invalid-metadata-scope",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-metadata-scope", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_number(params_json, "metadata_path_prefix", 1);
    request_json = create_remote_jsonrpc_request("req-memory-invalid-metadata-path-prefix",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-metadata-path-prefix", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_bool(params_json, "created_after", true);
    request_json = create_remote_jsonrpc_request("req-memory-invalid-created-after",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-created-after", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_number(params_json, "created_before", 1);
    request_json = create_remote_jsonrpc_request("req-memory-invalid-created-before",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-created-before", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;

    params_json = json_create_object();
    check_not_null(params_json);
    json_object_set_string(params_json, "namespace_prefix", "project/");
    json_object_set_string(params_json, "sort_order", "sideways");
    request_json = create_remote_jsonrpc_request("req-memory-invalid-sort-order",
                                                 "memory.queryRecordsEx", params_json);
    check_not_null(request_json);
    check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                 0);
    check_remote_jsonrpc_error(response_json, "req-memory-invalid-sort-order", -32602);

    json_free(response_json); response_json = NULL;
    json_free(request_json); request_json = NULL;
    json_free(params_json); params_json = NULL;
    turbo_agent_runtime_remote_destroy(remote);
    turbo_agent_memory_store_destroy(&memory_store);
    turbo_agent_runtime_destroy(runtime);
  }

  it("should return json-rpc invalid params for malformed method payloads") {
    static const char *const cases[] = {
        "start_missing_graph_name",
        "start_options_not_object",
        "resume_missing_checkpoint_id",
        "resume_wrong_graph_name_type",
        "get_thread_state_wrong_type",
        "get_checkpoint_context_wrong_type",
        "list_indexes_filters_not_object",
        "apply_command_missing_kind",
        "thread_command_missing_graph_name",
        "thread_command_wrong_command_type",
        "thread_state_wrong_graph_name_type",
        "thread_state_patch_missing_patch"};
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
      turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
      turbo_agent_runtime_remote_config_t config = {0};
      turbo_agent_runtime_remote_t *remote;
      json_value_t *request_json;
      json_value_t *response_json = NULL;

      check_not_null(runtime);
      config.runtime = runtime;
      remote = turbo_agent_runtime_remote_create(&config);
      check_not_null(remote);

      request_json = create_remote_jsonrpc_request_from_fixture(
          "runtime_remote_invalid_params.golden.json", cases[i]);
      check_not_null(request_json);
      check_equal(turbo_agent_runtime_remote_dispatch_jsonrpc(remote, request_json, &response_json),
                   0);
      check_remote_jsonrpc_error_fixture(response_json, "runtime_remote_invalid_params.golden.json",
                                         cases[i]);

      json_free(response_json); response_json = NULL;
      json_free(request_json); request_json = NULL;
      turbo_agent_runtime_remote_destroy(remote);
      turbo_agent_runtime_destroy(runtime);
    }
  }

  it("should keep remote json-rpc golden fixtures parseable") {
    json_value_t *thread_control_fixture =
        turbo_agent_test_load_fixture_json("runtime_remote_thread_control.golden.json");
    json_value_t *graph_runs_fixture =
        turbo_agent_test_load_fixture_json("runtime_remote_graph_runs.golden.json");
    json_value_t *inspect_fixture =
        turbo_agent_test_load_fixture_json("runtime_remote_inspect.golden.json");
    json_value_t *errors_fixture =
        turbo_agent_test_load_fixture_json("runtime_remote_errors.golden.json");
    json_value_t *invalid_params_fixture =
        turbo_agent_test_load_fixture_json("runtime_remote_invalid_params.golden.json");

    check_true(json_type(thread_control_fixture) == JSON_OBJECT);
    check_true(json_type(graph_runs_fixture) == JSON_OBJECT);
    check_true(json_type(inspect_fixture) == JSON_OBJECT);
    check_true(json_type(errors_fixture) == JSON_OBJECT);
    check_true(json_type(invalid_params_fixture) == JSON_OBJECT);

    check_not_null(turbo_agent_test_find_named_fixture_entry(thread_control_fixture, "start_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(thread_control_fixture, "get_thread_state_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(thread_control_fixture, "apply_command_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(thread_control_fixture, "apply_command_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(graph_runs_fixture, "resume_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(graph_runs_fixture, "resume_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(graph_runs_fixture, "thread_state_patch_resume_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(graph_runs_fixture, "thread_state_patch_fork_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "thread_observability_index_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "thread_observability_index_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "checkpoint_context_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "checkpoint_context_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture, "get_run_request"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture, "get_run_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "get_checkpoint_request"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(inspect_fixture, "get_checkpoint_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "list_checkpoints_request"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "list_checkpoints_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "load_history_events_request"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "load_history_events_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "get_checkpoint_trace_events_request"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "get_checkpoint_trace_events_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "get_run_trace_events_request"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(inspect_fixture,
                                                             "get_run_trace_events_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(errors_fixture, "invalid_request_response"));
    check_not_null(
        turbo_agent_test_find_named_fixture_entry(errors_fixture, "internal_error_response"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(invalid_params_fixture,
                                                             "apply_command_missing_kind"));
    check_not_null(turbo_agent_test_find_named_fixture_entry(invalid_params_fixture,
                                                             "list_indexes_filters_not_object"));

    json_free(thread_control_fixture); thread_control_fixture = NULL;
    json_free(graph_runs_fixture); graph_runs_fixture = NULL;
    json_free(inspect_fixture); inspect_fixture = NULL;
    json_free(errors_fixture); errors_fixture = NULL;
    json_free(invalid_params_fixture); invalid_params_fixture = NULL;
  }
}
