#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_state.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  const char *key;
  int value;
} runtime_history_bool_write_t;

static const char *runtime_history_text(const char *text) {
  return text ? text : "(null)";
}

static int runtime_history_write_bool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  runtime_history_bool_write_t *write = (runtime_history_bool_write_t *)user_data;
  turbo_runtime_data_bind_value_t *value = turbo_runtime_data_bind_value_create_bool(write->value);

  if (!value) {
    return -1;
  }
  return turbo_runtime_data_bind_object_set(ctx->bind_state, write->key, value) ==
                 TURBO_RUNTIME_DATA_BIND_OK
             ? 0
             : -1;
}

static turbo_graph_t *runtime_history_create_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-history-example");
  static runtime_history_bool_write_t start = {"visited_start", 1};
  static runtime_history_bool_write_t end = {"visited_end", 1};

  if (!graph) {
    return NULL;
  }
  if (turbo_graph_add_bind_node(graph, "start", runtime_history_write_bool_node, &start) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_node(graph, "end", runtime_history_write_bool_node, &end) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "start") != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

static turbo_runtime_data_bind_value_t *runtime_history_initial_state(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  if (!state) {
    return NULL;
  }
  if (turbo_agent_state_request_review(state, "inspect history before resume") != 0 ||
      turbo_agent_state_set_review_approved(state, 0) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *runtime_history_approve_review_command(void) {
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

static void runtime_history_print_record_header(const char *label, const json_value_t *record) {
  if (!record) {
    return;
  }
  printf("%s\n", label);
  printf("  id: %s\n", runtime_history_text(turbo_json_get_string(record, "id")));
  printf("  status: %s\n", runtime_history_text(turbo_json_get_string(record, "status")));
}

int main(void) {
  turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
  turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
  turbo_graph_t *graph = runtime_history_create_graph();
  turbo_runtime_data_bind_value_t *state = runtime_history_initial_state();
  turbo_runtime_data_bind_value_t *result_state = NULL;
  turbo_runtime_data_bind_value_t *command = NULL;
  turbo_runtime_data_bind_value_t *override = NULL;
  turbo_runtime_data_bind_value_t *history = NULL;
  json_value_t *summary = NULL;
  json_value_t *thread_record = NULL;
  json_value_t *run_record = NULL;
  json_value_t *checkpoint_record = NULL;
  json_value_t *runs = NULL;
  json_value_t *checkpoints = NULL;
  turbo_graph_run_options_t options = {0};
  const char *interrupt_before_review[] = {"review"};
  const char *thread_id;
  const char *run_id;
  const char *checkpoint_id;

  if (!runtime || !graph || !state) {
    fprintf(stderr, "failed to initialize runtime history example\n");
    return 1;
  }

  options.interrupt_before_nodes = interrupt_before_review;
  options.interrupt_before_count = 1;
  if (turbo_agent_runtime_exec_start(runtime, graph, state, &options, NULL, &summary, &result_state) != 0) {
    fprintf(stderr, "start_bind_graph failed\n");
    return 1;
  }

  thread_id = turbo_json_get_string(summary, "thread_id");
  run_id = turbo_json_get_string(summary, "run_id");
  checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  printf("summary\n");
  printf("  thread_id: %s\n", runtime_history_text(thread_id));
  printf("  run_id: %s\n", runtime_history_text(run_id));
  printf("  checkpoint_id: %s\n", runtime_history_text(checkpoint_id));
  printf("  parent_agent_run_id: %s\n",
         runtime_history_text(turbo_json_get_string(summary, "parent_agent_run_id")));
  printf("  parent_tool_call_id: %s\n",
         runtime_history_text(turbo_json_get_string(summary, "parent_tool_call_id")));
  printf("  parent_tool_name: %s\n",
         runtime_history_text(turbo_json_get_string(summary, "parent_tool_name")));

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_record) != 0 ||
      turbo_agent_runtime_get_run(runtime, run_id, &run_record) != 0 ||
      turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint_record) != 0 ||
      turbo_agent_runtime_list_runs(runtime, thread_id, &runs) != 0 ||
      turbo_agent_runtime_list_checkpoints(runtime, run_id, &checkpoints) != 0 ||
      turbo_agent_runtime_load_history_events_bind(runtime, run_id, NULL, &history) != 0) {
    fprintf(stderr, "runtime query failed\n");
    return 1;
  }

  runtime_history_print_record_header("thread record", thread_record);
  runtime_history_print_record_header("run record", run_record);
  runtime_history_print_record_header("checkpoint record", checkpoint_record);
  printf("list_runs count: %zu\n", turbo_json_array_size(runs));
  printf("list_checkpoints count: %zu\n", turbo_json_array_size(checkpoints));
  printf("history event count: %zu\n", turbo_runtime_data_bind_value_size(history));

  command = runtime_history_approve_review_command();
  turbo_runtime_data_bind_value_destroy(result_state);
  result_state = NULL;
  if (!command ||
      turbo_agent_runtime_prepare_checkpoint_command_override_bind(runtime, checkpoint_id, command, &override) != 0 ||
      !override) {
    fprintf(stderr, "failed to build review override\n");
    return 1;
  }
  if (turbo_agent_runtime_exec_resume(runtime, graph, override, NULL, &(turbo_agent_runtime_exec_options_t){ .scope = TURBO_RUNTIME_SCOPE_CHECKPOINT, .input_kind = TURBO_RUNTIME_INPUT_OVERRIDE, .checkpoint_id = checkpoint_id }, &summary, &result_state) != 0) {
    fprintf(stderr, "resume_bind_graph failed\n");
    return 1;
  }
  printf("resumed status: %s\n", runtime_history_text(turbo_json_get_string(summary, "status")));

  turbo_free_json(&summary);
  turbo_free_json(&thread_record);
  turbo_free_json(&run_record);
  turbo_free_json(&checkpoint_record);
  turbo_free_json(&runs);
  turbo_free_json(&checkpoints);
  turbo_runtime_data_bind_value_destroy(command);
  turbo_runtime_data_bind_value_destroy(history);
  turbo_runtime_data_bind_value_destroy(result_state);
  turbo_runtime_data_bind_value_destroy(override);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_graph_destroy(graph);
  turbo_agent_runtime_destroy(runtime);
  return 0;
}
