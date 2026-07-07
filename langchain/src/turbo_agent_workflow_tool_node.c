#include "turbo_agent_graph.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

static char *turbo_agent_tool_approval_note_create(const char *call_id, const char *tool_name,
                                                   const char *arguments_json) {
  json_value_t *note_json;
  char *note;

  if (!call_id || !tool_name || !arguments_json) {
    return NULL;
  }

  note_json = turbo_json_create_object();
  if (!note_json) {
    return NULL;
  }
  turbo_json_object_set_string(note_json, "kind", "tool_approval");
  turbo_json_object_set_string(note_json, "call_id", call_id);
  turbo_json_object_set_string(note_json, "tool_name", tool_name);
  turbo_json_object_set_string(note_json, "arguments", arguments_json);
  note = turbo_json_serialize(note_json, NULL);
  turbo_free_json(&note_json);
  return note;
}

int turbo_agent_tool_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  const json_value_t *tool_calls;
  json_value_t *event;
  json_value_t *outputs;
  size_t i;
  char *guardrail_reason = NULL;
  int guardrail_rejected = 0;

  if (!ctx || !ctx->state || !agent) {
    return -1;
  }

  tool_calls = turbo_agent_last_model_tool_calls(ctx->state);
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    turbo_agent_state_set_model_error(ctx->state, "tool", "no pending tool calls");
    return -1;
  }

  event = turbo_agent_event_create("tool_results");
  outputs = turbo_json_create_array();
  if (!event || !outputs) {
    turbo_agent_state_set_model_error(ctx->state, "tool",
                                      "failed to allocate tool result containers");
    turbo_free_json(&event);
    turbo_free_json(&outputs);
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
    const json_value_t *call = turbo_json_array_get(tool_calls, i);
    const char *call_id;
    const char *name;
    const char *arguments_json;
    turbo_agent_execution_context_t saved_context = {0};
    turbo_agent_execution_context_t tool_context = {0};
    char *mutable_arguments_json = NULL;
    char *output = NULL;
    turbo_tool_status_t status;
    json_value_t *output_item;

    if (!turbo_agent_tool_call_record_fields(call, &call_id, &name, &arguments_json)) {
      turbo_agent_state_set_model_error(ctx->state, "tool",
                                        "malformed pending tool call record");
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    turbo_agent_execution_context_get(&saved_context);
    tool_context = saved_context;
    tool_context.tool_call_id = call_id;
    tool_context.tool_name = name;
    turbo_agent_execution_context_set(&tool_context);

    mutable_arguments_json = turbo_agent_util_strdup(arguments_json ? arguments_json : "{}");
    if (!mutable_arguments_json) {
      turbo_agent_execution_context_set(&saved_context);
      turbo_agent_state_set_model_error(ctx->state, "tool",
                                        "failed to allocate tool arguments buffer");
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_agent_invoke_before_tool_middlewares(agent, ctx->state, call_id, name,
                                                   &mutable_arguments_json) != 0) {
      turbo_agent_execution_context_set(&saved_context);
      turbo_agent_state_set_model_error(ctx->state, "middleware",
                                        "before_tool middleware failed");
      tstr_free(mutable_arguments_json);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_agent_invoke_before_tool_guardrails(agent, ctx->state, call_id, name,
                                                  mutable_arguments_json,
                                                  &guardrail_reason) != 0) {
      if (guardrail_reason && strcmp(guardrail_reason, "approval_required") == 0) {
        char *approval_note;

        turbo_agent_execution_context_set(&saved_context);
        approval_note = turbo_agent_tool_approval_note_create(call_id, name, mutable_arguments_json);
        if (!approval_note ||
            turbo_agent_state_request_review(ctx->state, approval_note) != 0) {
          turbo_agent_state_set_model_error(ctx->state, "review",
                                            "failed to request tool approval");
          turbo_json_serialize_free(approval_note);
          tstr_free(guardrail_reason);
          tstr_free(mutable_arguments_json);
          turbo_free_json(&event);
          turbo_free_json(&outputs);
          return -1;
        }
        turbo_json_serialize_free(approval_note);
        turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_REVIEW_REQUIRED,
                               "before_tool", name, mutable_arguments_json, 0);
        if (ctx->current_node) {
          turbo_graph_ctx_set_next(ctx, ctx->current_node);
        }
        turbo_graph_ctx_stop(ctx);
        tstr_free(guardrail_reason);
        tstr_free(mutable_arguments_json);
        turbo_free_json(&event);
        turbo_free_json(&outputs);
        return 0;
      }
      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "before_tool",
          guardrail_reason ? guardrail_reason : "before_tool guardrail rejected tool dispatch");
      guardrail_rejected = 1;
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED,
                             "before_tool",
                             guardrail_reason ? guardrail_reason
                                              : "guardrail rejected tool dispatch",
                             mutable_arguments_json, -1);
      output = turbo_agent_format_tool_error(
          name, guardrail_reason ? guardrail_reason : "tool_guardrail_rejected");
      tstr_free(guardrail_reason);
      guardrail_reason = NULL;
      if (!output) {
        turbo_agent_execution_context_set(&saved_context);
        turbo_agent_state_set_model_error(ctx->state, "guardrail",
                                          "failed to format guardrail tool error payload");
        tstr_free(mutable_arguments_json);
        turbo_free_json(&event);
        turbo_free_json(&outputs);
        return -1;
      }
      status = TURBO_TOOL_ERROR;
    } else {
      turbo_agent_current_tool_approval_set(turbo_agent_tool_approval_review_matches(
          ctx->state, call_id, name, mutable_arguments_json));
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_TOOL_DISPATCH, name, call_id,
                             mutable_arguments_json, 0);
      status = turbo_tool_registry_execute(agent->tool_registry, name, mutable_arguments_json,
                                           &output);
      if (status != TURBO_TOOL_OK || !output) {
        output = turbo_agent_format_tool_error(
            name, status == TURBO_TOOL_NOT_FOUND ? "tool_not_found" : "tool_execution_failed");
        if (!output) {
          turbo_agent_execution_context_set(&saved_context);
          turbo_agent_state_set_model_error(ctx->state, "tool",
                                            "failed to format tool error payload");
          tstr_free(mutable_arguments_json);
          turbo_free_json(&event);
          turbo_free_json(&outputs);
          return -1;
        }
      }
    }
    if (turbo_agent_invoke_after_tool_middlewares(agent, ctx->state, call_id, name,
                                                  mutable_arguments_json, &output, status) != 0) {
      turbo_agent_execution_context_set(&saved_context);
      turbo_agent_state_set_model_error(ctx->state, "middleware",
                                        "after_tool middleware failed");
      tstr_free(mutable_arguments_json);
      free(output);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    if (turbo_agent_invoke_after_tool_guardrails(agent, ctx->state, call_id, name,
                                                 mutable_arguments_json, output, status,
                                                 &guardrail_reason) != 0) {
      char *guardrail_output;

      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "after_tool",
          guardrail_reason ? guardrail_reason : "after_tool guardrail rejected tool output");
      guardrail_rejected = 1;
      turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED,
                             "after_tool",
                             guardrail_reason ? guardrail_reason
                                              : "guardrail rejected tool output",
                             output, -1);
      guardrail_output = turbo_agent_format_tool_error(
          name, guardrail_reason ? guardrail_reason : "tool_output_guardrail_rejected");
      tstr_free(guardrail_reason);
      guardrail_reason = NULL;
      if (!guardrail_output) {
        turbo_agent_execution_context_set(&saved_context);
        turbo_agent_state_set_model_error(ctx->state, "guardrail",
                                          "failed to format guardrail error payload");
        tstr_free(mutable_arguments_json);
        free(output);
        turbo_free_json(&event);
        turbo_free_json(&outputs);
        return -1;
      }
      free(output);
      output = guardrail_output;
      status = TURBO_TOOL_ERROR;
    }
    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_TOOL_RESULT, name, call_id,
                           output, (int)status);

    output_item = turbo_agent_tool_result_output_item_create(call_id, output);
    if (!output_item) {
      turbo_agent_execution_context_set(&saved_context);
      turbo_agent_state_set_model_error(ctx->state, "tool", "failed to record tool output");
      tstr_free(mutable_arguments_json);
      free(output);
      turbo_free_json(&event);
      turbo_free_json(&outputs);
      return -1;
    }
    turbo_json_array_add(outputs, output_item);
    turbo_agent_execution_context_set(&saved_context);
    tstr_free(mutable_arguments_json);
    free(output);
  }

  turbo_json_object_add(event, "outputs", outputs);
  if (!guardrail_rejected) {
    turbo_agent_state_set_guardrail_rejection(ctx->state, "", "");
  }
  return turbo_agent_append_event(ctx->state, event);
}
