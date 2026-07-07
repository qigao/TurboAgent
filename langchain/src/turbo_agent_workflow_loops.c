#include "turbo_agent_workflow_graph_internal.h"

static int turbo_agent_supervisor_route_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *end_node_name = (const char *)user_data;
  const char *active_agent;

  if (!ctx || !ctx->state || !end_node_name) {
    return -1;
  }
  active_agent = turbo_agent_state_active_agent(ctx->state);
  if (active_agent && active_agent[0] != '\0') {
    return turbo_graph_ctx_set_next(ctx, active_agent);
  }
  return turbo_graph_ctx_set_next(ctx, end_node_name);
}

static int turbo_agent_supervisor_handoff_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *end_node_name = (const char *)user_data;
  const char *target_agent;
  const char *active_agent;

  if (!ctx || !ctx->state || !end_node_name) {
    return -1;
  }
  target_agent = turbo_agent_state_handoff_target_agent(ctx->state);
  if (!target_agent || target_agent[0] == '\0') {
    return turbo_graph_ctx_set_next(ctx, end_node_name);
  }
  if (turbo_agent_state_commit_handoff(ctx->state) != 0) {
    return -1;
  }
  active_agent = turbo_agent_state_active_agent(ctx->state);
  if (!active_agent || active_agent[0] == '\0') {
    return turbo_graph_ctx_set_next(ctx, end_node_name);
  }
  return turbo_graph_ctx_set_next(ctx, active_agent);
}

turbo_graph_exec_status_t
turbo_agent_install_supervisor_loop(turbo_graph_t *graph,
                                    const char *supervisor_node_name,
                                    const char *handoff_node_name,
                                    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !supervisor_node_name || !handoff_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_agent_workflow_add_node(graph, supervisor_node_name,
                                         turbo_agent_supervisor_route_node,
                                         (void *)end_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, handoff_node_name,
                                         turbo_agent_supervisor_handoff_node,
                                         (void *)end_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_end_node(graph, end_node_name, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, supervisor_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, handoff_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return turbo_agent_workflow_set_entry_if_requested(graph, supervisor_node_name, set_entry);
}

turbo_graph_exec_status_t
turbo_agent_install_planner_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !executor_node_name || !tool_node_name ||
      !plan_advance_node_name || !end_node_name) {
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

  status = turbo_agent_workflow_add_node(graph, plan_advance_node_name,
                                         turbo_agent_plan_advance_node, NULL);
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

  status = turbo_agent_workflow_connect_executor_cycle(graph, executor_node_name, tool_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, plan_advance_node_name, NULL,
                                         NULL);
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
turbo_agent_install_review_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !planner_agent || !executor_agent || !planner_node_name ||
      !plan_commit_node_name || !plan_step_node_name || !review_node_name ||
      !executor_node_name || !tool_node_name || !plan_advance_node_name || !end_node_name) {
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

  status = turbo_agent_workflow_add_node(graph, plan_advance_node_name,
                                         turbo_agent_plan_advance_node, NULL);
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

  status = turbo_agent_workflow_connect_executor_cycle(graph, executor_node_name, tool_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, plan_advance_node_name, NULL,
                                         NULL);
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
