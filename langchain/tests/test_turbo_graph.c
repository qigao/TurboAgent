#include "tinytest.h"
#include "turbo_event_log.h"
#include "turbo_graph.h"
#include "turbo_runtime_control.h"

#include <stdio.h>
#include <string.h>
#include <salts/clock.h>

typedef struct {
  const char *value;
} string_payload_t;

typedef struct {
  char *serialized;
  size_t len;
  int count;
} checkpoint_capture_t;

typedef struct {
  int count;
  int node_events;
  int route_events;
  char last_name[64];
  char last_detail[64];
  char last_payload[64];
} graph_event_capture_t;

typedef struct {
  turbo_cancel_source_t *source;
  const char *value;
} cancel_node_payload_t;

static const string_payload_t s_router_payload = {"router"};
static const string_payload_t s_tool_payload = {"tool"};
static const string_payload_t s_done_payload = {"done"};

static int write_phase_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  string_payload_t *payload = (string_payload_t *)user_data;
  char key[64];

  snprintf(key, sizeof(key), "visited_%s", payload->value);
  turbo_json_object_set_bool(ctx->state, key, true);
  return 0;
}

static int write_phase_node_alt(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  string_payload_t *payload = (string_payload_t *)user_data;
  char key[64];

  snprintf(key, sizeof(key), "visited_%s", payload->value);
  turbo_json_object_set_bool(ctx->state, key, true);
  return 0;
}

static int write_phase_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  string_payload_t *payload = (string_payload_t *)user_data;
  char key[64];
  json_value_t *value;

  snprintf(key, sizeof(key), "visited_%s", payload->value);
  check_not_null(ctx);
  check_not_null(ctx->json_value_state);
  value = turbo_json_create_bool(1);
  check_not_null(value);
  return turbo_runtime_json_object_set(ctx->json_value_state, key, value) ==
                 TURBO_RUNTIME_JSON_OK
             ? 0
             : -1;
}

static int write_error_json_value_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  json_value_t *value;

  (void)user_data;
  check_not_null(ctx);
  check_not_null(ctx->json_value_state);
  value = turbo_json_create_bool(1);
  check_not_null(value);
  if (turbo_runtime_json_object_set(ctx->json_value_state, "error_recorded", value) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(value);
  }
  return -1;
}

static int cancel_after_write_json_value_node(turbo_graph_exec_ctx_t *ctx,
                                              void *user_data) {
  cancel_node_payload_t *payload = (cancel_node_payload_t *)user_data;
  char key[64];
  json_value_t *value;

  snprintf(key, sizeof(key), "visited_%s", payload->value);
  value = turbo_json_create_bool(1);
  if (!value) {
    return -1;
  }
  if (turbo_runtime_json_object_set(ctx->json_value_state, key, value) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(value);
    return -1;
  }
  return turbo_cancel_source_cancel(payload->source, TURBO_CANCEL_USER) ==
                 SALTS_OK
             ? 0
             : -1;
}

static int router_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  turbo_json_object_set_bool(ctx->state, "visited_router", true);
  return 0;
}

static int override_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  turbo_json_object_set_bool(ctx->state, "visited_override", true);
  return turbo_graph_ctx_set_next(ctx, "chosen");
}

static int stop_and_resume_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *next_node = (const char *)user_data;

  turbo_json_object_set_bool(ctx->state, "needs_review", true);
  check_int_eq(turbo_graph_ctx_set_next(ctx, next_node), TURBO_GRAPH_EXEC_OK);
  turbo_graph_ctx_stop(ctx);
  return 0;
}

static int predicate_mode_equals(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *expected = (const char *)user_data;
  const char *mode = turbo_json_get_string(ctx->state, "mode");

  if (!mode) {
    return 0;
  }

  return strcmp(mode, expected) == 0;
}

static int predicate_mode_equals_json_value(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *expected = (const char *)user_data;
  const char *mode;

  check_not_null(ctx);
  check_not_null(ctx->json_value_state);
  mode = turbo_runtime_json_value_as_string(
      turbo_json_object_get(ctx->json_value_state, "mode"));
  if (!mode) {
    return 0;
  }

  return strcmp(mode, expected) == 0;
}

