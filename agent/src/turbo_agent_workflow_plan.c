#include "turbo_agent_graph.h"
#include <json_parser.h>
#include "turbo_agent_workflow_graph_internal.h"
#include "turbo_agent_plan_step_origin_internal.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_snapshot_internal.h"
#include "turbo_agent_state_flow_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_prompt.h"
#include <fmt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_value_t *turbo_agent_planner_loop_build_step_input(const json_value_t *state) {
  const json_value_t *input;
  json_value_t *step_input;
  json_value_t *context_message = NULL;
  json_value_t *message;
  const char *step_text;
  char *completed_steps_message = NULL;
  size_t step_index;
  size_t step_count;
  int needed;
  char *prompt;

  if (!state) {
    return NULL;
  }

  input = turbo_agent_state_get_array_const(state, "input");
  step_text = turbo_agent_state_current_plan_step_text(state);
  step_index = turbo_agent_state_plan_step_index(state);
  step_count = turbo_agent_state_plan_step_count(state);
  if (!input || !step_text || step_text[0] == '\0') {
    return NULL;
  }

  step_input = NULL;
  if (turbo_agent_clone_json(input, &step_input) != TURBO_GRAPH_EXEC_OK) {
    return NULL;
  }

  completed_steps_message = turbo_agent_build_completed_steps_message(state);
  if (completed_steps_message) {
    context_message = turbo_prompt_message_create("user", completed_steps_message);
    if (!context_message) {
      tstr_free(completed_steps_message);
      json_free(step_input); step_input = NULL;
      return NULL;
    }
    json_array_add(step_input, context_message);
    tstr_free(completed_steps_message);
  }

  needed = snprintf(NULL, 0,
                    "Execute plan step %lu of %lu: %s\nReturn only the result for this step.",
                    (unsigned long)(step_index + 1), (unsigned long)step_count, step_text);
  if (needed < 0) {
    json_free(step_input); step_input = NULL;
    return NULL;
  }

  prompt = (char *)malloc((size_t)needed + 1);
  if (!prompt) {
    json_free(step_input); step_input = NULL;
    return NULL;
  }

  snprintf(prompt, (size_t)needed + 1,
           "Execute plan step %lu of %lu: %s\nReturn only the result for this step.",
           (unsigned long)(step_index + 1), (unsigned long)step_count, step_text);
  message = turbo_prompt_message_create("user", prompt);
  free(prompt);
  if (!message) {
    json_free(step_input); step_input = NULL;
    return NULL;
  }
  json_array_add(step_input, message);
  return step_input;
}

int turbo_agent_plan_step_prepare_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  json_value_t *step_input = NULL, *step_events = NULL, *step_history = NULL;
  json_value_t *executor_state = NULL, *origin = NULL;
  const json_value_t *plans, *progress_versions, *progress, *steps;
  size_t plan_version, step_index;
  int rc = -1;

  (void)user_data;
  if (!ctx || !ctx->state || turbo_agent_state_plan_complete(ctx->state)) {
    return -1;
  }

  plans = json_object_get(ctx->state, "plan_versions");
  progress_versions = json_object_get(ctx->state, "plan_progress_versions");
  if (json_type(plans) != JSON_ARRAY || !json_array_size(plans) ||
      json_type(progress_versions) != JSON_ARRAY ||
      json_array_size(progress_versions) != json_array_size(plans)) return -1;
  plan_version = json_array_size(plans) - 1;
  progress = json_array_get(progress_versions, plan_version);
  steps = json_object_get(json_array_get(plans, plan_version), "steps");
  if (json_type(progress) != JSON_ARRAY || json_type(steps) != JSON_ARRAY) return -1;
  step_index = json_array_size(progress);
  if (step_index >= json_array_size(steps) || plan_version > INT64_MAX || step_index > INT64_MAX) return -1;
  origin = json_create_object();
  if (!origin || turbo_runtime_json_object_set_int64(origin, "schema_version",
      TURBO_AGENT_PLAN_STEP_ORIGIN_VERSION) != 0) goto done;
