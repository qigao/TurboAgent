#include "turbo_agent_graph.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_snapshot_internal.h"
#include "turbo_agent_state_flow_internal.h"
#include "turbo_agent_state_output_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_prompt.h"

#include <stdio.h>
#include <stdlib.h>

int turbo_agent_replan_prepare_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  const char *step_text;
  const char *reason;
  const char *latest_executor_text;
  char *completed_steps_message = NULL;
  size_t replan_count;
  size_t replan_limit;
  int needed;
  char *prompt;
  json_value_t *planner_input;
  json_value_t *planner_events;
  json_value_t *planner_history;
  json_value_t *planner_state;
  const json_value_t *base_input;
  json_value_t *message;

  if (!ctx || !ctx->state || !turbo_agent_state_replan_requested(ctx->state)) {
    return -1;
  }

  step_text = turbo_agent_state_current_plan_step_text(ctx->state);
  reason = turbo_agent_state_replan_reason(ctx->state);
  latest_executor_text = turbo_agent_state_latest_executor_output_text(ctx->state);
  completed_steps_message = turbo_agent_build_completed_steps_message(ctx->state);
  replan_count = turbo_agent_state_replan_count(ctx->state);
  replan_limit = turbo_agent_state_replan_limit(ctx->state);
  turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_REPLAN_REQUESTED, "replan", reason,
                         latest_executor_text, (int)replan_count);
  needed = snprintf(
      NULL, 0,
      "Replan from the current failure. Failed step: %s. Failure reason: %s. "
      "Latest executor output: %s. %sReturn only JSON with a steps array. "
      "Avoid repeating the same failed attempt.",
      step_text && step_text[0] != '\0' ? step_text : "unknown",
      reason && reason[0] != '\0' ? reason : "unspecified",
      latest_executor_text && latest_executor_text[0] != '\0' ? latest_executor_text : "none",
      completed_steps_message ? completed_steps_message : "");
  if (needed < 0) {
    tstr_free(completed_steps_message);
    return -1;
  }

  prompt = (char *)malloc((size_t)needed + 1);
  if (!prompt) {
    tstr_free(completed_steps_message);
    return -1;
  }

  snprintf(prompt, (size_t)needed + 1,
           "Replan from the current failure. Failed step: %s. Failure reason: %s. "
           "Latest executor output: %s. %sReturn only JSON with a steps array. "
           "Avoid repeating the same failed attempt.",
           step_text && step_text[0] != '\0' ? step_text : "unknown",
           reason && reason[0] != '\0' ? reason : "unspecified",
           latest_executor_text && latest_executor_text[0] != '\0' ? latest_executor_text : "none",
           completed_steps_message ? completed_steps_message : "");
  tstr_free(completed_steps_message);
  base_input = turbo_json_object_get(ctx->state, "input");
  if (!base_input || turbo_json_type(base_input) != TURBO_JSON_ARRAY) {
    free(prompt);
    return -1;
  }

  planner_input = NULL;
  if (turbo_agent_clone_json(base_input, &planner_input) != TURBO_GRAPH_EXEC_OK) {
    free(prompt);
    return -1;
  }

  planner_events = turbo_json_create_array();
  planner_history = turbo_json_create_array();
  planner_state = NULL;
  message = turbo_prompt_message_create("user", prompt);
  if (!message || !planner_events || !planner_history) {
    free(prompt);
    turbo_free_json(&planner_input);
    turbo_free_json(&message);
    turbo_free_json(&planner_events);
    turbo_free_json(&planner_history);
    return -1;
  }

  free(prompt);
  turbo_json_array_add(planner_input, message);
  if (turbo_agent_state_append_array_version(ctx->state, "planner_event_versions",
                                             planner_history) != 0) {
    turbo_free_json(&planner_input);
    turbo_free_json(&planner_events);
    turbo_free_json(&planner_history);
    return -1;
  }

  planner_state = turbo_agent_substate_create(planner_input, planner_events);
  if (!planner_state ||
      turbo_agent_state_append_object_version(ctx->state, "planner_state_versions",
                                              planner_state) != 0) {
    turbo_free_json(&planner_input);
    turbo_free_json(&planner_events);
    return -1;
  }

  return turbo_agent_state_append_replan_version(ctx->state, 0, replan_count, replan_limit, "");
}

