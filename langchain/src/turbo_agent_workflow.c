#include "turbo_agent_workflow_graph_internal.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"

#include <turbo_str.h>

static const char turbo_agent_planner_tool_node_suffix[] = ".tools";

static int turbo_agent_nested_has_pending_tool_calls(const turbo_graph_exec_ctx_t *ctx,
                                                     void *user_data) {
  const char *state_versions_key = (const char *)user_data;
  const json_value_t *nested_state;

  if (!ctx || !ctx->state || !state_versions_key) {
    return 0;
  }

  nested_state =
      turbo_agent_state_get_current_object_version_const(ctx->state, state_versions_key);
  return turbo_agent_state_pending_tool_calls(nested_state ? nested_state : ctx->state) > 0;
}

static tstr_t turbo_agent_planner_tool_node_name_create(const char *planner_node_name) {
  tstr_t name;
  tstr_t expanded;

  if (!planner_node_name) {
    return NULL;
  }
  name = tstr_dup(planner_node_name);
  if (!name) {
    return NULL;
  }
  expanded = tstr_cat(name, turbo_agent_planner_tool_node_suffix);
  if (!expanded) {
    tstr_free(name);
    return NULL;
  }
  return expanded;
}

int turbo_agent_workflow_plan_complete_predicate(const turbo_graph_exec_ctx_t *ctx,
                                                 void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_agent_state_plan_complete(ctx->state) : 0;
}

int turbo_agent_workflow_end_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  turbo_graph_ctx_stop(ctx);
  return 0;
}

