#include "turbo_agent_graph.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_event_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_tool_executor_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_tool_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_agent_tool_approval_note_create(const char *call_id, const char *tool_name,
                                                   const char *arguments_json) {
  json_value_t *note_json;
  char *note;
  if (!call_id || !tool_name || !arguments_json) return NULL;
  note_json = turbo_json_create_object();
  if (!note_json) return NULL;
  turbo_json_object_set_string(note_json, "kind", "tool_approval");
  turbo_json_object_set_string(note_json, "call_id", call_id);
  turbo_json_object_set_string(note_json, "tool_name", tool_name);
  turbo_json_object_set_string(note_json, "arguments", arguments_json);
  note = turbo_json_serialize(note_json, NULL);
  turbo_free_json(&note_json);
  return note;
}

static const char *turbo_agent_tool_status_reason(turbo_tool_status_t status) {
  switch (status) {
  case TURBO_TOOL_NOT_FOUND:
    return "tool_not_found";
  case TURBO_TOOL_CANCELLED:
    return "tool_cancelled";
  case TURBO_TOOL_DEADLINE_EXCEEDED:
    return "tool_deadline_exceeded";
  case TURBO_TOOL_OUTPUT_LIMIT:
    return "tool_output_limit_exceeded";
  case TURBO_TOOL_UNKNOWN_SIDE_EFFECT:
    return "tool_side_effect_unknown";
  case TURBO_TOOL_BACKPRESSURE:
    return "tool_executor_backpressure";
  default:
    return "tool_execution_failed";
  }
}

static void turbo_agent_tool_executions_destroy(turbo_agent_tool_execution_t *calls, size_t count) {
  size_t index;
  if (!calls) return;
  for (index = 0; index < count; ++index) {
    if (calls[index].arguments_owned) tstr_free((char *)calls[index].arguments_json);
    free(calls[index].output);
  }
  free(calls);
}

static int turbo_agent_tool_calls_preflight(const turbo_tool_registry_t *registry,
                                            const turbo_agent_policy_t *policy,
                                            const json_value_t *tool_calls,
                                            turbo_agent_tool_execution_t *calls, size_t count) {
  size_t index;
  size_t previous;
  for (index = 0; index < count; ++index) {
    const json_value_t *call = turbo_json_array_get(tool_calls, index);
    if (!turbo_agent_tool_call_record_fields(call, &calls[index].call_id, &calls[index].tool_name,
                                             &calls[index].arguments_json) ||
        !calls[index].call_id[0] || !calls[index].tool_name[0])
      return TURBO_EPROTO;
    for (previous = 0; previous < index; ++previous) {
      if (strcmp(calls[previous].call_id, calls[index].call_id) == 0) return TURBO_EPROTO;
    }
    calls[index].status = turbo_tool_registry_get_execution_policy(registry, calls[index].tool_name,
                                                                   &calls[index].policy);
    if (calls[index].status == TURBO_TOOL_NOT_FOUND) {
      calls[index].policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
      calls[index].policy.idempotency = TURBO_TOOL_IDEMPOTENCY_NONE;
      calls[index].replayed = 1;
      continue;
    }
    if (calls[index].status != TURBO_TOOL_OK) return TURBO_EPROTO;
    if (turbo_agent_policy_check_tool(policy, registry, calls[index].tool_name,
                                      &calls[index].policy_reason) != TURBO_AGENT_POLICY_ALLOW) {
      calls[index].status = TURBO_TOOL_ERROR;
      calls[index].replayed = 1;
    }
  }
  return TURBO_OK;
}