#define WRITE_ORIGIN_OFFSET(key) \
  if (turbo_runtime_json_object_set_int64(origin, #key, (int64_t)key) != 0) goto done;
  Replay(TURBO_AGENT_PLAN_STEP_ORIGIN_FIELDS, WRITE_ORIGIN_OFFSET)
#undef WRITE_ORIGIN_OFFSET

  step_input = turbo_agent_planner_loop_build_step_input(ctx->state);
  step_events = json_create_array();
  step_history = json_create_array();
  if (!step_input || !step_events || !step_history) {
    goto done;
  }

  executor_state = json_create_object();
  if (!executor_state || turbo_runtime_json_object_set(executor_state, "input", step_input) != 0) goto done;
  step_input = NULL;
  if (turbo_runtime_json_object_set(executor_state, "events", step_events) != 0) goto done;
  step_events = NULL;
  if (turbo_runtime_json_object_set(executor_state, TURBO_AGENT_PLAN_STEP_ORIGIN_KEY, origin) != 0) goto done;
  origin = NULL;

  if (turbo_agent_substate_copy_memory(executor_state, ctx->state) != 0) goto done;

  if (turbo_agent_state_append_array_version(ctx->state, "executor_event_versions", step_history) !=
      0) {
    goto done;
  }
  step_history = NULL;
  if (turbo_agent_state_append_object_version(ctx->state, "executor_state_versions", executor_state) != 0) {
    goto done;
  }
  executor_state = NULL;
  rc = 0;
done:
  json_free(step_input); json_free(step_events); json_free(step_history);
  json_free(executor_state); json_free(origin);
  return rc;
}

int turbo_agent_executor_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  json_value_t *executor_state;
  turbo_graph_exec_ctx_t temp_ctx = {0};
  int rc;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  executor_state = turbo_agent_state_ensure_versioned_substate(
      ctx->state, "executor_state_versions", "executor_input_versions", "executor_event_versions");
  if (!executor_state) {
    return -1;
  }

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = executor_state;
  rc = turbo_agent_model_node(&temp_ctx, agent);
  if (rc == 0) {
    rc = turbo_agent_append_last_substate_event(ctx->state, "executor_event_versions",
                                                executor_state);
  } else {
    turbo_agent_copy_model_error(ctx->state, executor_state);
  }

  return rc;
}

static int turbo_agent_nested_tool_node(turbo_graph_exec_ctx_t *ctx, turbo_agent_t *agent,
                                        json_value_t *nested_state,
                                        const char *event_versions_key,
                                        const char *state_versions_key, const char *journal_domain) {
  turbo_graph_exec_ctx_t temp_ctx = {0};
  const json_value_t *versions;
  tstr journal_scope;
  int rc;

  if (!ctx || !ctx->state || !agent || !nested_state || !event_versions_key ||
      !state_versions_key || !journal_domain) {
    return -1;
  }
  versions = json_object_get(ctx->state, state_versions_key);
  if (json_type(versions) != JSON_ARRAY || !json_array_size(versions) ||
      json_array_get(versions, json_array_size(versions) - 1) != nested_state) return -1;

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = nested_state;
  temp_ctx.current_node = ctx->current_node;
  if (turbo_agent_state_review_approved(ctx->state) &&
      turbo_agent_state_review_note(ctx->state)) {
    const char *parent_note = turbo_agent_state_review_note(ctx->state);
    const char *nested_note = turbo_agent_state_review_note(nested_state);
    if (!nested_note || strcmp(nested_note, parent_note) != 0) {
      if (turbo_agent_state_request_review(nested_state, parent_note) != 0) {
        return -1;
      }
    }
    if (turbo_agent_state_set_review_approved(nested_state, 1) != 0) {
      return -1;
    }
  }

  journal_scope = tstr_format("{}:{}", journal_domain, json_array_size(versions) - 1);
  if (!journal_scope) return -1;
  rc = turbo_agent_tool_node_scoped(&temp_ctx, agent, journal_scope);
  tstr_free(journal_scope);
  if (temp_ctx.stop) {
    const char *note = turbo_agent_state_review_note(nested_state);
    if (turbo_agent_state_review_required(nested_state) &&
        !turbo_agent_state_review_approved(nested_state) && note &&
        turbo_agent_state_request_review(ctx->state, note) != 0) {
      return -1;
    }
    if (ctx->current_node) {
      turbo_graph_ctx_set_next(ctx, ctx->current_node);
    }
    turbo_graph_ctx_stop(ctx);
  } else if (rc == 0) {
    rc = turbo_agent_append_last_substate_event(ctx->state, event_versions_key, nested_state);
  } else {
    turbo_agent_copy_model_error(ctx->state, nested_state);
    turbo_agent_copy_guardrail_rejection(ctx->state, nested_state);
  }

  return rc;
}

