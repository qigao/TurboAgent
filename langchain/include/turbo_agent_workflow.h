#ifndef TURBO_AGENT_WORKFLOW_H
#define TURBO_AGENT_WORKFLOW_H

#include <turbo_agent_api.h>

#include "turbo_agent_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install a minimal planner loop into a graph.
 *
 * Topology:
 * `planner_model -> plan_commit -> plan_step -> executor_model`
 * `executor_model -> tools -> executor_model` when tool calls exist
 * `executor_model -> plan_advance -> end | plan_step`
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param plan_step_node_name Step prompt node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_planner_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry);

/**
 * @brief Install a minimal `model -> tools -> model -> end` loop into a graph.
 * @param graph Graph handle.
 * @param agent Agent handle.
 * @param model_node_name Node name for model step.
 * @param tool_node_name Node name for tool dispatch.
 * @param end_node_name Node name for stop node.
 * @param set_entry Whether to set model_node_name as graph entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_loop(turbo_graph_t *graph, turbo_agent_t *agent,
                                const char *model_node_name, const char *tool_node_name,
                                const char *end_node_name, int set_entry);

/**
 * @brief Install a planner loop with a human review gate before execution.
 *
 * Topology:
 * `planner_model -> plan_commit -> plan_step -> review -> executor_model | end`
 * `executor_model -> tools -> executor_model` when tool calls exist
 * `executor_model -> plan_advance -> end | plan_step`
 *
 * The intended pattern is to interrupt before `review_node_name`, inspect state,
 * then set review approval and resume.
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param plan_step_node_name Step prompt node name.
 * @param review_node_name Review gate node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_review_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry);

/**
 * @brief Install a planner loop that can request replanning after a failed step.
 *
 * Topology:
 * `planner_model -> plan_commit -> plan_step -> executor_model`
 * `executor_model -> tools -> executor_model` when tool calls exist
 * `executor_model -> detect_replan -> replan_route`
 * `replan_route -> replan_prepare -> planner_model` when replanning is requested
 * `replan_route -> plan_advance -> end | plan_step` otherwise
 *
 * `detect_replan_node_fn(...)` should inspect state and optionally call
 * `turbo_agent_state_request_replan(...)`. It does not need to route.
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param plan_step_node_name Step prompt node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param detect_replan_node_name Failure detection node name.
 * @param detect_replan_node_fn Failure detection callback.
 * @param detect_replan_user_data Failure detection callback user data.
 * @param replan_route_node_name Replan route node name.
 * @param replan_prepare_node_name Replan reset node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_replan_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_replan_node_name, turbo_graph_node_fn detect_replan_node_fn,
    void *detect_replan_user_data, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry);

/**
 * @brief Install a planner loop with both a human review gate and replanning.
 *
 * Topology:
 * `planner_model -> plan_commit -> plan_step -> review -> executor_model | end`
 * `executor_model -> tools -> executor_model` when tool calls exist
 * `executor_model -> detect_replan -> replan_route`
 * `replan_route -> replan_prepare -> planner_model` when replanning is requested
 * `replan_route -> plan_advance -> end | plan_step` otherwise
 *
 * The intended pattern is to interrupt before `review_node_name`, inspect state,
 * then set review approval and resume.
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param plan_step_node_name Step prompt node name.
 * @param review_node_name Review gate node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param detect_replan_node_name Failure detection node name.
 * @param detect_replan_node_fn Failure detection callback.
 * @param detect_replan_user_data Failure detection callback user data.
 * @param replan_route_node_name Replan route node name.
 * @param replan_prepare_node_name Replan reset node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_review_replan_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_replan_node_name,
    turbo_graph_node_fn detect_replan_node_fn, void *detect_replan_user_data,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry);

/**
 * @brief Install the default engineering loop with review, tools, and replanning.
 *
 * This is a thin convenience wrapper over
 * `turbo_agent_install_review_replan_loop(...)` using the builtin
 * failure detector `turbo_agent_detect_failed_step_node(...)`.
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param plan_step_node_name Step prompt node name.
 * @param review_node_name Review gate node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param detect_failure_node_name Failure detection node name.
 * @param replan_route_node_name Replan route node name.
 * @param replan_prepare_node_name Replan reset node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_engineering_loop(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry);

/**
 * @brief Install the engineering loop with review re-armed before each plan step.
 *
 * This preserves the original engineering loop behavior for existing callers,
 * while providing an explicit per-step review mode for workflow UIs and CLIs.
 *
 * @param graph Graph handle.
 * @param planner_agent Agent used for planning turns.
 * @param executor_agent Agent used for execution turns.
 * @param planner_node_name Planner model node name.
 * @param plan_commit_node_name Plan commit node name.
 * @param review_reset_node_name Review re-arm node name.
 * @param plan_step_node_name Step prompt node name.
 * @param review_node_name Review gate node name.
 * @param executor_node_name Executor model node name.
 * @param tool_node_name Tool dispatch node name.
 * @param detect_failure_node_name Failure detection node name.
 * @param replan_route_node_name Replan route node name.
 * @param replan_prepare_node_name Replan reset node name.
 * @param plan_advance_node_name Plan advance node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set planner_node_name as entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_engineering_loop_each_step_review(
    turbo_graph_t *graph, turbo_agent_t *planner_agent,
    turbo_agent_t *executor_agent, const char *planner_node_name,
    const char *plan_commit_node_name, const char *review_reset_node_name,
    const char *plan_step_node_name, const char *review_node_name,
    const char *executor_node_name, const char *tool_node_name,
    const char *detect_failure_node_name, const char *replan_route_node_name,
    const char *replan_prepare_node_name, const char *plan_advance_node_name,
    const char *end_node_name, int set_entry);

/**
 * @brief Install a minimal supervisor + handoff routing scaffold.
 *
 * Topology:
 * `supervisor_route -> end` by default
 * `handoff_route -> end` by default
 *
 * The installed route nodes use `state.supervisor_versions` as the single
 * fact source. `supervisor_route` jumps to `active_agent` when set. Later,
 * `handoff_route` commits any staged handoff target and jumps to the new
 * active agent. Callers add the actual agent nodes separately and wire them
 * back into `handoff_route` or `end` as needed.
 *
 * @param graph Graph handle.
 * @param supervisor_node_name Supervisor route node name.
 * @param handoff_node_name Handoff commit/route node name.
 * @param end_node_name Stop node name.
 * @param set_entry Whether to set supervisor_node_name as graph entry.
 * @return Graph status code.
 */
CXX_C_API turbo_graph_exec_status_t
turbo_agent_install_supervisor_loop(turbo_graph_t *graph,
                                    const char *supervisor_node_name,
                                    const char *handoff_node_name,
                                    const char *end_node_name, int set_entry);

#ifdef __cplusplus
}
#endif

#endif