static void capture_checkpoint(const turbo_graph_checkpoint_t *checkpoint, void *user_data) {
  checkpoint_capture_t *capture = (checkpoint_capture_t *)user_data;

  if (capture->serialized) {
    turbo_json_serialize_free(capture->serialized);
    capture->serialized = NULL;
  }

  capture->serialized = turbo_graph_checkpoint_serialize(checkpoint, &capture->len);
  capture->count++;
}

static void capture_checkpoint_copy(const turbo_graph_checkpoint_t *checkpoint, void *user_data) {
  turbo_graph_checkpoint_t **out_checkpoint = (turbo_graph_checkpoint_t **)user_data;
  char *serialized;
  size_t len = 0;

  check_not_null(checkpoint);
  check_not_null(out_checkpoint);

  if (*out_checkpoint) {
    turbo_graph_checkpoint_destroy(*out_checkpoint);
    *out_checkpoint = NULL;
  }

  serialized = turbo_graph_checkpoint_serialize(checkpoint, &len);
  check_not_null(serialized);
  check_int_eq(turbo_graph_checkpoint_deserialize(serialized, len, out_checkpoint),
               TURBO_GRAPH_EXEC_OK);
  turbo_json_serialize_free(serialized);
}

static void capture_graph_event(const json_value_t *event, void *user_data) {
  graph_event_capture_t *capture = (graph_event_capture_t *)user_data;
  const char *name;
  const char *detail;
  const char *payload;

  check_not_null(event);
  check_not_null(capture);
  name = turbo_runtime_json_value_as_string(turbo_json_object_get(event, "name"));
  detail =
      turbo_runtime_json_value_as_string(turbo_json_object_get(event, "detail"));
  payload =
      turbo_runtime_json_value_as_string(turbo_json_object_get(event, "payload"));
  capture->count++;
  if (name && strcmp(name, "graph.node") == 0) {
    capture->node_events++;
  } else if (name && strcmp(name, "graph.route") == 0) {
    capture->route_events++;
  }
  strncpy(capture->last_name, name ? name : "", sizeof(capture->last_name) - 1);
  capture->last_name[sizeof(capture->last_name) - 1] = '\0';
  strncpy(capture->last_detail, detail ? detail : "", sizeof(capture->last_detail) - 1);
  capture->last_detail[sizeof(capture->last_detail) - 1] = '\0';
  strncpy(capture->last_payload, payload ? payload : "", sizeof(capture->last_payload) - 1);
  capture->last_payload[sizeof(capture->last_payload) - 1] = '\0';
}

