#ifndef TURBO_AGENT_WORKFLOW_GRAPH_INTERNAL_H
#define TURBO_AGENT_WORKFLOW_GRAPH_INTERNAL_H

#include "turbo_agent_workflow.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_agent_workflow_plan_complete_predicate(
    const turbo_graph_exec_ctx_t *ctx, void *user_data);
CXX_C_API int turbo_agent_workflow_end_node(turbo_graph_exec_ctx_t *ctx,
                                            void *user_data);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_end_node(
    turbo_graph_t *graph, const char *end_node_name, void *user_data);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_set_entry_if_requested(
    turbo_graph_t *graph, const char *entry_node_name, int set_entry);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_node(
    turbo_graph_t *graph, const char *node_name, turbo_graph_node_fn node_fn,
    void *user_data);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_edge(
    turbo_graph_t *graph, const char *from_node_name, const char *to_node_name,
    turbo_graph_edge_predicate_fn predicate, void *user_data);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_planner_core(
    turbo_graph_t *graph, const char *planner_node_name, turbo_agent_t *planner_agent,
    const char *plan_commit_node_name, const char *plan_step_node_name);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_executor_core(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name,
    turbo_agent_t *executor_agent);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_add_replan_core(
    turbo_graph_t *graph, const char *detect_replan_node_name,
    turbo_graph_node_fn detect_replan_node_fn, void *detect_replan_user_data,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, turbo_agent_t *planner_agent);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_executor_cycle(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_executor_replan_cycle(
    turbo_graph_t *graph, const char *executor_node_name, const char *tool_node_name,
    const char *detect_replan_node_name);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_replan_cycle(
    turbo_graph_t *graph, const char *detect_replan_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *planner_node_name, const char *plan_advance_node_name);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_planner_path(
    turbo_graph_t *graph, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *next_node_name);
CXX_C_API int turbo_agent_review_gate_open_predicate(const turbo_graph_exec_ctx_t *ctx,
                                                     void *user_data);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_review_gate(
    turbo_graph_t *graph, const char *review_node_name,
    const char *approved_node_name, const char *end_node_name);
CXX_C_API turbo_graph_exec_status_t turbo_agent_workflow_connect_plan_advance(
    turbo_graph_t *graph, const char *plan_advance_node_name, const char *end_node_name,
    const char *next_step_node_name);

#ifdef __cplusplus
}
#endif

#endif