int turbo_agent_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  const json_value_t *tool_calls;
  turbo_agent_tool_execution_t *calls = NULL;
  turbo_agent_execution_context_t saved_context = {0};
  json_value_t *event = NULL;
  json_value_t *outputs = NULL;
  size_t count;
  size_t index;
  char turn_key[32];
  int guardrail_rejected = 0;
  int rc = -1;

  if (!ctx || !ctx->state || !agent) return -1;
  tool_calls = turbo_agent_last_model_tool_calls(ctx->state);
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "no pending tool calls");
    return -1;
  }
  count = turbo_json_array_size(tool_calls);
  if (count == 0) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "empty pending tool call batch");
    return -1;
  }
  turbo_agent_execution_context_get(&saved_context);
  snprintf(turn_key, sizeof(turn_key), "%zu", turbo_agent_state_event_count(ctx->state) - 1);
  calls = (turbo_agent_tool_execution_t *)calloc(count, sizeof(*calls));
  if (!calls) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "failed to allocate tool batch");
    return -1;
  }
  if (turbo_agent_tool_calls_preflight(agent->tool_registry, &agent->tool_policy, tool_calls, calls,
                                       count) != TURBO_OK) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "malformed pending tool call record");
    goto cleanup;
  }
  if (!agent->tool_registry || !agent->tool_executor) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "tool registry is not configured");
    goto cleanup;
  }
  for (index = 0; index < count; ++index)
    calls[index].turn_key = turn_key;

  for (index = 0; index < count; ++index) {
    turbo_agent_execution_context_t tool_context = saved_context;
    char *mutable_arguments;
    char *guardrail_reason = NULL;
    if (calls[index].policy_reason) {
      turbo_agent_state_set_guardrail_rejection(ctx->state, "tool_policy",
                                                calls[index].policy_reason);
      guardrail_rejected = 1;
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED, "tool_policy",
                             calls[index].policy_reason, calls[index].tool_name, -1);
      calls[index].output =
          turbo_agent_format_tool_error(calls[index].tool_name, calls[index].policy_reason);
      if (!calls[index].output) {
        turbo_agent_state_set_model_error(ctx->state, "tool_policy",
                                          "failed to format policy error payload");
        goto cleanup;
      }
      continue;
    }
    if (calls[index].replayed) continue;
    tool_context.tool_call_id = calls[index].call_id;
    tool_context.tool_name = calls[index].tool_name;
    turbo_agent_execution_context_set(&tool_context);
    mutable_arguments =
        turbo_agent_util_strdup(calls[index].arguments_json ? calls[index].arguments_json : "{}");
    if (!mutable_arguments) {
      turbo_agent_state_set_model_error(ctx->state, "tool",
                                        "failed to allocate tool arguments buffer");
      goto cleanup;
    }
    calls[index].arguments_json = mutable_arguments;
    calls[index].arguments_owned = 1;
    if (turbo_agent_invoke_before_tool_middlewares(agent, ctx->state, calls[index].call_id,
                                                   calls[index].tool_name,
                                                   (char **)&calls[index].arguments_json) != 0) {
      turbo_agent_state_set_model_error(ctx->state, "middleware", "before_tool middleware failed");
      goto cleanup;
    }
    if (turbo_agent_invoke_before_tool_guardrails(
            agent, ctx->state, calls[index].call_id, calls[index].tool_name,
            calls[index].arguments_json, &guardrail_reason) != 0) {
      if (guardrail_reason && strcmp(guardrail_reason, "approval_required") == 0) {
        char *approval_note = turbo_agent_tool_approval_note_create(
            calls[index].call_id, calls[index].tool_name, calls[index].arguments_json);
        if (!approval_note || turbo_agent_state_request_review(ctx->state, approval_note) != 0) {
          turbo_agent_state_set_model_error(ctx->state, "review",
                                            "failed to request tool approval");
          turbo_json_serialize_free(approval_note);
          tstr_free(guardrail_reason);
          goto cleanup;
        }
        turbo_json_serialize_free(approval_note);
        turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_REVIEW_REQUIRED, "before_tool",
                               calls[index].tool_name, calls[index].arguments_json, 0);
        if (ctx->current_node) turbo_graph_ctx_set_next(ctx, ctx->current_node);
        turbo_graph_ctx_stop(ctx);
        tstr_free(guardrail_reason);
        rc = 0;
        goto cleanup;
      }
      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "before_tool",
          guardrail_reason ? guardrail_reason : "before_tool guardrail rejected tool dispatch");
      guardrail_rejected = 1;
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED, "before_tool",
                             guardrail_reason ? guardrail_reason
                                              : "guardrail rejected tool dispatch",
                             calls[index].arguments_json, -1);
      calls[index].output = turbo_agent_format_tool_error(
          calls[index].tool_name, guardrail_reason ? guardrail_reason : "tool_guardrail_rejected");
      calls[index].status = TURBO_TOOL_ERROR;
      calls[index].replayed = 1;
      tstr_free(guardrail_reason);
      if (!calls[index].output) {
        turbo_agent_state_set_model_error(ctx->state, "guardrail",
                                          "failed to format guardrail tool error payload");
        goto cleanup;
      }
      continue;
    }
    calls[index].approval_granted = turbo_agent_tool_approval_review_matches(
        ctx->state, calls[index].call_id, calls[index].tool_name, calls[index].arguments_json);
    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_TOOL_DISPATCH,
                           calls[index].tool_name, calls[index].call_id,
                           calls[index].arguments_json, 0);
  }
  turbo_agent_execution_context_set(&saved_context);

  rc = turbo_agent_tool_executor_execute(agent->tool_executor, saved_context.runtime,
                                         saved_context.cancel_token, saved_context.thread_id,
                                         saved_context.run_id, agent->tool_registry,
                                         &agent->tool_policy, calls, count);
  if (rc != TURBO_OK) {
    turbo_agent_state_set_model_error(ctx->state, "tool_executor",
                                      "tool batch planning or dispatch failed");
    rc = -1;
    goto cleanup;
  }

  event = turbo_agent_event_create("tool_results");
  outputs = turbo_json_create_array();
  if (!event || !outputs) {
    turbo_agent_state_set_model_error(ctx->state, "tool",
                                      "failed to allocate tool result containers");
    rc = -1;
    goto cleanup;
  }
  for (index = 0; index < count; ++index) {
    turbo_agent_execution_context_t tool_context = saved_context;
    char *guardrail_reason = NULL;
    json_value_t *output_item;
    tool_context.tool_call_id = calls[index].call_id;
    tool_context.tool_name = calls[index].tool_name;
    turbo_agent_execution_context_set(&tool_context);
    if (!calls[index].output) {
      calls[index].output = turbo_agent_format_tool_error(
          calls[index].tool_name, turbo_agent_tool_status_reason(calls[index].status));
      if (!calls[index].output) {
        turbo_agent_state_set_model_error(ctx->state, "tool",
                                          "failed to format tool error payload");
        rc = -1;
        goto cleanup;
      }
    }
    if (turbo_agent_invoke_after_tool_middlewares(
            agent, ctx->state, calls[index].call_id, calls[index].tool_name,
            calls[index].arguments_json ? calls[index].arguments_json : "{}", &calls[index].output,
            calls[index].status) != 0) {
      turbo_agent_state_set_model_error(ctx->state, "middleware", "after_tool middleware failed");
      rc = -1;
      goto cleanup;
    }
    if (turbo_agent_invoke_after_tool_guardrails(
            agent, ctx->state, calls[index].call_id, calls[index].tool_name,
            calls[index].arguments_json ? calls[index].arguments_json : "{}", calls[index].output,
            calls[index].status, &guardrail_reason) != 0) {
      char *guardrail_output;
      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "after_tool",
          guardrail_reason ? guardrail_reason : "after_tool guardrail rejected tool output");
      guardrail_rejected = 1;
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED, "after_tool",
                             guardrail_reason ? guardrail_reason : "guardrail rejected tool output",
                             calls[index].output, -1);
      guardrail_output = turbo_agent_format_tool_error(
          calls[index].tool_name,
          guardrail_reason ? guardrail_reason : "tool_output_guardrail_rejected");
      tstr_free(guardrail_reason);
      if (!guardrail_output) {
        turbo_agent_state_set_model_error(ctx->state, "guardrail",
                                          "failed to format guardrail error payload");
        rc = -1;
        goto cleanup;
      }
      free(calls[index].output);
      calls[index].output = guardrail_output;
      calls[index].status = TURBO_TOOL_ERROR;
    }
    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_TOOL_RESULT, calls[index].tool_name,
                           calls[index].call_id, calls[index].output, (int)calls[index].status);
    output_item =
        turbo_agent_tool_result_output_item_create(calls[index].call_id, calls[index].output);
    if (!output_item) {
      turbo_agent_state_set_model_error(ctx->state, "tool", "failed to record tool output");
      rc = -1;
      goto cleanup;
    }
    turbo_json_array_add(outputs, output_item);
  }
  turbo_json_object_add(event, "outputs", outputs);
  outputs = NULL;
  if (!guardrail_rejected) turbo_agent_state_set_guardrail_rejection(ctx->state, "", "");
  rc = turbo_agent_append_event(ctx->state, event);
  event = NULL;

cleanup:
  turbo_agent_execution_context_set(&saved_context);
  turbo_runtime_json_destroy(event);
  turbo_runtime_json_destroy(outputs);
  turbo_agent_tool_executions_destroy(calls, count);
  return rc;
}