spec("turbo graph runtime") {

  describe("graph structure") {

    it("should add nodes and edges") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      string_payload_t start = {"start"};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "end", write_phase_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);
      check_size_eq(turbo_graph_node_count(graph), 2);
      check_size_eq(turbo_graph_edge_count(graph), 1);
      check_str_eq(turbo_graph_get_entry(graph), "start");

      turbo_graph_destroy(graph);
    }

    it("should reject duplicate nodes") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      string_payload_t node = {"x"};

      check_not_null(graph);
      check_int_eq(turbo_graph_add_node(graph, "dup", write_phase_node, &node),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "dup", write_phase_node, &node),
                   TURBO_GRAPH_EXEC_DUPLICATE_NODE);

      turbo_graph_destroy(graph);
    }
  }

  describe("execution") {

    it("should run a linear graph to completion") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_result_t result = {0};
      string_payload_t start = {"start"};
      string_payload_t middle = {"middle"};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "middle", write_phase_node, &middle),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "end", write_phase_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "middle", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "middle", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run(graph, state, NULL, &result), TURBO_GRAPH_EXEC_OK);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "end");
      check_null(result.next_node);
      check_size_eq(result.steps, 3);
      check_true(turbo_json_get_bool(state, "visited_end", false));

      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should route using the first matching conditional edge") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_result_t result = {0};
      string_payload_t tool = {"tool"};
      string_payload_t done = {"done"};

      check_not_null(graph);
      check_not_null(state);
      turbo_json_object_set_string(state, "mode", "tool");

      check_int_eq(turbo_graph_add_node(graph, "router", router_node, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "tool", write_phase_node, &tool),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "done", write_phase_node, &done),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(
          turbo_graph_add_edge(graph, "router", "tool", predicate_mode_equals, "tool"),
          TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "router", "done", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "router"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run(graph, state, NULL, &result), TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "tool");
      check_size_eq(result.steps, 2);
      check_true(turbo_json_get_bool(state, "visited_tool", false));

      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should honor node-level next overrides") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_result_t result = {0};
      string_payload_t chosen = {"chosen"};
      string_payload_t skipped = {"skipped"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", override_node, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "chosen", write_phase_node, &chosen),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "skipped", write_phase_node, &skipped),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "skipped", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run(graph, state, NULL, &result), TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "chosen");
      check_true(turbo_json_get_bool(state, "visited_chosen", false));

      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should stop at the configured step limit") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      string_payload_t a = {"A"};
      string_payload_t b = {"B"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "A", write_phase_node, &a),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "B", write_phase_node, &b),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "A", "B", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "B", "A", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "A"), TURBO_GRAPH_EXEC_OK);

      options.max_steps = 2;
      check_int_eq(turbo_graph_run(graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_STEP_LIMIT);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_STEP_LIMIT);
      check_str_eq(result.last_node, "B");
      check_str_eq(result.next_node, "A");
      check_size_eq(result.steps, 2);
      check_true(turbo_json_get_bool(state, "visited_B", false));

      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should checkpoint before the first node when already cancelled") {
      turbo_graph_t *graph = turbo_graph_create("agent-cancelled");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};
      turbo_graph_run_options_t options = {0};
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      checkpoint_capture_t capture = {0};
      string_payload_t start = {"start"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_json_value_node(
                       graph, "start", write_phase_json_value_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
      check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
      check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER),
                   SALTS_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      check_int_eq(turbo_graph_run_json_value_stream_controlled(
                       graph, state, &options, token, NULL, NULL, &result,
                       &result_state),
                   TURBO_GRAPH_EXEC_CANCELLED);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_CANCELLED);
      check_null(result.last_node);
      check_str_eq(result.next_node, "start");
      check_size_eq(result.steps, 0);
      check_int_eq(capture.count, 1);
      check_false(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_start"), 0));

      turbo_json_serialize_free(capture.serialized);
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should stop after a routed checkpoint and resume with a fresh token") {
      turbo_graph_t *graph = turbo_graph_create("agent-cancel-resume");
      json_value_t *state = turbo_json_create_object();
      json_value_t *cancelled_state = NULL;
      json_value_t *resumed_state = NULL;
      turbo_graph_run_result_t cancelled_result = {0};
      turbo_graph_run_result_t resumed_result = {0};
      turbo_graph_run_options_t options = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      cancel_node_payload_t start = {0};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
      check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
      start.source = source;
      start.value = "start";

      check_int_eq(turbo_graph_add_json_value_node(
                       graph, "start", cancel_after_write_json_value_node,
                       &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(
                       graph, "end", write_phase_json_value_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"),
                   TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint_copy;
      options.checkpoint_user_data = &checkpoint;
      check_int_eq(turbo_graph_run_json_value_stream_controlled(
                       graph, state, &options, token, NULL, NULL,
                       &cancelled_result, &cancelled_state),
                   TURBO_GRAPH_EXEC_CANCELLED);
      check_str_eq(cancelled_result.last_node, "start");
      check_str_eq(cancelled_result.next_node, "end");
      check_size_eq(cancelled_result.steps, 1);
      check_not_null(checkpoint);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(cancelled_state, "visited_start"), 0));
      check_false(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(cancelled_state, "visited_end"), 0));

      check_int_eq(turbo_graph_run_checkpoint_json_value(
                       graph, checkpoint, NULL, &resumed_result,
                       &resumed_state),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(resumed_result.last_node, "end");
      check_size_eq(resumed_result.steps, 2);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(resumed_state, "visited_end"), 0));

      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      turbo_runtime_json_destroy(resumed_state);
      turbo_runtime_json_destroy(cancelled_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should distinguish an expired deadline from explicit cancellation") {
      turbo_graph_t *graph = turbo_graph_create("agent-deadline");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};
      turbo_cancel_source_config_t config = {
          sizeof(config), TURBO_RUNTIME_CONTROL_ABI_VERSION,
          salts_monotonic_ms(), NULL, NULL};
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      string_payload_t start = {"start"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_json_value_node(
                       graph, "start", write_phase_json_value_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_cancel_source_create(&config, &source), SALTS_OK);
      check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);

      check_int_eq(turbo_graph_run_json_value_stream_controlled(
                       graph, state, NULL, token, NULL, NULL, &result,
                       &result_state),
                   TURBO_GRAPH_EXEC_DEADLINE);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_DEADLINE);
      check_str_eq(result.next_node, "start");
      check_int_eq(turbo_cancel_token_reason(token), TURBO_CANCEL_DEADLINE);

      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should run through a TurboParser JSON state boundary") {
      turbo_graph_t *graph = turbo_graph_create("agent-bind");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};
      string_payload_t start = {"start"};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "end", write_phase_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run_json_value(graph, state, NULL, &result, &result_state),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_OK);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0));

      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should run TurboParser JSON-native nodes and predicates without json bridge") {
      turbo_graph_t *graph = turbo_graph_create("agent-json-native");
      json_value_t *state = turbo_json_create_object();
      json_value_t *mode = turbo_json_create_string("tool");
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};

      check_not_null(graph);
      check_not_null(state);
      check_not_null(mode);
      check_int_eq(turbo_runtime_json_object_set(state, "mode", mode),
                   TURBO_RUNTIME_JSON_OK);

      check_int_eq(turbo_graph_add_json_value_node(graph, "router", write_phase_json_value_node,
                                             (void *)&s_router_payload),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(graph, "tool", write_phase_json_value_node,
                                             (void *)&s_tool_payload),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(graph, "done", write_phase_json_value_node,
                                             (void *)&s_done_payload),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(
          turbo_graph_add_json_value_edge(graph, "router", "tool", predicate_mode_equals_json_value, "tool"),
          TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_edge(graph, "router", "done", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "router"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run_json_value(graph, state, NULL, &result, &result_state),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "tool");
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_tool"), 0));
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_router"), 0));

      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should emit canonical trace events while running a TurboParser JSON-native graph") {
      turbo_graph_t *graph = turbo_graph_create("agent-bind-stream");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};
      graph_event_capture_t capture = {0};
      string_payload_t start = {"start"};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_json_value_node(graph, "start", write_phase_json_value_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(graph, "end", write_phase_json_value_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(
          turbo_graph_run_json_value_stream(graph, state, NULL, capture_graph_event, &capture, &result,
                                      &result_state),
          TURBO_GRAPH_EXEC_OK);
      check_not_null(result_state);
      check_int_eq(capture.count, 6);
      check_int_eq(capture.node_events, 4);
      check_int_eq(capture.route_events, 2);
      check_str_eq(capture.last_name, "graph.route");
      check_str_eq(capture.last_detail, "complete");
      check_str_eq(capture.last_payload, "end");

      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should return TurboParser JSON-native state written by a failing node") {
      turbo_graph_t *graph = turbo_graph_create("agent-bind-error-state");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_run_result_t result = {0};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_json_value_node(graph, "error", write_error_json_value_node, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "error"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_run_json_value_stream(graph, state, NULL, NULL, NULL, &result,
                                               &result_state),
                   TURBO_GRAPH_EXEC_ERROR);
      check_not_null(result_state);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_ERROR);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "error_recorded"), 0));

      turbo_runtime_json_destroy(result_state);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }

    it("should resume a TurboParser JSON-native graph from checkpoint and continue the event log") {
      turbo_graph_t *graph = turbo_graph_create("agent-bind-resume");
      json_value_t *state = turbo_json_create_object();
      json_value_t *result_state = NULL;
      turbo_graph_checkpoint_t *checkpoint = NULL;
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      turbo_graph_run_result_t resumed = {0};
      turbo_event_log_t *log = turbo_event_log_create();
      const char *interrupt_before[] = {"end"};
      string_payload_t start = {"start"};
      string_payload_t middle = {"middle"};
      string_payload_t end = {"end"};
      const json_value_t *last_event;

      check_not_null(graph);
      check_not_null(state);
      check_not_null(log);
      check_int_eq(turbo_graph_add_json_value_node(graph, "start", write_phase_json_value_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(graph, "middle", write_phase_json_value_node, &middle),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_node(graph, "end", write_phase_json_value_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_edge(graph, "start", "middle", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_json_value_edge(graph, "middle", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      options.checkpoint_cb = capture_checkpoint_copy;
      options.checkpoint_user_data = &checkpoint;

      check_int_eq(turbo_graph_run_json_value_stream(graph, state, &options, turbo_event_log_capture_json_value,
                                               log, &result, &result_state),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_not_null(checkpoint);
      check_not_null(result_state);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_INTERRUPTED);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint), "end");
      check_size_eq(turbo_graph_checkpoint_steps(checkpoint), 2);
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_middle"), 0));
      check_false(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0));
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_int_eq((int)turbo_event_log_size(log), 6);

      turbo_runtime_json_destroy(result_state);
      result_state = NULL;

      check_int_eq(turbo_graph_run_checkpoint_json_value_stream(graph, checkpoint, NULL,
                                                          turbo_event_log_capture_json_value, log,
                                                          &resumed, &result_state),
                   TURBO_GRAPH_EXEC_OK);
      check_not_null(result_state);
      check_int_eq(resumed.status, TURBO_GRAPH_EXEC_OK);
      check_str_eq(resumed.last_node, "end");
      check_true(turbo_runtime_json_value_as_bool(
          turbo_json_object_get(result_state, "visited_end"), 0));
      check_int_eq(turbo_event_log_status(log), TURBO_EVENT_LOG_OK);
      check_int_eq((int)turbo_event_log_size(log), 9);

      last_event = turbo_event_log_get(log, turbo_event_log_size(log) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "name")),
                   "graph.route");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "detail")),
                   "complete");
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(last_event, "payload")),
                   "end");

      turbo_runtime_json_destroy(result_state);
      turbo_event_log_destroy(log);
      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_runtime_json_destroy(state);
      turbo_graph_destroy(graph);
    }
  }

  describe("checkpointing") {

    it("should keep a stable topology id across equivalent graphs built in different orders") {
      turbo_graph_t *source_graph = turbo_graph_create("agent");
      turbo_graph_t *other_graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      const char *interrupt_nodes[] = {"review"};
      const char *source_topology_id;
      const char *other_topology_id;
      string_payload_t start = {"start"};
      string_payload_t review = {"review"};
      string_payload_t done = {"done"};

      check_not_null(source_graph);
      check_not_null(other_graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node_ex(source_graph, "start", "node.start", write_phase_node,
                                           &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node_ex(source_graph, "review", "node.review", write_phase_node,
                                           &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node_ex(source_graph, "done", "node.done", write_phase_node,
                                           &done),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge_ex(source_graph, "start", "review",
                                           "edge.start.review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge_ex(source_graph, "review", "done", "edge.review.done",
                                           NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(source_graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_add_node_ex(other_graph, "done", "node.done",
                                           write_phase_node_alt, &done),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node_ex(other_graph, "start", "node.start",
                                           write_phase_node_alt, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node_ex(other_graph, "review", "node.review",
                                           write_phase_node_alt, &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge_ex(other_graph, "review", "done", "edge.review.done",
                                           NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge_ex(other_graph, "start", "review",
                                           "edge.start.review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(other_graph, "start"), TURBO_GRAPH_EXEC_OK);

      source_topology_id = turbo_graph_topology_id(source_graph);
      other_topology_id = turbo_graph_topology_id(other_graph);
      check_not_null(source_topology_id);
      check_not_null(other_topology_id);
      check_str_eq(source_topology_id, other_topology_id);

      options.interrupt_before_nodes = interrupt_nodes;
      options.interrupt_before_count = 1;
      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      check_int_eq(turbo_graph_run(source_graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_not_null(capture.serialized);
      check_true(strstr(capture.serialized, "\"topology_id\":") != NULL);
      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len,
                                                      &checkpoint),
                   TURBO_GRAPH_EXEC_OK);
      check_not_null(turbo_graph_checkpoint_topology_id(checkpoint));
      check_str_eq(turbo_graph_checkpoint_topology_id(checkpoint), source_topology_id);

      options.interrupt_before_nodes = NULL;
      options.interrupt_before_count = 0;
      check_int_eq(turbo_graph_run_checkpoint(other_graph, checkpoint, &options, &result),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "done");
      check_size_eq(result.steps, 3);

      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(other_graph);
      turbo_graph_destroy(source_graph);
    }

    it("should emit resumable checkpoints between nodes") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      turbo_graph_checkpoint_t *checkpoint_clone = NULL;
      string_payload_t start = {"start"};
      string_payload_t end = {"end"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "end", write_phase_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      check_int_eq(turbo_graph_run(graph, state, &options, &result), TURBO_GRAPH_EXEC_OK);
      check_true(capture.count >= 1);
      check_not_null(capture.serialized);
      check_true(strstr(capture.serialized, "\"topology_id\":") != NULL);

      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len,
                                                      &checkpoint),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint), "end");
      check_size_eq(turbo_graph_checkpoint_steps(checkpoint), 1);
      check_not_null(turbo_graph_checkpoint_topology_id(checkpoint));
      check_str_eq(turbo_graph_checkpoint_topology_id(checkpoint), turbo_graph_topology_id(graph));
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint),
                                     "visited_start", false));
      check_int_eq(turbo_graph_checkpoint_clone(checkpoint, &checkpoint_clone),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint_clone), "end");
      check_size_eq(turbo_graph_checkpoint_steps(checkpoint_clone), 1);
      check_not_null(turbo_graph_checkpoint_topology_id(checkpoint_clone));
      check_str_eq(turbo_graph_checkpoint_topology_id(checkpoint_clone),
                   turbo_graph_topology_id(graph));
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint_clone),
                                     "visited_start", false));

      turbo_graph_checkpoint_destroy(checkpoint_clone);
      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should round-trip checkpoint serialization") {
      json_value_t *state = turbo_json_create_object();
      turbo_graph_checkpoint_t *checkpoint = NULL;
      turbo_graph_checkpoint_t *parsed = NULL;
      char *json = NULL;
      size_t len = 0;

      check_not_null(state);
      turbo_json_object_set_string(state, "phase", "tool_result");

      check_int_eq(
          turbo_graph_checkpoint_create("tool_node", 3, state, &checkpoint),
          TURBO_GRAPH_EXEC_OK);
      check_size_eq(turbo_graph_checkpoint_schema_version(), 3);
      json = turbo_graph_checkpoint_serialize(checkpoint, &len);
      check_not_null(json);
      check_size_gt(len, 0);
      check_true(strstr(json, "\"checkpoint_version\":3") != NULL);
      check_true(strstr(json, "\"topology_id\":\"\"") != NULL);
      check_int_eq(turbo_graph_checkpoint_deserialize(json, len, &parsed),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(turbo_graph_checkpoint_next_node(parsed), "tool_node");
      check_size_eq(turbo_graph_checkpoint_steps(parsed), 3);
      check_null(turbo_graph_checkpoint_topology_id(parsed));
      check_str_eq(turbo_json_get_string(turbo_graph_checkpoint_state(parsed), "phase"),
                   "tool_result");

      turbo_graph_checkpoint_destroy(parsed);
      turbo_json_serialize_free(json);
      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_free_json(&state);
    }

    it("should reject legacy checkpoints without topology ids") {
      const char *json =
          "{\"checkpoint_version\":2,\"next_node\":\"tool_node\",\"steps\":3,"
          "\"state\":{\"phase\":\"tool_result\"}}";
      turbo_graph_checkpoint_t *parsed = NULL;

      check_int_eq(turbo_graph_checkpoint_deserialize(json, strlen(json), &parsed),
                   TURBO_GRAPH_EXEC_ERROR);
      check_null(parsed);
    }

    it("should create and read checkpoints through TurboParser JSON") {
      json_value_t *state = turbo_json_create_object();
      json_value_t *phase = turbo_json_create_string("tool");
      json_value_t *bound = NULL;
      turbo_graph_checkpoint_t *checkpoint = NULL;

      check_not_null(state);
      check_not_null(phase);
      check_int_eq(turbo_runtime_json_object_set(state, "phase", phase),
                   TURBO_RUNTIME_JSON_OK);
      check_int_eq(turbo_graph_checkpoint_create_json_value("tool_node", 3, state, &checkpoint),
                   TURBO_GRAPH_EXEC_OK);

      bound = turbo_graph_checkpoint_state_json_value(checkpoint);
      check_not_null(bound);
      check_str_eq(turbo_runtime_json_value_as_string(
                       turbo_json_object_get(bound, "phase")),
                   "tool");

      turbo_runtime_json_destroy(bound);
      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_runtime_json_destroy(state);
    }

    it("should interrupt before a node and emit a resumable checkpoint") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      const char *interrupt_nodes[] = {"review"};
      string_payload_t start = {"start"};
      string_payload_t review = {"review"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "review", write_phase_node, &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      options.interrupt_before_nodes = interrupt_nodes;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_graph_run(graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_INTERRUPTED);
      check_str_eq(result.last_node, "start");
      check_str_eq(result.next_node, "review");
      check_size_eq(result.steps, 1);
      check_int_eq(capture.count, 1);
      check_not_null(capture.serialized);

      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len,
                                                      &checkpoint),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint), "review");
      check_size_eq(turbo_graph_checkpoint_steps(checkpoint), 1);
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint),
                                     "visited_start", false));

      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should interrupt with a checkpoint when a node stops with an explicit next node") {
      turbo_graph_t *graph = turbo_graph_create("approval-stop");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      string_payload_t tool = {"tool"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "tool", stop_and_resume_node, "tool"),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "tool"), TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;

      check_int_eq(turbo_graph_run(graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_INTERRUPTED);
      check_str_eq(result.last_node, "tool");
      check_str_eq(result.next_node, "tool");
      check_size_eq(result.steps, 1);
      check_int_eq(capture.count, 1);
      check_not_null(capture.serialized);
      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len,
                                                      &checkpoint),
                   TURBO_GRAPH_EXEC_OK);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint), "tool");
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint),
                                     "needs_review", false));

      (void)tool;
      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should resume execution from a checkpoint") {
      turbo_graph_t *graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      const char *interrupt_nodes[] = {"review"};
      string_payload_t start = {"start"};
      string_payload_t review = {"review"};
      string_payload_t done = {"done"};

      check_not_null(graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "review", write_phase_node, &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(graph, "done", write_phase_node, &done),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "start", "review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(graph, "review", "done", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      options.interrupt_before_nodes = interrupt_nodes;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_graph_run(graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len,
                                                      &checkpoint),
                   TURBO_GRAPH_EXEC_OK);

      options.interrupt_before_nodes = NULL;
      options.interrupt_before_count = 0;
      check_int_eq(turbo_graph_run_checkpoint(graph, checkpoint, &options, &result),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_OK);
      check_str_eq(result.last_node, "done");
      check_size_eq(result.steps, 3);
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint),
                                     "visited_review", false));
      check_true(turbo_json_get_bool(turbo_graph_checkpoint_state(checkpoint),
                                     "visited_done", false));

      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(graph);
    }

    it("should reject checkpoint resume against a different graph topology") {
      turbo_graph_t *source_graph = turbo_graph_create("agent");
      turbo_graph_t *other_graph = turbo_graph_create("agent");
      json_value_t *state = turbo_json_create_object();
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      checkpoint_capture_t capture = {0};
      turbo_graph_checkpoint_t *checkpoint = NULL;
      const char *interrupt_nodes[] = {"review"};
      string_payload_t start = {"start"};
      string_payload_t review = {"review"};
      string_payload_t done = {"done"};
      string_payload_t other = {"other"};

      check_not_null(source_graph);
      check_not_null(other_graph);
      check_not_null(state);
      check_int_eq(turbo_graph_add_node(source_graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(source_graph, "review", write_phase_node, &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(source_graph, "done", write_phase_node, &done),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(source_graph, "start", "review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(source_graph, "review", "done", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(source_graph, "start"), TURBO_GRAPH_EXEC_OK);

      check_int_eq(turbo_graph_add_node(other_graph, "start", write_phase_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(other_graph, "review", write_phase_node, &review),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_node(other_graph, "other", write_phase_node, &other),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(other_graph, "start", "review", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_edge(other_graph, "review", "other", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(other_graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.checkpoint_cb = capture_checkpoint;
      options.checkpoint_user_data = &capture;
      options.interrupt_before_nodes = interrupt_nodes;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_graph_run(source_graph, state, &options, &result),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_int_eq(turbo_graph_checkpoint_deserialize(capture.serialized, capture.len, &checkpoint),
                   TURBO_GRAPH_EXEC_OK);

      options.interrupt_before_nodes = NULL;
      options.interrupt_before_count = 0;
      check_int_eq(turbo_graph_run_checkpoint(other_graph, checkpoint, &options, &result),
                   TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_CHECKPOINT_MISMATCH);
      check_str_eq(result.next_node, "review");
      check_size_eq(result.steps, 1);

      turbo_graph_checkpoint_destroy(checkpoint);
      turbo_json_serialize_free(capture.serialized);
      turbo_free_json(&state);
      turbo_graph_destroy(other_graph);
      turbo_graph_destroy(source_graph);
    }
  }
}
