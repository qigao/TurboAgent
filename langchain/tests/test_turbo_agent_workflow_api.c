#include "tinytest.h"
#include "turbo_agent_workflow.h"
#include "turbo_agent_state.h"

static int workflow_planner_request_handoff_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_not_null(ctx);
  check_not_null(ctx->state);
  check_str_eq(turbo_agent_state_active_agent(ctx->state), "planner");
  check_null(turbo_agent_state_handoff_target_agent(ctx->state));
  check_null(turbo_agent_state_handoff_reason(ctx->state));
  turbo_json_object_set_bool(ctx->state, "planner_requested_handoff", 1);
  return turbo_agent_state_request_handoff(ctx->state, "executor", "delegate execution");
}

static int workflow_executor_complete_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const json_value_t *history;

  (void)user_data;

  check_not_null(ctx);
  check_not_null(ctx->state);
  history = turbo_agent_state_supervisor_handoff_history(ctx->state);
  check_str_eq(turbo_agent_state_active_agent(ctx->state), "executor");
  check_null(turbo_agent_state_handoff_target_agent(ctx->state));
  check_null(turbo_agent_state_handoff_reason(ctx->state));
  check_not_null(history);
  check_size_eq(turbo_json_array_size(history), 1);
  turbo_json_object_set_bool(ctx->state, "executor_completed", 1);
  return 0;
}

spec("turbo agent workflow api") {

  it("should expose workflow installers through the workflow header") {
    void *planner_loop = (void *)turbo_agent_install_planner_loop;
    void *basic_loop = (void *)turbo_agent_install_loop;
    void *review_loop = (void *)turbo_agent_install_review_loop;
    void *replan_loop = (void *)turbo_agent_install_replan_loop;
    void *review_replan_loop = (void *)turbo_agent_install_review_replan_loop;
    void *engineering_loop = (void *)turbo_agent_install_engineering_loop;
    void *engineering_each_step_review =
        (void *)turbo_agent_install_engineering_loop_each_step_review;
    void *supervisor_loop = (void *)turbo_agent_install_supervisor_loop;

    check_not_null(planner_loop);
    check_not_null(basic_loop);
    check_not_null(review_loop);
    check_not_null(replan_loop);
    check_not_null(review_replan_loop);
    check_not_null(engineering_loop);
    check_not_null(engineering_each_step_review);
    check_not_null(supervisor_loop);
  }

  it("should execute a minimal supervisor handoff loop through the installed route nodes") {
    turbo_graph_t *graph = turbo_graph_create("workflow-supervisor-handoff");
    json_value_t *state = turbo_agent_state_create();
    json_value_t *control = NULL;
    json_value_t *workflow = NULL;
    const json_value_t *supervisor = NULL;
    const json_value_t *history = NULL;
    const json_value_t *history_entry = NULL;
    turbo_graph_run_result_t result = {0};

    check_not_null(graph);
    check_not_null(state);
    check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);
    check_int_eq(
        turbo_agent_install_supervisor_loop(graph, "supervisor_route", "handoff_route", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_graph_add_node(graph, "planner", workflow_planner_request_handoff_node,
                                      NULL),
                 TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_graph_add_node(graph, "executor", workflow_executor_complete_node, NULL),
                 TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_graph_add_edge(graph, "planner", "handoff_route", NULL, NULL),
                 TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_graph_add_edge(graph, "executor", "end", NULL, NULL), TURBO_GRAPH_EXEC_OK);

    check_int_eq(turbo_graph_run(graph, state, NULL, &result), TURBO_GRAPH_EXEC_STOP);
    check_int_eq(result.status, TURBO_GRAPH_EXEC_STOP);
    check_str_eq(result.last_node, "end");
    check_null(result.next_node);

    check_true(turbo_json_get_bool(state, "planner_requested_handoff", false));
    check_true(turbo_json_get_bool(state, "executor_completed", false));
    check_str_eq(turbo_agent_state_active_agent(state), "executor");
    check_null(turbo_agent_state_handoff_target_agent(state));
    check_null(turbo_agent_state_handoff_reason(state));

    history = turbo_agent_state_supervisor_handoff_history(state);
    check_not_null(history);
    check_size_eq(turbo_json_array_size(history), 1);
    history_entry = turbo_json_array_get(history, 0);
    check_str_eq(turbo_json_get_string(history_entry, "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(history_entry, "target_agent"), "executor");
    check_str_eq(turbo_json_get_string(history_entry, "reason"), "delegate execution");

    control = turbo_agent_state_control_snapshot(state);
    workflow = turbo_agent_state_workflow_snapshot(state);
    check_not_null(control);
    check_not_null(workflow);

    supervisor = turbo_json_object_get(control, "supervisor");
    check_not_null(supervisor);
    check_str_eq(turbo_json_get_string(supervisor, "active_agent"), "executor");
    check_str_eq(turbo_json_get_string(supervisor, "target_agent"), "");
    check_str_eq(turbo_json_get_string(supervisor, "handoff_reason"), "");
    check_int_eq(turbo_json_get_int(supervisor, "handoff_count", 0), 1);

    supervisor = turbo_json_object_get(workflow, "supervisor");
    check_not_null(supervisor);
    check_str_eq(turbo_json_get_string(supervisor, "active_agent"), "executor");
    check_str_eq(turbo_json_get_string(supervisor, "target_agent"), "");
    check_str_eq(turbo_json_get_string(supervisor, "handoff_reason"), "");
    history = turbo_json_object_get(supervisor, "handoff_history");
    check_not_null(history);
    check_size_eq(turbo_json_array_size(history), 1);
    history_entry = turbo_json_array_get(history, 0);
    check_str_eq(turbo_json_get_string(history_entry, "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(history_entry, "target_agent"), "executor");
    check_str_eq(turbo_json_get_string(history_entry, "reason"), "delegate execution");

    turbo_free_json(&workflow);
    turbo_free_json(&control);
    turbo_free_json(&state);
    turbo_graph_destroy(graph);
  }
}