int turbo_agent_executor_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  json_value_t *executor_state;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  executor_state = turbo_agent_state_ensure_versioned_substate(
      ctx->state, "executor_state_versions", "executor_input_versions", "executor_event_versions");
  if (!executor_state) {
    return -1;
  }

  if (turbo_agent_state_replan_requested(ctx->state)) {
    return 0;
  }

  return turbo_agent_nested_tool_node(ctx, agent, executor_state, "executor_event_versions",
      "executor_state_versions", "executor");
}

int turbo_agent_planner_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  json_value_t *planner_state;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  planner_state = turbo_agent_state_ensure_versioned_substate(
      ctx->state, "planner_state_versions", "planner_input_versions", "planner_event_versions");
  if (!planner_state) {
    /* Initial planning uses the root event stream before a replanner substate
     * exists. Give that producer its own identity, separate from root tool loops. */
    return turbo_agent_tool_node_scoped(ctx, agent, "planner:root");
  }

  return turbo_agent_nested_tool_node(ctx, agent, planner_state, "planner_event_versions",
      "planner_state_versions", "planner");
}

int turbo_agent_plan_advance_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *step_text;
  const char *output_text;
  size_t step_index;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  step_text = turbo_agent_state_current_plan_step_text(ctx->state);
  output_text = turbo_agent_state_latest_executor_output_text(ctx->state);
  step_index = turbo_agent_state_plan_step_index(ctx->state);
  if (step_text && output_text && output_text[0] != '\0' &&
      turbo_agent_append_completed_step(ctx->state, step_index, step_text, output_text) != 0) {
    return -1;
  }

  return turbo_agent_state_advance_plan(ctx->state);
}

int turbo_agent_planner_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  json_value_t *planner_state;
  turbo_graph_exec_ctx_t temp_ctx = {0};
  int rc;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  planner_state = turbo_agent_state_ensure_versioned_substate(
      ctx->state, "planner_state_versions", "planner_input_versions", "planner_event_versions");
  if (!planner_state) {
    return turbo_agent_model_node(ctx, user_data);
  }

  temp_ctx.graph = ctx->graph;
  temp_ctx.state = planner_state;
  rc = turbo_agent_model_node(&temp_ctx, agent);
  if (rc == 0) {
    rc = turbo_agent_append_last_substate_event(ctx->state, "planner_event_versions",
                                                planner_state);
  } else {
    turbo_agent_copy_model_error(ctx->state, planner_state);
  }

  return rc;
}

int turbo_agent_plan_commit_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const char *output_text;

  (void)user_data;
  if (!ctx || !ctx->state) {
    return -1;
  }

  output_text = turbo_agent_state_last_output_text(ctx->state);
  if (!output_text || output_text[0] == '\0') {
    return -1;
  }

  return turbo_agent_state_set_plan_from_json(ctx->state, output_text);
}
