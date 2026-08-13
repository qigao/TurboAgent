#include "tinytest.h"
#include "turbo_parser.h"
#include "turbo_state_graph.h"
#include "turbo_state_graph_store.h"

#include <string.h>

typedef struct {
  const char *text;
} state_graph_text_payload_t;

static json_value_t *make_string_array_value(const char *value) {
  json_value_t *array = turbo_json_create_array();

  check_not_null(array);
  check_int_eq(turbo_runtime_json_array_append(
                   array, turbo_json_create_string(value)),
               TURBO_RUNTIME_JSON_OK);
  return array;
}

static json_value_t *make_profile_value(const char *name,
                                                           const char *kind) {
  json_value_t *profile = turbo_json_create_object();

  check_not_null(profile);
  if (name) {
    check_int_eq(turbo_runtime_json_object_set(
                     profile, "name", turbo_json_create_string(name)),
                 TURBO_RUNTIME_JSON_OK);
  }
  if (kind) {
    check_int_eq(turbo_runtime_json_object_set(
                     profile, "kind", turbo_json_create_string(kind)),
                 TURBO_RUNTIME_JSON_OK);
  }
  return profile;
}

static turbo_state_graph_t *create_branching_graph(void) {
  turbo_state_graph_t *graph = turbo_state_graph_create("state-graph");
  turbo_state_graph_channel_config_t config = {0};
  json_value_t *default_value = NULL;

  check_not_null(graph);

  default_value = turbo_json_create_int64(0);
  config.reducer = TURBO_STATE_GRAPH_REDUCER_ADD;
  config.value_kind = TURBO_JSON_NUMBER;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "count", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  default_value = turbo_json_create_array();
  config.reducer = TURBO_STATE_GRAPH_REDUCER_APPEND;
  config.value_kind = TURBO_JSON_STRING;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "messages", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  default_value = turbo_json_create_object();
  config.reducer = TURBO_STATE_GRAPH_REDUCER_MERGE_OBJECT;
  config.value_kind = TURBO_JSON_OBJECT;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "profile", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  default_value = turbo_json_create_string("left");
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_JSON_STRING;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "mode", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  default_value = turbo_json_create_bool(0);
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_JSON_BOOL;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "approved", &config),
               TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  return graph;
}

static int branch_seed_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "count", turbo_json_create_int64(1)),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("seed")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(ctx->update, "profile",
                                         make_profile_value("alpha", NULL)),
      TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int branch_route_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("route")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(
      turbo_runtime_json_object_set(ctx->update, "profile",
                                         make_profile_value(NULL, "router")),
      TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int branch_left_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "count", turbo_json_create_int64(10)),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("left")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int branch_right_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "count", turbo_json_create_int64(20)),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("right")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int mode_is_left(const json_value_t *state, void *user_data) {
  const char *expected = (const char *)user_data;
  const char *mode = turbo_runtime_json_value_as_string(
      turbo_json_object_get(state, "mode"));

  return mode && strcmp(mode, expected) == 0;
}

static int review_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  const state_graph_text_payload_t *payload = (const state_graph_text_payload_t *)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value(payload->text)),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int done_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "count", turbo_json_create_int64(5)),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("done")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int invalid_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "missing", turbo_json_create_bool(1)),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int command_start_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("start")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_state_graph_ctx_goto(ctx, "right"), TURBO_STATE_GRAPH_OK);
  return 0;
}

static int command_left_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("left")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int command_right_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("right")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int send_dispatch_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  json_value_t *first = turbo_json_create_object();
  json_value_t *second = turbo_json_create_object();

  (void)user_data;
  check_not_null(first);
  check_not_null(second);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("dispatch")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(first, "messages",
                                                  make_string_array_value("a")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_runtime_json_object_set(second, "messages",
                                                  make_string_array_value("b")),
               TURBO_RUNTIME_JSON_OK);
  check_int_eq(turbo_state_graph_ctx_send(ctx, "worker", first), TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_ctx_send(ctx, "worker", second), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(second);
  turbo_runtime_json_destroy(first);
  return 0;
}

static int send_worker_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "messages", make_string_array_value("worker")),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static int child_answer_node(turbo_state_graph_exec_ctx_t *ctx, void *user_data) {
  const char *prompt = turbo_runtime_json_value_as_string(
      turbo_json_object_get(ctx->state, "prompt"));

  (void)user_data;
  check_not_null(prompt);
  check_int_eq(turbo_runtime_json_object_set(
                   ctx->update, "answer",
                   turbo_json_create_string(prompt)),
               TURBO_RUNTIME_JSON_OK);
  return 0;
}

static turbo_state_graph_t *create_child_answer_graph(void) {
  turbo_state_graph_t *graph = turbo_state_graph_create("child-answer");
  turbo_state_graph_channel_config_t config = {0};
  json_value_t *default_value = NULL;

  check_not_null(graph);
  default_value = turbo_json_create_string("");
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_JSON_STRING;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "prompt", &config), TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_add_channel(graph, "answer", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  check_int_eq(turbo_state_graph_add_json_value_node(graph, "answer", child_answer_node, NULL),
               TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_set_entry(graph, "answer"), TURBO_STATE_GRAPH_OK);
  return graph;
}

