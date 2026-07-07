#include "turbo_agent_graph.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *key;
  int value;
} runtime_example_bool_write_t;

static const char *runtime_example_text(const char *text) {
  return text ? text : "(null)";
}

static int runtime_example_write_bool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  runtime_example_bool_write_t *write = (runtime_example_bool_write_t *)user_data;
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

static turbo_graph_t *runtime_example_create_review_graph(void) {
  turbo_graph_t *graph = turbo_graph_create("runtime-review-example");
  static runtime_example_bool_write_t start = {"visited_start", 1};
  static runtime_example_bool_write_t end = {"visited_end", 1};

  if (!graph) {
    return NULL;
  }
  if (turbo_graph_add_bind_node(graph, "start", runtime_example_write_bool_node, &start) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_node(graph, "review", turbo_agent_review_node, NULL) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_node(graph, "end", runtime_example_write_bool_node, &end) !=
          TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "start", "review", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_add_bind_edge(graph, "review", "end", NULL, NULL) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(graph, "start") != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }

  return graph;
}

static turbo_runtime_data_bind_value_t *runtime_example_initial_state(void) {
  json_value_t *state = turbo_agent_state_create();
  turbo_runtime_data_bind_value_t *bound;

  if (!state) {
    return NULL;
  }
  if (turbo_agent_state_request_review(state, "approve before end") != 0 ||
      turbo_agent_state_set_review_approved(state, 0) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  bound = turbo_runtime_data_bind_value_from_json(state);
  turbo_free_json(&state);
  return bound;
}

static turbo_runtime_data_bind_value_t *runtime_example_approve_review_command(void) {
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

static void runtime_example_print_command_descriptors(const json_value_t *summary) {
  const json_value_t *descriptors;
  size_t count;
  size_t i;

  if (!summary) {
    return;
  }
  descriptors = turbo_json_object_get(summary, "available_command_descriptors");
  if (!descriptors) {
    return;
  }
  count = turbo_json_array_size(descriptors);
  if (count == 0) {
    return;
  }
  printf("  available_command_descriptors:");
  for (i = 0; i < count; ++i) {
    const json_value_t *descriptor = turbo_json_array_get(descriptors, i);
    const char *name = descriptor ? turbo_json_get_string(descriptor, "name") : NULL;
    const char *resume_mode =
        descriptor ? turbo_json_get_string(descriptor, "resume_mode") : NULL;

    printf(" %s[%s]", runtime_example_text(name), runtime_example_text(resume_mode));
  }
  printf("\n");
}

static void runtime_example_print_summary(const char *label, const json_value_t *summary) {
  json_value_t *result;
  const json_value_t *parent_agent_run_id;
  const json_value_t *parent_tool_call_id;
  const json_value_t *parent_tool_name;

  if (!summary) {
    return;
  }
  result = turbo_json_object_get(summary, "result");
  parent_agent_run_id = turbo_json_object_get(summary, "parent_agent_run_id");
  parent_tool_call_id = turbo_json_object_get(summary, "parent_tool_call_id");
  parent_tool_name = turbo_json_object_get(summary, "parent_tool_name");
  printf(
      "%s\n"
      "  thread_id: %s\n"
      "  run_id: %s\n"
      "  status: %s\n"
      "  checkpoint_id: %s\n"
      "  parent_agent_run_id: %s\n"
      "  parent_tool_call_id: %s\n"
      "  parent_tool_name: %s\n"
      "  interrupt_reason: %s\n"
      "  pending_node: %s\n"
      "  pending_action: %s\n"
      "  steps: %.0f\n",
      label, runtime_example_text(turbo_json_get_string(summary, "thread_id")),
      runtime_example_text(turbo_json_get_string(summary, "run_id")),
      runtime_example_text(turbo_json_get_string(summary, "status")),
      turbo_json_is_null(turbo_json_object_get(summary, "checkpoint_id"))
          ? "(null)"
          : runtime_example_text(turbo_json_get_string(summary, "checkpoint_id")),
      (!parent_agent_run_id || turbo_json_is_null(parent_agent_run_id))
          ? "(null)"
          : runtime_example_text(turbo_json_get_string(summary, "parent_agent_run_id")),
      (!parent_tool_call_id || turbo_json_is_null(parent_tool_call_id))
          ? "(null)"
          : runtime_example_text(turbo_json_get_string(summary, "parent_tool_call_id")),
      (!parent_tool_name || turbo_json_is_null(parent_tool_name))
          ? "(null)"
          : runtime_example_text(turbo_json_get_string(summary, "parent_tool_name")),
      runtime_example_text(turbo_json_get_string(summary, "interrupt_reason")),
      runtime_example_text(turbo_json_get_string(summary, "pending_node")),
      runtime_example_text(turbo_json_get_string(summary, "pending_action")),
      result ? turbo_json_get_double(result, "steps", 0) : 0);
  runtime_example_print_command_descriptors(summary);
}

int main(void) {
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime = NULL;
  turbo_graph_t *graph = NULL;
  turbo_runtime_data_bind_value_t *state = NULL;
  turbo_runtime_data_bind_value_t *resumed_state = NULL;
  turbo_runtime_data_bind_value_t *command = NULL;
  turbo_runtime_data_bind_value_t *fork_state = NULL;
  json_value_t *summary = NULL;
  json_value_t *resume_summary = NULL;
  json_value_t *fork_summary = NULL;
  json_value_t *checkpoint = NULL;
  turbo_graph_run_options_t options = {0};
  const char *interrupt_before_review[] = {"review"};
  const char *checkpoint_id;

  store = turbo_agent_runtime_store_memory_create();
  runtime = turbo_agent_runtime_create(&store);
  graph = runtime_example_create_review_graph();
  state = runtime_example_initial_state();
  if (!runtime || !graph || !state) {
    fprintf(stderr, "failed to initialize runtime example\n");
    return 1;
  }

  options.interrupt_before_nodes = interrupt_before_review;
  options.interrupt_before_count = 1;
  if (turbo_agent_runtime_exec_start(runtime, graph, state, &options, NULL, &summary, &resumed_state) != 0) {
    fprintf(stderr, "start_bind_graph failed\n");
    return 1;
  }

  runtime_example_print_summary("initial run", summary);
  checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  if (!checkpoint_id) {
    fprintf(stderr, "expected checkpoint id after interrupt\n");
    return 1;
  }

  if (turbo_agent_runtime_get_checkpoint(runtime, checkpoint_id, &checkpoint) != 0) {
    fprintf(stderr, "get_checkpoint failed\n");
    return 1;
  }
  printf("checkpoint control snapshot present: %s\n",
         turbo_json_object_get(checkpoint, "control_snapshot") ? "yes" : "no");

  command = runtime_example_approve_review_command();
  turbo_runtime_data_bind_value_destroy(resumed_state);
  resumed_state = NULL;
  options.interrupt_before_nodes = NULL;
  options.interrupt_before_count = 0;

  if (!command ||
      turbo_agent_runtime_exec_resume(
          runtime, graph, command, &options,
          &(turbo_agent_runtime_exec_options_t){
              .scope = TURBO_RUNTIME_SCOPE_CHECKPOINT,
              .input_kind = TURBO_RUNTIME_INPUT_COMMAND,
              .checkpoint_id = checkpoint_id,
          },
          &resume_summary, &resumed_state) != 0) {
    fprintf(stderr, "resume_command_bind failed\n");
    return 1;
  }
  runtime_example_print_summary("resumed run", resume_summary);

  if (turbo_agent_runtime_exec_fork(
          runtime, graph, command, NULL,
          &(turbo_agent_runtime_exec_options_t){
              .scope = TURBO_RUNTIME_SCOPE_CHECKPOINT,
              .input_kind = TURBO_RUNTIME_INPUT_COMMAND,
              .checkpoint_id = checkpoint_id,
          },
          &fork_summary, &fork_state) != 0) {
    fprintf(stderr, "fork_command_bind failed\n");
    return 1;
  }
  runtime_example_print_summary("forked run", fork_summary);

  turbo_free_json(&fork_summary);
  turbo_free_json(&resume_summary);
  turbo_free_json(&summary);
  turbo_free_json(&checkpoint);
  turbo_runtime_data_bind_value_destroy(fork_state);
  turbo_runtime_data_bind_value_destroy(command);
  turbo_runtime_data_bind_value_destroy(resumed_state);
  turbo_runtime_data_bind_value_destroy(state);
  turbo_graph_destroy(graph);
  turbo_agent_runtime_destroy(runtime);
  return 0;
}
