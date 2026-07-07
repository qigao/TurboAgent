#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_state.h"
#include "turbo_event.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const char *key;
  int value;
} runtime_live_stream_bool_write_t;

typedef struct {
  size_t count;
} runtime_live_stream_capture_t;

static const char *runtime_live_stream_text(const char *text) {
  return text ? text : "(null)";
}

static int runtime_live_stream_write_bool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  runtime_live_stream_bool_write_t *write = (runtime_live_stream_bool_write_t *)user_data;
  turbo_runtime_data_bind_value_t *value;

  value = turbo_runtime_data_bind_value_create_bool(write->value);
  if (!value) {
    return -1;
  }
  return turbo_runtime_data_bind_object_set(ctx->bind_state, write->key, value) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static turbo_graph_t *runtime_live_stream_create_review_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-live-stream-example");
  static runtime_live_stream_bool_write_t start = {"visited_start", 1};
  static runtime_live_stream_bool_write_t end = {"visited_end", 1};

  if (!graph) {
    return NULL;
  }
  if (turbo_graph_add_bind_node(graph, "start", runtime_live_stream_write_bool_node, &start) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_node(graph, "end", runtime_live_stream_write_bool_node, &end) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "start") != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }

  return graph;
}

static turbo_runtime_data_bind_value_t *runtime_live_stream_initial_state(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  if (!state) {
    return NULL;
  }
  if (turbo_agent_state_request_review(state, "stream canonical events while resuming") != 0 ||
      turbo_agent_state_set_review_approved(state, 0) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *runtime_live_stream_approve_review_command(void) {
  turbo_runtime_data_bind_value_t *command =
      turbo_runtime_data_bind_value_create_object();

  if (!command) {
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(
          command, "kind",
          turbo_runtime_data_bind_value_create_string("approve_review")) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(command);
    return NULL;
  }
  return command;
}

static void runtime_live_stream_capture_event(const turbo_runtime_data_bind_value_t *event,
                                              void *user_data) {
  runtime_live_stream_capture_t *capture = (runtime_live_stream_capture_t *)user_data;

  if (!capture) {
    return;
  }
  capture->count += 1;
  printf("  event kind: %s\n", runtime_live_stream_text(turbo_event_kind_bind(event)));
}

static void runtime_live_stream_print_summary(const char *label, const json_value_t *summary) {
  if (!summary) {
    return;
  }
  printf(
      "%s\n"
      "  thread_id: %s\n"
      "  run_id: %s\n"
      "  status: %s\n"
      "  checkpoint_id: %s\n",
      label, runtime_live_stream_text(turbo_json_get_string(summary, "thread_id")),
      runtime_live_stream_text(turbo_json_get_string(summary, "run_id")),
      runtime_live_stream_text(turbo_json_get_string(summary, "status")),
      turbo_json_is_null(turbo_json_object_get(summary, "checkpoint_id"))
          ? "(null)"
          : runtime_live_stream_text(turbo_json_get_string(summary, "checkpoint_id")));
}

int main(void) {
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime = NULL;
  turbo_graph_t *graph = NULL;
  turbo_runtime_data_bind_value_t *state = NULL;
  turbo_runtime_data_bind_value_t *result_state = NULL;
  turbo_runtime_data_bind_value_t *command = NULL;
  turbo_runtime_data_bind_value_t *override = NULL;
  json_value_t *summary = NULL;
  json_value_t *resume_summary = NULL;
  turbo_graph_run_options_t options = {0};
  runtime_live_stream_capture_t capture = {0};
  const char *interrupt_before_review[] = {"review"};
  const char *checkpoint_id;

  store = turbo_agent_runtime_store_memory_create();
  runtime = turbo_agent_runtime_create(&store);
  graph = runtime_live_stream_create_review_graph();
  state = runtime_live_stream_initial_state();
  if (!runtime || !graph || !state) {
    fprintf(stderr, "failed to initialize runtime live stream example\n");
    return 1;
  }

  options.interrupt_before_nodes = interrupt_before_review;
  options.interrupt_before_count = 1;
  if (turbo_agent_runtime_exec_start(runtime, graph, state, &options, NULL, &summary, &result_state) != 0) {
    fprintf(stderr, "start_bind_graph failed\n");
    return 1;
  }

  runtime_live_stream_print_summary("initial run", summary);
  checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  if (!checkpoint_id) {
    fprintf(stderr, "expected checkpoint id after interrupt\n");
    return 1;
  }

  command = runtime_live_stream_approve_review_command();
  if (!command ||
      turbo_agent_runtime_prepare_checkpoint_command_override_bind(runtime, checkpoint_id, command,
                                                        &override) != 0 ||
      !override) {
    fprintf(stderr, "apply_checkpoint_command_bind failed\n");
    return 1;
  }

  turbo_runtime_data_bind_value_destroy(result_state);
  result_state = NULL;
  options.interrupt_before_nodes = NULL;
  options.interrupt_before_count = 0;

  printf("live stream\n");
  if (turbo_agent_runtime_exec_resume(runtime, graph, override, &options, &(turbo_agent_runtime_exec_options_t){ .scope = TURBO_RUNTIME_SCOPE_CHECKPOINT, .input_kind = TURBO_RUNTIME_INPUT_OVERRIDE, .checkpoint_id = checkpoint_id, .event_sink = runtime_live_stream_capture_event, .event_sink_user_data = &capture }, &resume_summary, &result_state) != 0) {
    fprintf(stderr, "resume_bind_graph_stream failed\n");
    return 1;
  }
  if (capture.count == 0) {
    fprintf(stderr, "expected live stream events while resuming\n");
    return 1;
  }

  runtime_live_stream_print_summary("live summary", resume_summary);

  turbo_free_json(&resume_summary);
  turbo_free_json(&summary);
  turbo_runtime_data_bind_value_destroy(override);
  turbo_runtime_data_bind_value_destroy(command);
  turbo_runtime_data_bind_value_destroy(result_state);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_graph_destroy(graph);
  turbo_agent_runtime_destroy(runtime);
  return 0;
}
