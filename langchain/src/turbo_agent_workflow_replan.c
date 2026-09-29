#include "turbo_agent_workflow_graph_internal.h"

turbo_graph_exec_status_t
turbo_agent_install_engineering_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  return turbo_agent_install_review_replan_loop(
      graph, planner_agent, executor_agent, planner_node_name, plan_commit_node_name,
      plan_step_node_name, review_node_name, executor_node_name, tool_node_name,
      detect_failure_node_name, turbo_agent_detect_failed_step_node, NULL,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name, end_node_name,
      set_entry);
}

turbo_graph_exec_status_t
turbo_agent_install_engineering_loop_each_step_review(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *review_reset_node_name,
    const char *plan_step_node_name, const char *review_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_failure_node_name, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !review_reset_node_name || !plan_step_node_name ||
      !review_node_name || !executor_node_name || !tool_node_name ||
      !detect_failure_node_name || !replan_route_node_name || !replan_prepare_node_name ||
      !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_agent_workflow_add_planner_core(graph, planner_node_name, planner_agent,
                                                 plan_commit_node_name, plan_step_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, review_reset_node_name,
                                         turbo_agent_review_reset_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, review_node_name, turbo_agent_review_node,
                                         executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_executor_core(graph, executor_node_name, tool_node_name,
                                                  executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_replan_core(
      graph, detect_failure_node_name, turbo_agent_detect_failed_step_node, NULL,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_end_node(graph, end_node_name, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, planner_node_name, plan_commit_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, plan_commit_node_name, review_reset_node_name,
                                         NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, review_reset_node_name, plan_step_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, plan_step_node_name, review_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_review_gate(graph, review_node_name, executor_node_name,
                                                    end_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_executor_replan_cycle(
      graph, executor_node_name, tool_node_name, detect_failure_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, detect_failure_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_replan_cycle(
      graph, detect_failure_node_name, replan_route_node_name, replan_prepare_node_name,
      planner_node_name, plan_advance_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_plan_advance(graph, plan_advance_node_name, end_node_name,
                                                     review_reset_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return turbo_agent_workflow_set_entry_if_requested(graph, planner_node_name, set_entry);
}

turbo_graph_exec_status_t
turbo_agent_install_replan_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_replan_node_name, turbo_graph_node_fn detect_replan_node_fn,
    void *detect_replan_user_data, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !executor_node_name || !tool_node_name ||
      !detect_replan_node_name || !detect_replan_node_fn || !replan_route_node_name ||
      !replan_prepare_node_name || !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_agent_workflow_add_planner_core(graph, planner_node_name, planner_agent,
                                                 plan_commit_node_name, plan_step_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_executor_core(graph, executor_node_name, tool_node_name,
                                                  executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_replan_core(
      graph, detect_replan_node_name, detect_replan_node_fn, detect_replan_user_data,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_end_node(graph, end_node_name, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_planner_path(graph, planner_node_name,
                                                     plan_commit_node_name, plan_step_node_name,
                                                     executor_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_executor_replan_cycle(
      graph, executor_node_name, tool_node_name, detect_replan_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, detect_replan_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_replan_cycle(
      graph, detect_replan_node_name, replan_route_node_name, replan_prepare_node_name,
      planner_node_name, plan_advance_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_plan_advance(graph, plan_advance_node_name, end_node_name,
                                                     plan_step_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return turbo_agent_workflow_set_entry_if_requested(graph, planner_node_name, set_entry);
}

turbo_graph_exec_status_t
turbo_agent_install_review_replan_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_replan_node_name,
    turbo_graph_node_fn detect_replan_node_fn, void *detect_replan_user_data,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !review_node_name ||
      !executor_node_name || !tool_node_name || !detect_replan_node_name ||
      !detect_replan_node_fn || !replan_route_node_name || !replan_prepare_node_name ||
      !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_agent_workflow_add_planner_core(graph, planner_node_name, planner_agent,
                                                 plan_commit_node_name, plan_step_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, review_node_name, turbo_agent_review_node,
                                         executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_executor_core(graph, executor_node_name, tool_node_name,
                                                  executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_replan_core(
      graph, detect_replan_node_name, detect_replan_node_fn, detect_replan_user_data,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_end_node(graph, end_node_name, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_planner_path(graph, planner_node_name,
                                                     plan_commit_node_name, plan_step_node_name,
                                                     review_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_review_gate(graph, review_node_name, executor_node_name,
                                                    end_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_executor_replan_cycle(
      graph, executor_node_name, tool_node_name, detect_replan_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, detect_replan_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_replan_cycle(
      graph, detect_replan_node_name, replan_route_node_name, replan_prepare_node_name,
      planner_node_name, plan_advance_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_connect_plan_advance(graph, plan_advance_node_name, end_node_name,
                                                     plan_step_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return turbo_agent_workflow_set_entry_if_requested(graph, planner_node_name, set_entry);
}
