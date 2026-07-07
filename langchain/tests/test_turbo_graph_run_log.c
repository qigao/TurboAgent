#include "tinytest.h"
#include "turbo_graph_run_log.h"

#include <stdio.h>

typedef struct {
  const char *value;
} string_payload_t;

static int write_phase_bind_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  string_payload_t *payload = (string_payload_t *)user_data;
  char key[64];
  turbo_runtime_data_bind_value_t *value;

  snprintf(key, sizeof(key), "visited_%s", payload->value);
  value = turbo_runtime_data_bind_value_create_bool(1);
  check_not_null(value);
  return turbo_runtime_data_bind_object_set(ctx->bind_state, key, value) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

spec("turbo graph run log runtime") {

  describe("bind-native recording") {

    it("should capture one interrupted bind-native run segment") {
      turbo_graph_t *graph = turbo_graph_create("graph-run-log");
      turbo_graph_run_log_t *log = turbo_graph_run_log_create();
      turbo_runtime_data_bind_value_t *state = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *result_state = NULL;
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      const char *interrupt_before[] = {"end"};
      string_payload_t start = {"start"};
      string_payload_t middle = {"middle"};
      string_payload_t end = {"end"};
      const turbo_graph_checkpoint_t *checkpoint;

      check_not_null(graph);
      check_not_null(log);
      check_not_null(state);
      check_int_eq(turbo_graph_add_bind_node(graph, "start", write_phase_bind_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_node(graph, "middle", write_phase_bind_node, &middle),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_node(graph, "end", write_phase_bind_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_edge(graph, "start", "middle", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_edge(graph, "middle", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;

      check_int_eq(turbo_graph_run_bind_log(graph, state, &options, log, &result, &result_state),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_not_null(result_state);
      check_int_eq(result.status, TURBO_GRAPH_EXEC_INTERRUPTED);
      check_int_eq((int)turbo_event_log_size(turbo_graph_run_log_events(log)), 6);
      checkpoint = turbo_graph_run_log_checkpoint(log);
      check_not_null(checkpoint);
      check_str_eq(turbo_graph_checkpoint_next_node(checkpoint), "end");
      check_size_eq(turbo_graph_checkpoint_steps(checkpoint), 2);
      check_not_null(turbo_graph_checkpoint_topology_id(checkpoint));
      check_str_eq(turbo_graph_checkpoint_topology_id(checkpoint), turbo_graph_topology_id(graph));

      turbo_runtime_data_bind_value_destroy(result_state);
      turbo_runtime_data_bind_value_destroy(state);
      turbo_graph_run_log_destroy(log);
      turbo_graph_destroy(graph);
    }

    it("should resume one checkpointed bind-native segment into a fresh run log") {
      turbo_graph_t *graph = turbo_graph_create("graph-run-log-resume");
      turbo_graph_run_log_t *log = turbo_graph_run_log_create();
      turbo_runtime_data_bind_value_t *state = turbo_runtime_data_bind_value_create_object();
      turbo_runtime_data_bind_value_t *result_state = NULL;
      turbo_graph_run_options_t options = {0};
      turbo_graph_run_result_t result = {0};
      turbo_graph_run_result_t resumed = {0};
      const char *interrupt_before[] = {"end"};
      string_payload_t start = {"start"};
      string_payload_t middle = {"middle"};
      string_payload_t end = {"end"};
      const turbo_graph_checkpoint_t *checkpoint;
      const turbo_runtime_data_bind_value_t *last_event;

      check_not_null(graph);
      check_not_null(log);
      check_not_null(state);
      check_int_eq(turbo_graph_add_bind_node(graph, "start", write_phase_bind_node, &start),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_node(graph, "middle", write_phase_bind_node, &middle),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_node(graph, "end", write_phase_bind_node, &end),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_edge(graph, "start", "middle", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_add_bind_edge(graph, "middle", "end", NULL, NULL),
                   TURBO_GRAPH_EXEC_OK);
      check_int_eq(turbo_graph_set_entry(graph, "start"), TURBO_GRAPH_EXEC_OK);

      options.interrupt_before_nodes = interrupt_before;
      options.interrupt_before_count = 1;
      check_int_eq(turbo_graph_run_bind_log(graph, state, &options, log, &result, &result_state),
                   TURBO_GRAPH_EXEC_INTERRUPTED);
      check_not_null(result_state);
      checkpoint = turbo_graph_run_log_checkpoint(log);
      check_not_null(checkpoint);
      turbo_runtime_data_bind_value_destroy(result_state);
      result_state = NULL;

      check_int_eq(
          turbo_graph_run_checkpoint_bind_log(graph, checkpoint, NULL, log, &resumed, &result_state),
          TURBO_GRAPH_EXEC_OK);
      check_not_null(result_state);
      check_int_eq(resumed.status, TURBO_GRAPH_EXEC_OK);
      check_true(turbo_runtime_data_bind_value_as_bool(
          turbo_runtime_data_bind_object_get(result_state, "visited_end"), 0));
      check_null(turbo_graph_run_log_checkpoint(log));
      check_int_eq((int)turbo_event_log_size(turbo_graph_run_log_events(log)), 3);

      last_event = turbo_event_log_get(turbo_graph_run_log_events(log),
                                       turbo_event_log_size(turbo_graph_run_log_events(log)) - 1);
      check_not_null(last_event);
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(last_event, "name")),
                   "graph.route");
      check_str_eq(turbo_runtime_data_bind_value_as_string(
                       turbo_runtime_data_bind_object_get(last_event, "detail")),
                   "complete");

      turbo_runtime_data_bind_value_destroy(result_state);
      turbo_runtime_data_bind_value_destroy(state);
      turbo_graph_run_log_destroy(log);
      turbo_graph_destroy(graph);
    }
  }
}