turbo_graph_exec_status_t turbo_agent_workflow_add_end_node(
    turbo_graph_t *graph, const char *end_node_name, void *user_data) {
  if (!graph || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  return turbo_graph_add_node(graph, end_node_name, turbo_agent_workflow_end_node, user_data);
}

turbo_graph_exec_status_t turbo_agent_workflow_add_node(
    turbo_graph_t *graph, const char *node_name, turbo_graph_node_fn node_fn,
    void *user_data) {
  if (!graph || !node_name || !node_fn) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  return turbo_graph_add_node(graph, node_name, node_fn, user_data);
}

turbo_graph_exec_status_t turbo_agent_workflow_add_edge(
    turbo_graph_t *graph, const char *from_node_name, const char *to_node_name,
    turbo_graph_edge_predicate_fn predicate, void *user_data) {
  if (!graph || !from_node_name || !to_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  return turbo_graph_add_edge(graph, from_node_name, to_node_name, predicate, user_data);
}

turbo_graph_exec_status_t turbo_agent_workflow_add_planner_core(
    turbo_graph_t *graph, const char *planner_node_name, turbo_agent_t *planner_agent,
    const char *plan_commit_node_name, const char *plan_step_node_name) {
  turbo_graph_exec_status_t status;
  tstr_t planner_tool_node_name;

  planner_tool_node_name = turbo_agent_planner_tool_node_name_create(planner_node_name);
  if (!planner_tool_node_name) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  status = turbo_agent_workflow_add_node(graph, planner_node_name,
                                         turbo_agent_planner_model_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    tstr_free(planner_tool_node_name);
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, planner_tool_node_name,
                                         turbo_agent_planner_tool_node, planner_agent);
  tstr_free(planner_tool_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, plan_commit_node_name,
                                         turbo_agent_plan_commit_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, plan_step_node_name,
                                         turbo_agent_plan_step_prepare_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_add_executor_core(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name,
    turbo_agent_t *executor_agent) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_node(graph, executor_node_name,
                                         turbo_agent_executor_model_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, tool_node_name,
                                         turbo_agent_executor_tool_node, executor_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_add_replan_core(
    turbo_graph_t *graph, const char *detect_replan_node_name,
    turbo_graph_node_fn detect_replan_node_fn, void *detect_replan_user_data,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, turbo_agent_t *planner_agent) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_node(graph, detect_replan_node_name, detect_replan_node_fn,
                                         detect_replan_user_data);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, replan_route_node_name,
                                         turbo_agent_replan_route_node,
                                         (void *)replan_prepare_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, replan_prepare_node_name,
                                         turbo_agent_replan_prepare_node, planner_agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, plan_advance_node_name,
                                         turbo_agent_plan_advance_node, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_executor_cycle(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, tool_node_name,
                                         turbo_agent_nested_has_pending_tool_calls,
                                         (void *)"executor_state_versions");
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_executor_replan_cycle(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name,
    const char *detect_replan_node_name) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_edge(graph, executor_node_name, tool_node_name,
                                         turbo_agent_nested_has_pending_tool_calls,
                                         (void *)"executor_state_versions");
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, tool_node_name, detect_replan_node_name,
                                         turbo_agent_should_replan, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, tool_node_name, executor_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_replan_cycle(
    turbo_graph_t *graph, const char *detect_replan_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *planner_node_name, const char *plan_advance_node_name) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_edge(graph, detect_replan_node_name, replan_route_node_name,
                                         NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, replan_route_node_name, plan_advance_node_name,
                                         NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, replan_prepare_node_name, planner_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_planner_path(
    turbo_graph_t *graph, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *next_node_name) {
  turbo_graph_exec_status_t status;
  tstr_t planner_tool_node_name;

  planner_tool_node_name = turbo_agent_planner_tool_node_name_create(planner_node_name);
  if (!planner_tool_node_name) {
    return TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
  }

  status = turbo_agent_workflow_add_edge(graph, planner_node_name, planner_tool_node_name,
                                         turbo_agent_nested_has_pending_tool_calls,
                                         (void *)"planner_state_versions");
  if (status != TURBO_GRAPH_EXEC_OK) {
    tstr_free(planner_tool_node_name);
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, planner_tool_node_name, planner_node_name, NULL,
                                         NULL);
  tstr_free(planner_tool_node_name);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, planner_node_name, plan_commit_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, plan_commit_node_name, plan_step_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, plan_step_node_name, next_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_review_gate(
    turbo_graph_t *graph, const char *review_node_name,
    const char *approved_node_name, const char *end_node_name) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_edge(graph, review_node_name, approved_node_name,
                                         turbo_agent_review_gate_open_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, review_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_connect_plan_advance(
    turbo_graph_t *graph, const char *plan_advance_node_name, const char *end_node_name,
    const char *next_step_node_name) {
  turbo_graph_exec_status_t status;

  status = turbo_agent_workflow_add_edge(graph, plan_advance_node_name, end_node_name,
                                         turbo_agent_workflow_plan_complete_predicate, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, plan_advance_node_name, next_step_node_name, NULL,
                                         NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return TURBO_GRAPH_EXEC_OK;
}

turbo_graph_exec_status_t turbo_agent_workflow_set_entry_if_requested(
    turbo_graph_t *graph, const char *entry_node_name, int set_entry) {
  if (!set_entry) {
    return TURBO_GRAPH_EXEC_OK;
  }
  if (!graph || !entry_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  return turbo_graph_set_entry(graph, entry_node_name);
}

turbo_graph_exec_status_t
turbo_agent_install_loop(turbo_graph_t *graph, turbo_agent_t *agent,
                         const char *model_node_name, const char *tool_node_name,
                         const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !agent || !model_node_name || !tool_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_agent_workflow_add_node(graph, model_node_name, turbo_agent_model_node, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_node(graph, tool_node_name, turbo_agent_tool_node, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_end_node(graph, end_node_name, agent);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, model_node_name, tool_node_name,
                                         turbo_agent_has_pending_tool_calls, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, model_node_name, end_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_workflow_add_edge(graph, tool_node_name, model_node_name, NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return turbo_agent_workflow_set_entry_if_requested(graph, model_node_name, set_entry);
}