static turbo_state_graph_t *create_parent_subgraph_graph(turbo_state_graph_t *child_graph) {
  turbo_state_graph_t *graph = turbo_state_graph_create("parent-subgraph");
  turbo_state_graph_channel_config_t config = {0};
  turbo_state_graph_subgraph_node_config_t subgraph_config = {0};
  json_value_t *default_value = NULL;
  const char *input_channels[] = {"prompt"};

  check_not_null(graph);
  default_value = turbo_json_create_string("");
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_JSON_STRING;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "prompt", &config), TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  default_value = turbo_json_create_object();
  config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
  config.value_kind = TURBO_JSON_OBJECT;
  config.default_value = default_value;
  check_int_eq(turbo_state_graph_add_channel(graph, "child_result", &config),
               TURBO_STATE_GRAPH_OK);
  turbo_runtime_json_destroy(default_value);

  subgraph_config.child_graph = child_graph;
  subgraph_config.child_thread_id = "child-thread";
  subgraph_config.output_channel = "child_result";
  subgraph_config.input_channels = input_channels;
  subgraph_config.input_channel_count = 1;
  check_int_eq(turbo_state_graph_add_subgraph_node(graph, "child", &subgraph_config),
               TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_set_entry(graph, "child"), TURBO_STATE_GRAPH_OK);
  return graph;
}

static turbo_state_graph_t *create_review_graph(
    const state_graph_text_payload_t *review_payload) {
  turbo_state_graph_t *graph = create_branching_graph();

  check_not_null(graph);
  check_int_eq(turbo_state_graph_add_json_value_node(graph, "start", review_node, (void *)review_payload),
               TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_add_json_value_node(graph, "done", done_node, NULL),
               TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_add_json_value_edge(graph, "start", "done", NULL, NULL),
               TURBO_STATE_GRAPH_OK);
  check_int_eq(turbo_state_graph_set_entry(graph, "start"), TURBO_STATE_GRAPH_OK);
  return graph;
}

static int json_array_contains_string(const json_value_t *array, const char *expected) {
  size_t i;

  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY || !expected) {
    return 0;
  }
  for (i = 0; i < turbo_json_array_size(array); ++i) {
    const json_value_t *entry = turbo_json_array_get(array, i);
    if (entry && turbo_json_type(entry) == TURBO_JSON_STRING &&
        strcmp(turbo_json_string(entry), expected) == 0) {
      return 1;
    }
  }
  return 0;
}

static int json_array_contains_object_string_field(const json_value_t *array, const char *field,
                                                   const char *expected) {
  size_t i;

  if (!array || turbo_json_type(array) != TURBO_JSON_ARRAY || !field || !expected) {
    return 0;
  }
  for (i = 0; i < turbo_json_array_size(array); ++i) {
    const json_value_t *entry = turbo_json_array_get(array, i);
    const char *value = entry && turbo_json_type(entry) == TURBO_JSON_OBJECT
                            ? turbo_json_get_string(entry, field)
                            : NULL;
    if (value && strcmp(value, expected) == 0) {
      return 1;
    }
  }
  return 0;
}

static void check_review_thread_snapshot_state(turbo_state_graph_t *graph, const char *thread_id,
                                               const char *expected_run_status,
                                               size_t expected_message_count,
                                               int expected_approved,
                                               int expect_pending_run) {
  json_value_t *latest_run = NULL;
  json_value_t *thread_state = NULL;
  json_value_t *pending_run = NULL;

  check_int_eq(turbo_state_graph_get_latest_run(graph, thread_id, &latest_run),
               TURBO_STATE_GRAPH_OK);
  check_str_eq(turbo_runtime_json_value_as_string(
                   turbo_json_object_get(latest_run, "status")),
               expected_run_status);
  turbo_runtime_json_destroy(latest_run);
  latest_run = NULL;

  check_int_eq(turbo_state_graph_get_thread_state(graph, thread_id, &thread_state),
               TURBO_STATE_GRAPH_OK);
  check_size_eq(turbo_runtime_json_value_size(
                    turbo_json_object_get(thread_state, "messages")),
                expected_message_count);
  check_int_eq((int)turbo_runtime_json_value_as_bool(
                   turbo_json_object_get(thread_state, "approved"), 0),
               expected_approved);
  turbo_runtime_json_destroy(thread_state);
  thread_state = NULL;

  if (expect_pending_run) {
    check_int_eq(turbo_state_graph_get_pending_run(graph, thread_id, &pending_run),
                 TURBO_STATE_GRAPH_OK);
    turbo_runtime_json_destroy(pending_run);
    pending_run = NULL;
  } else {
    check_int_eq(turbo_state_graph_get_pending_run(graph, thread_id, &pending_run),
                 TURBO_STATE_GRAPH_RUN_NOT_FOUND);
  }
}

