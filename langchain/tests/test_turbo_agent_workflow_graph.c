#include "tinytest.h"
#include "turbo_agent.h"
#include "turbo_agent_state.h"
#include "turbo_agent_workflow.h"

static int dummy_transport(const char *request_json, char **out_response_json, void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static int dummy_detect_replan(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
  return 0;
}

static turbo_agent_t *create_test_agent(const char *model) {
  turbo_agent_config_t config = {0};

  config.model = model;
  config.transport_fn = dummy_transport;
  return turbo_agent_create(&config);
}

static int supervisor_planner_handoff_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_not_null(ctx);
  check_not_null(ctx->state);
  turbo_json_object_set_bool(ctx->state, "planner_visited", true);
  check_int_eq(turbo_agent_state_request_handoff(ctx->state, "executor", "delegate execution"), 0);
  return turbo_graph_ctx_set_next(ctx, "handoff");
}

static int supervisor_executor_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;

  check_not_null(ctx);
  check_not_null(ctx->state);
  turbo_json_object_set_bool(ctx->state, "executor_visited", true);
  return turbo_graph_ctx_set_next(ctx, "end");
}

spec("turbo agent workflow graph") {

  it("should install planner loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("planner-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_planner_loop(graph, planner, executor, "planner", "commit", "step",
                                         "executor", "tools", "advance", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 7);
    check_size_eq(turbo_graph_edge_count(graph), 8);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install review loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("review-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_review_loop(graph, planner, executor, "planner", "commit", "step",
                                        "review", "executor", "tools", "advance", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 8);
    check_size_eq(turbo_graph_edge_count(graph), 10);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install review replan loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("review-replan-loop");
    turbo_agent_t *planner = create_test_agent("gpt-5.4");
    turbo_agent_t *executor = create_test_agent("gpt-5.4");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    check_int_eq(
        turbo_agent_install_review_replan_loop(
            graph, planner, executor, "planner", "commit", "step", "review", "executor",
            "tools", "detect", dummy_detect_replan, NULL, "route", "prepare", "advance", "end",
            1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "planner");
    check_size_eq(turbo_graph_node_count(graph), 11);
    check_size_eq(turbo_graph_edge_count(graph), 14);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should install supervisor loop graph structure") {
    turbo_graph_t *graph = turbo_graph_create("supervisor-loop");

    check_not_null(graph);
    check_int_eq(
        turbo_agent_install_supervisor_loop(graph, "supervisor", "handoff", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "supervisor");
    check_size_eq(turbo_graph_node_count(graph), 3);
    check_size_eq(turbo_graph_edge_count(graph), 2);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_graph_destroy(graph);
  }

  it("should route one staged handoff through the supervisor loop") {
    turbo_graph_t *graph = turbo_graph_create("supervisor-handoff-loop");
    json_value_t *state = turbo_agent_state_create();
    turbo_runtime_data_bind_value_t *input = NULL;
    turbo_runtime_data_bind_value_t *result_state = NULL;
    json_value_t *result_json = NULL;
    const json_value_t *handoff_history = NULL;
    const json_value_t *handoff_entry = NULL;
    turbo_graph_run_result_t result = {0};

    check_not_null(graph);
    check_not_null(state);
    check_int_eq(
        turbo_agent_install_supervisor_loop(graph, "supervisor", "handoff", "end", 1),
        TURBO_GRAPH_EXEC_OK);
    check_int_eq(
        turbo_graph_add_node(graph, "planner", supervisor_planner_handoff_node, NULL),
        TURBO_GRAPH_EXEC_OK);
    check_int_eq(
        turbo_graph_add_node(graph, "executor", supervisor_executor_node, NULL),
        TURBO_GRAPH_EXEC_OK);
    check_int_eq(turbo_agent_state_set_active_agent(state, "planner"), 0);

    input = turbo_runtime_data_bind_value_from_json(state);
    turbo_free_json(&state);
    check_not_null(input);

    check_int_eq(turbo_graph_run_bind(graph, input, NULL, &result, &result_state),
                 TURBO_GRAPH_EXEC_STOP);
    check_int_eq(result.status, TURBO_GRAPH_EXEC_STOP);
    check_str_eq(result.last_node, "end");
    check_not_null(result_state);

    result_json = turbo_runtime_data_bind_value_to_json(result_state);
    check_not_null(result_json);
    check_true(turbo_json_get_bool(result_json, "planner_visited", false));
    check_true(turbo_json_get_bool(result_json, "executor_visited", false));
    check_str_eq(turbo_agent_state_active_agent(result_json), "executor");
    check_null(turbo_agent_state_handoff_target_agent(result_json));
    check_null(turbo_agent_state_handoff_reason(result_json));
    handoff_history = turbo_agent_state_supervisor_handoff_history(result_json);
    check_not_null(handoff_history);
    check_size_eq(turbo_json_array_size(handoff_history), 1);
    handoff_entry = turbo_json_array_get(handoff_history, 0);
    check_not_null(handoff_entry);
    check_str_eq(turbo_json_get_string(handoff_entry, "from_agent"), "planner");
    check_str_eq(turbo_json_get_string(handoff_entry, "target_agent"), "executor");
    check_str_eq(turbo_json_get_string(handoff_entry, "reason"), "delegate execution");

    turbo_free_json(&result_json);
    turbo_runtime_data_bind_value_destroy(result_state);
    turbo_runtime_data_bind_value_destroy(input);
    turbo_graph_destroy(graph);
  }
}