int turbo_agent_has_pending_tool_calls(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return ctx && ctx->state && turbo_agent_state_pending_tool_calls(ctx->state) > 0;
}

int turbo_agent_should_replan(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_agent_state_replan_requested(ctx->state) : 0;
}

int turbo_agent_replan_route_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *replan_node_name = (const char *)user_data;

  if (!ctx || !ctx->state || !replan_node_name) {
    return -1;
  }

  if (!turbo_agent_state_replan_requested(ctx->state)) {
    return 0;
  }

  return turbo_graph_ctx_set_next(ctx, replan_node_name);
}

int turbo_agent_review_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  if (turbo_agent_state_review_required(ctx->state) &&
      !turbo_agent_state_review_approved(ctx->state)) {
    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_REVIEW_REQUIRED, "review",
                           turbo_agent_state_review_note(ctx->state), NULL, 0);
    turbo_graph_ctx_stop(ctx);
  } else if (turbo_agent_state_review_approved(ctx->state)) {
    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_REVIEW_APPROVED, "review",
                           turbo_agent_state_review_note(ctx->state), NULL, 1);
  }
  return 0;
}

int turbo_agent_review_reset_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *note;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  note = turbo_agent_state_review_note(ctx->state);
  if (!note) {
    return 0;
  }

  if (turbo_agent_state_review_required(ctx->state) &&
      !turbo_agent_state_review_approved(ctx->state)) {
    return 0;
  }

  return turbo_agent_state_request_review(ctx->state, note);
}

int turbo_agent_review_approved_predicate(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  return ctx && ctx->state ? turbo_agent_state_review_approved(ctx->state) : 0;
}

int turbo_agent_review_gate_open_predicate(const turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)user_data;
  if (!ctx || !ctx->state) {
    return 0;
  }

  return !turbo_agent_state_review_required(ctx->state) ||
         turbo_agent_state_review_approved(ctx->state);
}

int turbo_agent_detect_failed_step_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *output_text;
  char *failure_reason = NULL;
  const char *failure_kind = NULL;
  size_t replan_limit = 0;
  size_t replan_count = 0;
  int tool_failed = 0;
  int text_failed = 0;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  output_text = turbo_agent_state_latest_executor_output_text(ctx->state);
  tool_failed = turbo_agent_state_executor_tool_results_failed(ctx->state);
  text_failed = turbo_agent_contains_failure_marker(output_text);
  if (tool_failed || text_failed) {
    int rc;

    failure_reason = turbo_agent_executor_failure_reason(ctx->state);
    failure_kind = tool_failed ? "tool_result" : "executor_text";
    rc = turbo_agent_state_set_failure(ctx->state, failure_kind,
                                       failure_reason && failure_reason[0] != '\0'
                                           ? failure_reason
                                           : "step failed");
    if (rc != 0) {
      free(failure_reason);
      return rc;
    }

    replan_limit = turbo_agent_state_replan_limit(ctx->state);
    replan_count = turbo_agent_state_replan_count(ctx->state);
    if (replan_limit > 0 && replan_count >= replan_limit) {
      char *final_answer = NULL;
      int needed = snprintf(NULL, 0, "FAILED: maximum replans exceeded after failure: %s",
                            failure_reason && failure_reason[0] != '\0' ? failure_reason
                                                                         : "step failed");
      turbo_agent_state_set_failure(ctx->state, "limit", "maximum replans exceeded");
      if (needed > 0) {
        final_answer = (char *)malloc((size_t)needed + 1);
        if (final_answer) {
          snprintf(final_answer, (size_t)needed + 1,
                   "FAILED: maximum replans exceeded after failure: %s",
                   failure_reason && failure_reason[0] != '\0' ? failure_reason : "step failed");
          turbo_agent_state_set_final_answer(ctx->state, final_answer);
          free(final_answer);
        }
      }
      turbo_graph_ctx_stop(ctx);
      free(failure_reason);
      return 0;
    }

    rc = turbo_agent_state_request_replan(ctx->state,
                                          failure_reason && failure_reason[0] != '\0'
                                              ? failure_reason
                                              : "step failed");
    free(failure_reason);
    return rc;
  }

  return 0;
}