spec("turbo state graph") {

  describe("reducers and history") {

    it("should merge channel updates and expose state history") {
      turbo_state_graph_t *graph = create_branching_graph();
      turbo_state_graph_run_result_t result = {0};
      json_value_t *input = turbo_json_create_object();
      json_value_t *state = NULL;
      json_value_t *history = NULL;
      json_value_t *run = NULL;
      json_value_t *thread = NULL;
      json_value_t *latest_run = NULL;
      json_value_t *runs = NULL;
      json_value_t *history_entry = NULL;
      json_value_t *checkpoint = NULL;
      json_value_t *checkpoints = NULL;
      json_value_t *context = NULL;
      const json_value_t *messages = NULL;
      const json_value_t *profile = NULL;
      const json_value_t *first_history = NULL;
      const char *first_history_id = NULL;
      const char *first_checkpoint_id = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       input, "mode", turbo_json_create_string("left")),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_state_graph_add_json_value_node(graph, "seed", branch_seed_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "route", branch_route_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "left", branch_left_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "right", branch_right_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_edge(graph, "seed", "route", NULL, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(
          turbo_state_graph_add_json_value_edge(graph, "route", "left", mode_is_left, "left"),
          TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_edge(graph, "route", "right", NULL, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "seed"), TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(graph, "thread-alpha", input, NULL, &result, &state),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(result.thread_id, "thread-alpha");
      check_not_null(result.run_id);
      check_not_null(result.history_entry_id);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(state, "count"), 0),
                   11);
      messages = turbo_json_object_get(state, "messages");
      check_size_eq(turbo_runtime_json_value_size(messages), 3);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 0)),
                   "seed");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 2)),
                   "left");
      profile = turbo_json_object_get(state, "profile");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(profile, "name")),
                   "alpha");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(profile, "kind")),
                   "router");

      check_int_eq(turbo_state_graph_list_state_history(graph, result.run_id, &history),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(history), 4);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(
                           turbo_json_array_get(history, 0), "source_kind")),
                   "start");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(
                           turbo_json_array_get(history, 2), "source_name")),
                   "route");
      first_history = turbo_json_array_get(history, 0);
      first_history_id = turbo_runtime_json_value_as_string(
          turbo_json_object_get(first_history, "history_entry_id"));
      first_checkpoint_id = turbo_runtime_json_value_as_string(
          turbo_json_object_get(first_history, "checkpoint_id"));
      check_not_null(first_history_id);
      check_not_null(first_checkpoint_id);

      check_int_eq(turbo_state_graph_get_run(graph, result.run_id, &run),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(run, "run_id")),
                   result.run_id);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(run, "status")),
                   "completed");

      check_int_eq(turbo_state_graph_get_thread(graph, "thread-alpha", &thread),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(thread, "thread_id")),
                   "thread-alpha");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(thread, "current_run_id")),
                   result.run_id);

      check_int_eq(turbo_state_graph_get_latest_run(graph, "thread-alpha", &latest_run),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(latest_run, "run_id")),
                   result.run_id);

      check_int_eq(turbo_state_graph_list_runs(graph, "thread-alpha", &runs),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(runs), 1);

      check_int_eq(turbo_state_graph_get_history_entry(graph, first_history_id, &history_entry),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(history_entry, "history_entry_id")),
                   first_history_id);

      check_int_eq(turbo_state_graph_get_checkpoint(graph, first_checkpoint_id, &checkpoint),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(checkpoint, "checkpoint_id")),
                   first_checkpoint_id);

      check_int_eq(turbo_state_graph_list_checkpoints(graph, result.run_id, &checkpoints),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(checkpoints), 4);

      check_int_eq(
          turbo_state_graph_get_checkpoint_context(graph, first_checkpoint_id, &context),
          TURBO_STATE_GRAPH_OK);
      check_not_null(turbo_json_object_get(context, "checkpoint"));
      check_not_null(turbo_json_object_get(context, "run"));
      check_not_null(turbo_json_object_get(context, "thread"));
      check_not_null(turbo_json_object_get(context, "branch_tree"));

      turbo_runtime_json_destroy(runs);
      turbo_runtime_json_destroy(latest_run);
      turbo_runtime_json_destroy(thread);
      turbo_runtime_json_destroy(context);
      turbo_runtime_json_destroy(checkpoints);
      turbo_runtime_json_destroy(checkpoint);
      turbo_runtime_json_destroy(history_entry);
      turbo_runtime_json_destroy(run);
      turbo_runtime_json_destroy(history);
      turbo_runtime_json_destroy(state);
      turbo_runtime_json_destroy(input);
      turbo_state_graph_destroy(graph);
    }

    it("should reject updates to undeclared channels") {
      turbo_state_graph_t *graph = turbo_state_graph_create("invalid");
      turbo_state_graph_channel_config_t config = {0};
      json_value_t *default_value = turbo_json_create_bool(0);
      turbo_state_graph_run_result_t result = {0};
      json_value_t *state = NULL;

      check_not_null(graph);
      config.reducer = TURBO_STATE_GRAPH_REDUCER_REPLACE;
      config.value_kind = TURBO_JSON_BOOL;
      config.default_value = default_value;
      check_int_eq(turbo_state_graph_add_channel(graph, "known", &config), TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(default_value);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "bad", invalid_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "bad"), TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(graph, "thread-invalid", NULL, NULL, &result, &state),
                   TURBO_STATE_GRAPH_CHANNEL_NOT_FOUND);
      check_null(state);

      turbo_state_graph_destroy(graph);
    }
  }

  describe("update and time travel") {

    it("should apply host updates and resume an interrupted run") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_run_result_t result = {0};
      turbo_state_graph_run_result_t update_result = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *patch = turbo_json_create_object();
      json_value_t *thread = NULL;
      json_value_t *latest_run = NULL;
      json_value_t *pending_run = NULL;
      json_value_t *runs = NULL;
      json_value_t *thread_state = NULL;
      const char *interrupt_before[] = {"done"};

      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_state_graph_start(graph, "thread-review", NULL, &options, &result, &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      check_str_eq(result.next_node, "done");
      check_int_eq(turbo_state_graph_get_thread(graph, "thread-review", &thread),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_get_latest_run(graph, "thread-review", &latest_run),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_get_pending_run(graph, "thread-review", &pending_run),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_list_runs(graph, "thread-review", &runs),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(runs), 1);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(latest_run, "status")),
                   "interrupted");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(pending_run, "run_id")),
                   result.run_id);
      turbo_runtime_json_destroy(runs);
      turbo_runtime_json_destroy(pending_run);
      turbo_runtime_json_destroy(latest_run);
      turbo_runtime_json_destroy(thread);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       patch, "approved", turbo_json_create_bool(1)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       patch, "messages", make_string_array_value("manual")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_update_state(graph, result.run_id, patch, NULL,
                                                  &update_result, &state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_resume(graph, result.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(state, "count"), 0),
                   5);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      check_int_eq(turbo_state_graph_get_thread_state(graph, "thread-review", &thread_state),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(thread_state, "messages")),
                    3);

      turbo_runtime_json_destroy(thread_state);
      turbo_runtime_json_destroy(state);
      turbo_runtime_json_destroy(patch);
      turbo_state_graph_destroy(graph);
    }

    it("should round-trip snapshot state and resume the imported pending run") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_t *restored = create_review_graph(&review_payload);
      turbo_state_graph_run_result_t result = {0};
      turbo_state_graph_run_result_t update_result = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *patch = turbo_json_create_object();
      json_value_t *thread_state = NULL;
      json_value_t *restored_state = NULL;
      json_value_t *pending_run = NULL;
      json_value_t *history = NULL;
      char *snapshot = NULL;
      size_t snapshot_len = 0;
      const char *interrupt_before[] = {"done"};

      check_size_eq(turbo_state_graph_snapshot_schema_version(), 1);
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_state_graph_start(graph, "thread-snapshot", NULL, &options, &result, &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       patch, "approved", turbo_json_create_bool(1)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       patch, "messages", make_string_array_value("manual")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_update_state(graph, result.run_id, patch, NULL,
                                                  &update_result, &state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_get_thread_state(graph, "thread-snapshot", &thread_state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(thread_state, "approved"), 0));
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(thread_state, "messages")),
                    2);

      snapshot = turbo_state_graph_serialize_snapshot(graph, &snapshot_len);
      check_not_null(snapshot);
      check_true(snapshot_len > 0);
      check_int_eq(turbo_state_graph_load_snapshot(restored, snapshot, snapshot_len),
                   TURBO_STATE_GRAPH_OK);
      turbo_json_serialize_free(snapshot);
      snapshot = NULL;

      check_int_eq(turbo_state_graph_get_pending_run(restored, "thread-snapshot", &pending_run),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(pending_run, "run_id")),
                   result.run_id);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(pending_run, "status")),
                   "interrupted");

      check_int_eq(turbo_state_graph_get_thread_state(restored, "thread-snapshot", &restored_state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(restored_state, "approved"), 0));
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(restored_state, "messages")),
                    2);

      check_int_eq(turbo_state_graph_list_state_history(restored, result.run_id, &history),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(history), 3);
      turbo_runtime_json_destroy(history);
      history = NULL;

      check_int_eq(turbo_state_graph_resume(restored, result.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(state, "count"), 0),
                   5);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(state, "messages")),
                    3);

      check_int_eq(turbo_state_graph_list_state_history(restored, result.run_id, &history),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(history), 4);

      turbo_runtime_json_destroy(history);
      turbo_runtime_json_destroy(state);
      turbo_runtime_json_destroy(restored_state);
      turbo_runtime_json_destroy(pending_run);
      turbo_runtime_json_destroy(thread_state);
      turbo_runtime_json_destroy(patch);
      turbo_state_graph_destroy(restored);
      turbo_state_graph_destroy(graph);
    }

    it("should fork and resume from a historical state") {
      turbo_state_graph_t *graph = create_branching_graph();
      turbo_state_graph_run_result_t result = {0};
      turbo_state_graph_run_result_t forked = {0};
      turbo_state_graph_run_result_t resumed = {0};
      json_value_t *input = turbo_json_create_object();
      json_value_t *fork_patch = turbo_json_create_object();
      json_value_t *resume_patch = turbo_json_create_object();
      json_value_t *state = NULL;
      json_value_t *thread_state = NULL;
      json_value_t *history = NULL;
      const json_value_t *seed_entry = NULL;
      const char *seed_history_id = NULL;
      json_value_t *tree = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       input, "mode", turbo_json_create_string("left")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       fork_patch, "mode", turbo_json_create_string("right")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       resume_patch, "mode", turbo_json_create_string("right")),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_state_graph_add_json_value_node(graph, "seed", branch_seed_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "route", branch_route_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "left", branch_left_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "right", branch_right_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_edge(graph, "seed", "route", NULL, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(
          turbo_state_graph_add_json_value_edge(graph, "route", "left", mode_is_left, "left"),
          TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_edge(graph, "route", "right", NULL, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "seed"), TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(graph, "thread-branch", input, NULL, &result, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_list_state_history(graph, result.run_id, &history),
                   TURBO_STATE_GRAPH_OK);
      seed_entry = turbo_json_array_get(history, 1);
      check_not_null(seed_entry);
      seed_history_id = turbo_runtime_json_value_as_string(
          turbo_json_object_get(seed_entry, "history_entry_id"));
      check_not_null(seed_history_id);

      check_int_eq(turbo_state_graph_fork_from_history(graph, seed_history_id, fork_patch, NULL,
                                                       NULL, &forked, &state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(state, "count"), 0),
                   21);
      check_int_eq(turbo_state_graph_get_thread_state(graph, "thread-branch", &thread_state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(thread_state, "count"), 0),
                   11);
      turbo_runtime_json_destroy(thread_state);
      thread_state = NULL;
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_resume_from_history(graph, seed_history_id, resume_patch, NULL,
                                                         NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(state, "count"), 0),
                   21);
      check_int_eq(turbo_state_graph_get_thread_state(graph, "thread-branch", &thread_state),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq((int)turbo_runtime_json_value_as_int64(
                       turbo_json_object_get(thread_state, "count"), 0),
                   21);
      check_int_eq(turbo_state_graph_get_branch_tree(graph, "thread-branch", &tree),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(
                        turbo_json_object_get(tree, "branches")),
                    3);

      turbo_runtime_json_destroy(tree);
      turbo_runtime_json_destroy(thread_state);
      turbo_runtime_json_destroy(state);
      turbo_runtime_json_destroy(history);
      turbo_runtime_json_destroy(resume_patch);
      turbo_runtime_json_destroy(fork_patch);
      turbo_runtime_json_destroy(input);
      turbo_state_graph_destroy(graph);
    }
  }

  describe("subgraphs") {

    it("should run a child state graph node and merge its result into the parent state") {
      turbo_state_graph_t *child = create_child_answer_graph();
      turbo_state_graph_t *parent = create_parent_subgraph_graph(child);
      turbo_state_graph_run_result_t result = {0};
      json_value_t *input = turbo_json_create_object();
      json_value_t *state = NULL;
      json_value_t *child_run = NULL;
      const json_value_t *child_result = NULL;
      const json_value_t *child_state = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       input, "prompt", turbo_json_create_string("write-plan")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_start(parent, "parent-thread", input, NULL, &result, &state),
                   TURBO_STATE_GRAPH_OK);
      child_result = turbo_json_object_get(state, "child_result");
      check_not_null(child_result);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(child_result, "status")),
                   "completed");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(child_result, "thread_id")),
                   "child-thread");
      child_state = turbo_json_object_get(child_result, "state");
      check_not_null(child_state);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(child_state, "prompt")),
                   "write-plan");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(child_state, "answer")),
                   "write-plan");

      check_int_eq(turbo_state_graph_get_latest_run(child, "child-thread", &child_run),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(child_run, "status")),
                   "completed");

      turbo_runtime_json_destroy(child_run);
      turbo_runtime_json_destroy(state);
      turbo_runtime_json_destroy(input);
      turbo_state_graph_destroy(parent);
      turbo_state_graph_destroy(child);
    }
  }

  describe("command and send") {

    it("should let command goto override static edges") {
      turbo_state_graph_t *graph = create_branching_graph();
      turbo_state_graph_run_result_t result = {0};
      json_value_t *state = NULL;
      const json_value_t *messages = NULL;

      check_int_eq(turbo_state_graph_add_json_value_node(graph, "start", command_start_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "left", command_left_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "right", command_right_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_edge(graph, "start", "left", NULL, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "start"), TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(graph, "thread-command", NULL, NULL, &result, &state),
                   TURBO_STATE_GRAPH_OK);
      messages = turbo_json_object_get(state, "messages");
      check_size_eq(turbo_runtime_json_value_size(messages), 2);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 0)),
                   "start");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 1)),
                   "right");

      turbo_runtime_json_destroy(state);
      turbo_state_graph_destroy(graph);
    }

    it("should fan out dynamic sends and reduce target updates back into state") {
      turbo_state_graph_t *graph = create_branching_graph();
      turbo_state_graph_run_result_t result = {0};
      json_value_t *state = NULL;
      json_value_t *history = NULL;
      const json_value_t *messages = NULL;

      check_int_eq(turbo_state_graph_add_json_value_node(graph, "dispatch", send_dispatch_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_add_json_value_node(graph, "worker", send_worker_node, NULL),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_set_entry(graph, "dispatch"), TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(graph, "thread-send", NULL, NULL, &result, &state),
                   TURBO_STATE_GRAPH_OK);
      messages = turbo_json_object_get(state, "messages");
      check_size_eq(turbo_runtime_json_value_size(messages), 5);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 0)),
                   "dispatch");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 1)),
                   "a");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 2)),
                   "worker");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 3)),
                   "b");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_array_get(messages, 4)),
                   "worker");

      check_int_eq(turbo_state_graph_list_state_history(graph, result.run_id, &history),
                   TURBO_STATE_GRAPH_OK);
      check_size_eq(turbo_runtime_json_value_size(history), 6);

      turbo_runtime_json_destroy(history);
      turbo_runtime_json_destroy(state);
      turbo_state_graph_destroy(graph);
    }
  }

  describe("snapshot stores") {

    it("should persist and restore one snapshot through the memory store") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_t *restored = create_review_graph(&review_payload);
      turbo_state_graph_store_t store = turbo_state_graph_store_memory_create();
      turbo_state_graph_run_result_t result = {0};
      turbo_state_graph_run_result_t update_result = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *patch = turbo_json_create_object();
      json_value_t *pending_run = NULL;
      json_value_t *snapshot_ids = NULL;
      json_value_t *descriptor = NULL;
      json_value_t *descriptors = NULL;
      const char *interrupt_before[] = {"done"};

      check_not_null(store.user_data);
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_state_graph_start(graph, "thread-memory-store", NULL, &options, &result,
                                           &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       patch, "approved", turbo_json_create_bool(1)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_update_state(graph, result.run_id, patch, NULL,
                                                  &update_result, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "memory-review", graph),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_store_list(&store, &snapshot_ids), 0);
      check_size_eq(turbo_json_array_size(snapshot_ids), 1);
      check_true(json_array_contains_string(snapshot_ids, "memory-review"));
      turbo_free_json(&snapshot_ids);
      check_int_eq(
          turbo_state_graph_store_get_snapshot_descriptor(&store, "memory-review", &descriptor), 0);
      check_str_eq(turbo_json_get_string(descriptor, "snapshot_id"), "memory-review");
      check_str_eq(turbo_json_get_string(descriptor, "graph_name"), "state-graph");
      check_str_eq(turbo_json_get_string(descriptor, "entry_node"), "start");
      check_int_eq((int)turbo_json_get_double(descriptor, "thread_count", 0), 1);
      check_int_eq((int)turbo_json_get_double(descriptor, "run_count", 0), 1);
      check_int_eq((int)turbo_json_get_double(descriptor, "history_count", 0), 3);
      check_int_eq((int)turbo_json_get_double(descriptor, "interrupted_run_count", 0), 1);
      turbo_free_json(&descriptor);
      descriptor = NULL;
      check_int_eq(turbo_state_graph_store_list_snapshot_descriptors(&store, &descriptors), 0);
      check_size_eq(turbo_json_array_size(descriptors), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(descriptors, 0), "snapshot_id"),
                   "memory-review");
      turbo_free_json(&descriptors);
      descriptors = NULL;

      check_int_eq(
          turbo_state_graph_store_load_snapshot(&store, "memory-review", restored),
          TURBO_STATE_GRAPH_OK);
      check_int_eq(
          turbo_state_graph_get_pending_run(restored, "thread-memory-store", &pending_run),
          TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(pending_run, "run_id")),
                   result.run_id);
      turbo_runtime_json_destroy(pending_run);
      pending_run = NULL;

      check_int_eq(turbo_state_graph_resume(restored, result.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_store_delete(&store, "memory-review"), 0);
      check_int_eq(turbo_state_graph_store_list(&store, &snapshot_ids), 0);
      check_size_eq(turbo_json_array_size(snapshot_ids), 0);
      check_int_eq(turbo_state_graph_store_list_snapshot_descriptors(&store, &descriptors), 0);
      check_size_eq(turbo_json_array_size(descriptors), 0);

      turbo_free_json(&descriptors);
      turbo_free_json(&snapshot_ids);
      turbo_runtime_json_destroy(patch);
      turbo_state_graph_store_destroy(&store);
      turbo_state_graph_destroy(restored);
      turbo_state_graph_destroy(graph);
    }

    it("should filter snapshot descriptors by graph runtime identifiers") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *pending_graph = create_review_graph(&review_payload);
      turbo_state_graph_t *completed_graph = create_review_graph(&review_payload);
      turbo_state_graph_store_t store = turbo_state_graph_store_memory_create();
      turbo_state_graph_run_result_t pending_result = {0};
      turbo_state_graph_run_result_t completed_result = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *descriptors = NULL;
      turbo_state_graph_store_list_options_t filter = {0};
      const char *interrupt_before[] = {"done"};

      check_not_null(store.user_data);
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_state_graph_start(pending_graph, "thread-filter-pending", NULL, &options,
                                           &pending_result, &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-pending",
                                                         pending_graph),
                   TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_start(completed_graph, "thread-filter-complete", NULL, NULL,
                                           &completed_result, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-complete",
                                                         completed_graph),
                   TURBO_STATE_GRAPH_OK);

      filter.thread_id = "thread-filter-pending";
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(descriptors, 0), "snapshot_id"),
                   "snapshot-pending");
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.thread_id = NULL;
      filter.run_id = pending_result.run_id;
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 2);
      check_true(json_array_contains_object_string_field(descriptors, "snapshot_id",
                                                         "snapshot-pending"));
      check_true(json_array_contains_object_string_field(descriptors, "snapshot_id",
                                                         "snapshot-complete"));
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.run_id = NULL;
      filter.history_entry_id = pending_result.history_entry_id;
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 2);
      check_true(json_array_contains_object_string_field(descriptors, "snapshot_id",
                                                         "snapshot-pending"));
      check_true(json_array_contains_object_string_field(descriptors, "snapshot_id",
                                                         "snapshot-complete"));
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.history_entry_id = NULL;
      filter.checkpoint_id = completed_result.checkpoint_id;
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(descriptors, 0), "snapshot_id"),
                   "snapshot-complete");
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.checkpoint_id = NULL;
      filter.run_status = "interrupted";
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(descriptors, 0), "snapshot_id"),
                   "snapshot-pending");
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.run_status = "completed";
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 1);
      check_str_eq(turbo_json_get_string(turbo_json_array_get(descriptors, 0), "snapshot_id"),
                   "snapshot-complete");
      turbo_free_json(&descriptors);
      descriptors = NULL;

      filter.run_status = NULL;
      filter.graph_name = "state-graph";
      filter.limit = 1;
      check_int_eq(
          turbo_state_graph_store_list_snapshot_descriptors_filtered(&store, &filter, &descriptors),
          0);
      check_size_eq(turbo_json_array_size(descriptors), 1);

      turbo_free_json(&descriptors);
      turbo_state_graph_store_destroy(&store);
      turbo_state_graph_destroy(completed_graph);
      turbo_state_graph_destroy(pending_graph);
    }

    it("should return the latest snapshot descriptor for one thread or run") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_store_t store = turbo_state_graph_store_memory_create();
      turbo_state_graph_run_result_t interrupted = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *latest = NULL;
      char *snapshot_id = NULL;
      turbo_state_graph_store_list_options_t filter = {0};
      const char *interrupt_before[] = {"done"};

      check_not_null(store.user_data);
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_state_graph_start(graph, "thread-latest", NULL, &options, &interrupted,
                                           &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-latest-1", graph),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_resume(graph, interrupted.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-latest-2", graph),
                   TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor_for_thread(
                       &store, "thread-latest", &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-2");
      check_int_eq((int)turbo_json_get_double(latest, "next_history_id", 0), 3);
      turbo_free_json(&latest);
      latest = NULL;

      check_int_eq(
          turbo_state_graph_store_get_latest_snapshot_id_for_thread(&store, "thread-latest",
                                                                    &snapshot_id),
          0);
      check_str_eq(snapshot_id, "snapshot-latest-2");
      free(snapshot_id);
      snapshot_id = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor_for_run(
                       &store, interrupted.run_id, &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-2");
      check_int_eq((int)turbo_json_get_double(latest, "next_history_id", 0), 3);
      turbo_free_json(&latest);
      latest = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_id_for_run(
                       &store, interrupted.run_id, &snapshot_id),
                   0);
      check_str_eq(snapshot_id, "snapshot-latest-2");
      free(snapshot_id);
      snapshot_id = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor_for_history_entry(
                       &store, interrupted.history_entry_id, &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-2");
      check_int_eq((int)turbo_json_get_double(latest, "next_history_id", 0), 3);
      turbo_free_json(&latest);
      latest = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_id_for_history_entry(
                       &store, interrupted.history_entry_id, &snapshot_id),
                   0);
      check_str_eq(snapshot_id, "snapshot-latest-2");
      free(snapshot_id);
      snapshot_id = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor_for_checkpoint(
                       &store, interrupted.checkpoint_id, &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-2");
      check_int_eq((int)turbo_json_get_double(latest, "next_history_id", 0), 3);
      turbo_free_json(&latest);
      latest = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_id_for_checkpoint(
                       &store, interrupted.checkpoint_id, &snapshot_id),
                   0);
      check_str_eq(snapshot_id, "snapshot-latest-2");
      free(snapshot_id);
      snapshot_id = NULL;

      filter.run_status = "interrupted";
      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor(&store, &filter, &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-1");
      turbo_free_json(&latest);
      latest = NULL;

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_id(&store, &filter, &snapshot_id),
                   0);
      check_str_eq(snapshot_id, "snapshot-latest-1");
      free(snapshot_id);
      snapshot_id = NULL;

      filter.run_status = "completed";
      check_int_eq(turbo_state_graph_store_get_latest_snapshot_descriptor(&store, &filter, &latest),
                   0);
      check_str_eq(turbo_json_get_string(latest, "snapshot_id"), "snapshot-latest-2");

      check_int_eq(turbo_state_graph_store_get_latest_snapshot_id(&store, &filter, &snapshot_id),
                   0);
      check_str_eq(snapshot_id, "snapshot-latest-2");

      free(snapshot_id);
      turbo_free_json(&latest);
      turbo_state_graph_store_destroy(&store);
      turbo_state_graph_destroy(graph);
    }

    it("should load the latest snapshot for one graph runtime anchor") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_t *restored = create_review_graph(&review_payload);
      turbo_state_graph_run_result_t interrupted = {0};
      turbo_state_graph_run_result_t updated = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      turbo_state_graph_store_t store = turbo_state_graph_store_memory_create();
      json_value_t *state = NULL;
      json_value_t *patch = turbo_json_create_object();
      turbo_state_graph_store_list_options_t filter = {0};
      const char *interrupt_before[] = {"done"};

      check_not_null(store.user_data);
      check_not_null(patch);
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_state_graph_start(graph, "thread-load-latest", NULL, &options,
                                           &interrupted, &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-load-1", graph),
                   TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_runtime_json_object_set(
                       patch, "approved", turbo_json_create_bool(1)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_runtime_json_object_set(
                       patch, "messages", make_string_array_value("manual")),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_update_state(graph, interrupted.run_id, patch, NULL,
                                                  &updated, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-load-2", graph),
                   TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_resume(graph, interrupted.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;
      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "snapshot-load-3", graph),
                   TURBO_STATE_GRAPH_OK);

      check_int_eq(turbo_state_graph_store_load_latest_snapshot(&store, NULL, restored),
                   TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "completed", 3, 1, 0);

      check_int_eq(turbo_state_graph_store_load_latest_snapshot_for_thread(
                       &store, "thread-load-latest", restored),
                   TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "completed", 3, 1, 0);

      check_int_eq(
          turbo_state_graph_store_load_latest_snapshot_for_run(&store, interrupted.run_id, restored),
          TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "completed", 3, 1, 0);

      check_int_eq(turbo_state_graph_store_load_latest_snapshot_for_history_entry(
                       &store, interrupted.history_entry_id, restored),
                   TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "completed", 3, 1, 0);

      check_int_eq(turbo_state_graph_store_load_latest_snapshot_for_checkpoint(
                       &store, interrupted.checkpoint_id, restored),
                   TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "completed", 3, 1, 0);

      filter.run_status = "interrupted";
      check_int_eq(turbo_state_graph_store_load_latest_snapshot(&store, &filter, restored),
                   TURBO_STATE_GRAPH_OK);
      check_review_thread_snapshot_state(restored, "thread-load-latest", "interrupted", 2, 1, 1);

      turbo_runtime_json_destroy(patch);
      turbo_state_graph_store_destroy(&store);
      turbo_state_graph_destroy(restored);
      turbo_state_graph_destroy(graph);
    }

    it("should persist and restore one snapshot through the file store") {
      state_graph_text_payload_t review_payload = {"review"};
      turbo_state_graph_t *graph = create_review_graph(&review_payload);
      turbo_state_graph_t *restored = create_review_graph(&review_payload);
      turbo_state_graph_store_t store =
          turbo_state_graph_store_file_create("state-graph-store-file");
      turbo_state_graph_run_result_t result = {0};
      turbo_state_graph_run_result_t update_result = {0};
      turbo_state_graph_run_result_t resumed = {0};
      turbo_state_graph_run_options_t options = {0};
      json_value_t *state = NULL;
      json_value_t *patch = turbo_json_create_object();
      json_value_t *pending_run = NULL;
      json_value_t *snapshot_ids = NULL;
      json_value_t *descriptor = NULL;
      const char *interrupt_before[] = {"done"};

      check_not_null(store.user_data);
      (void)turbo_state_graph_store_delete(&store, "file-review");
      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_state_graph_start(graph, "thread-file-store", NULL, &options, &result,
                                           &state),
                   TURBO_STATE_GRAPH_INTERRUPTED);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_runtime_json_object_set(
                       patch, "approved", turbo_json_create_bool(1)),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_state_graph_update_state(graph, result.run_id, patch, NULL,
                                                  &update_result, &state),
                   TURBO_STATE_GRAPH_OK);
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_store_save_snapshot(&store, "file-review", graph),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_store_list(&store, &snapshot_ids), 0);
      check_size_eq(turbo_json_array_size(snapshot_ids), 1);
      check_true(json_array_contains_string(snapshot_ids, "file-review"));
      turbo_free_json(&snapshot_ids);
      check_int_eq(
          turbo_state_graph_store_get_snapshot_descriptor(&store, "file-review", &descriptor), 0);
      check_str_eq(turbo_json_get_string(descriptor, "snapshot_id"), "file-review");
      check_int_eq((int)turbo_json_get_double(descriptor, "interrupted_run_count", 0), 1);
      turbo_free_json(&descriptor);
      descriptor = NULL;

      check_int_eq(turbo_state_graph_store_load_snapshot(&store, "file-review", restored),
                   TURBO_STATE_GRAPH_OK);
      check_int_eq(turbo_state_graph_get_pending_run(restored, "thread-file-store", &pending_run),
                   TURBO_STATE_GRAPH_OK);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(pending_run, "run_id")),
                   result.run_id);
      turbo_runtime_json_destroy(pending_run);
      pending_run = NULL;

      check_int_eq(turbo_state_graph_resume(restored, result.run_id, NULL, &resumed, &state),
                   TURBO_STATE_GRAPH_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(state, "approved"), 0));
      turbo_runtime_json_destroy(state);
      state = NULL;

      check_int_eq(turbo_state_graph_store_delete(&store, "file-review"), 0);
      check_int_eq(turbo_state_graph_store_list(&store, &snapshot_ids), 0);
      check_size_eq(turbo_json_array_size(snapshot_ids), 0);

      turbo_free_json(&snapshot_ids);
      turbo_runtime_json_destroy(patch);
      turbo_state_graph_store_destroy(&store);
      turbo_state_graph_destroy(restored);
      turbo_state_graph_destroy(graph);
    }
  }
}
