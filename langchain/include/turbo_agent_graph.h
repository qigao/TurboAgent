#ifndef TURBO_AGENT_GRAPH_H
#define TURBO_AGENT_GRAPH_H

#include <turbo_agent_api.h>

#include "turbo_graph.h"
#include "turbo_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Graph node that performs one Responses API model call.
 * @param ctx Graph execution context.
 * @param user_data `turbo_agent_t*`
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that runs a planner turn, optionally against replan-specific input.
 * @param ctx Graph execution context.
 * @param user_data `turbo_agent_t*`
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_planner_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that executes pending tool calls and appends outputs.
 * @param ctx Graph execution context.
 * @param user_data `turbo_agent_t*`
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that parses the last assistant text as planner JSON and stores `state.plan`.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success, non-zero on parse failure.
 */
CXX_C_API int turbo_agent_plan_commit_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that prepares executor input for the current plan step.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_plan_step_prepare_node(turbo_graph_exec_ctx_t *ctx,
                                                        void *user_data);

/**
 * @brief Graph node that runs the executor model against the current step sub-history.
 * @param ctx Graph execution context.
 * @param user_data `turbo_agent_t*`
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_executor_model_node(turbo_graph_exec_ctx_t *ctx,
                                                     void *user_data);

/**
 * @brief Graph node that executes pending tools inside the current step sub-history.
 * @param ctx Graph execution context.
 * @param user_data `turbo_agent_t*`
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_executor_tool_node(turbo_graph_exec_ctx_t *ctx,
                                                    void *user_data);

/**
 * @brief Graph node that appends a replan prompt back into planner input.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_replan_prepare_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that advances the current plan by one completed step.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_plan_advance_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node representing a human review gate.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_review_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Graph node that re-arms an existing review gate for the next step.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_review_reset_node(turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Edge predicate: true if the last model event contains tool calls.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 1 if tool calls exist, else 0.
 */
CXX_C_API int turbo_agent_has_pending_tool_calls(const turbo_graph_exec_ctx_t *ctx,
                                                        void *user_data);

/**
 * @brief Edge predicate: true if replanning has been requested in state.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 1 if replanning is requested, else 0.
 */
CXX_C_API int turbo_agent_should_replan(const turbo_graph_exec_ctx_t *ctx, void *user_data);

/**
 * @brief Route to either replanning or normal plan advance.
 * @param ctx Graph execution context.
 * @param user_data Replan target node name. If no replan is requested, normal
 *        graph routing continues through outgoing edges.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_replan_route_node(turbo_graph_exec_ctx_t *ctx,
                                                   void *user_data);

/**
 * @brief Edge predicate: true if the current review gate is approved.
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 1 if approved, else 0.
 */
CXX_C_API int turbo_agent_review_approved_predicate(const turbo_graph_exec_ctx_t *ctx,
                                                           void *user_data);

/**
 * @brief Default engineering-step failure detector.
 *
 * Inspects the latest executor model output and requests replanning when it
 * contains obvious failure markers such as `FAILED:`, `error`, or `unable`.
 *
 * @param ctx Graph execution context.
 * @param user_data Unused.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_detect_failed_step_node(turbo_graph_exec_ctx_t *ctx,
                                                         void *user_data);

#ifdef __cplusplus
}
#endif

#endif


