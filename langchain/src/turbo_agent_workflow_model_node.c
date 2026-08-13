#include "turbo_agent_graph.h"
#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_event_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_lifecycle_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_resilience_internal.h"
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_util_internal.h"
#include "turbo_agent_validation_internal.h"
#include "turbo_model_provider.h"

#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

int turbo_agent_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  json_value_t *attempt_state = NULL;
  char *request_json = NULL;
  char *response_json = NULL;
  json_value_t *response = NULL;
  char *detail = NULL;
  char *structured_reason = NULL;
  char *guardrail_reason = NULL;
  int rc;
  int stream_emit_failed = 0;
  int overflow_retried = 0;
  size_t attempt = 0;
  size_t provider_attempt = 0;

  if (!ctx || !ctx->state || !agent || !agent->transport_fn) {
    return -1;
  }

  if (agent->before_turn &&
      agent->before_turn(agent, ctx->state, agent->before_turn_user_data) != 0) {
    turbo_agent_state_set_model_error(ctx->state, "before_turn", "failed to prepare session turn");
    return -1;
  }

  attempt_state = ctx->state;
  for (attempt = 0;; ++attempt) {
    turbo_agent_clear_last_stream_sse(agent);
    if (turbo_agent_build_turn_request(agent, attempt_state, &request_json) != 0) {
      turbo_agent_state_set_model_error(ctx->state, "build_request",
                                        "failed to build model request");
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    if (turbo_agent_invoke_before_model_middlewares(agent, attempt_state, &request_json) != 0) {
      turbo_agent_state_set_model_error(ctx->state, "middleware", "before_model middleware failed");
      turbo_json_serialize_free(request_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }
    if (turbo_agent_invoke_before_model_guardrails(agent, attempt_state, request_json,
                                                   &guardrail_reason) != 0) {
      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "before_model",
          guardrail_reason ? guardrail_reason : "before_model guardrail rejected request");
      turbo_agent_emit_trace(
          agent, attempt_state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED, "before_model",
          guardrail_reason ? guardrail_reason : "guardrail rejected request", request_json, -1);
      turbo_agent_state_set_model_error(
          ctx->state, "guardrail",
          guardrail_reason ? guardrail_reason : "before_model guardrail rejected request");
      tstr_free(guardrail_reason);
      turbo_json_serialize_free(request_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    turbo_agent_emit_trace(agent, attempt_state, TURBO_AGENT_TRACE_MODEL_REQUEST, agent->model,
                           turbo_agent_provider_name(agent), request_json, (int)provider_attempt);
    rc = turbo_agent_resilient_transport(agent, ctx->state, request_json, &response_json);
    ++provider_attempt;
    if (turbo_agent_invoke_after_model_middlewares(agent, attempt_state, request_json,
                                                   &response_json, rc) != 0) {
      turbo_agent_state_set_model_error(ctx->state, "middleware", "after_model middleware failed");
      turbo_json_serialize_free(request_json);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }
    turbo_agent_emit_trace(agent, attempt_state, TURBO_AGENT_TRACE_MODEL_RESPONSE, agent->model,
                           turbo_agent_provider_name(agent), response_json, rc);
    if (!overflow_retried && agent->context_overflow) {
      int overflow_rc = agent->context_overflow(agent, attempt_state, rc, response_json,
                                                agent->context_overflow_user_data);
      if (overflow_rc == 1) {
        overflow_retried = 1;
        turbo_json_serialize_free(request_json);
        request_json = NULL;
        free(response_json);
        response_json = NULL;
        continue;
      }
      if (overflow_rc < 0) {
        turbo_agent_state_set_model_error(ctx->state, "context_compaction",
                                          "context overflow recovery failed");
        turbo_json_serialize_free(request_json);
        free(response_json);
        if (attempt_state != ctx->state) {
          turbo_free_json(&attempt_state);
        }
        return -1;
      }
    }
    turbo_json_serialize_free(request_json);
    request_json = NULL;
    if (rc != 0 || !response_json) {
      detail = response_json && response_json[0] != '\0'
                   ? turbo_agent_util_strdup(response_json)
                   : turbo_agent_util_strdup("model request transport failed");
      turbo_agent_state_set_model_error(ctx->state, "transport",
                                        detail ? detail : "model request transport failed");
      tstr_free(detail);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    if (turbo_parse_json((const uint8_t *)response_json, strlen(response_json), &response) != 0) {
      detail = turbo_agent_util_strdup(response_json);
      turbo_agent_state_set_model_error(ctx->state, "parse_response",
                                        detail ? detail : "failed to parse model response JSON");
      tstr_free(detail);
      free(response_json);
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }
    if (turbo_agent_usage_record(agent, ctx->state, response) != TURBO_OK) {
      turbo_agent_state_set_model_error(ctx->state, "usage", "failed to record provider usage");
      free(response_json);
      turbo_runtime_json_destroy(response);
      if (attempt_state != ctx->state) turbo_runtime_json_destroy(attempt_state);
      return -1;
    }

    rc = turbo_agent_append_model_event(agent, attempt_state, response);
    if (rc == 0 && turbo_agent_invoke_after_model_guardrails(agent, attempt_state, response_json,
                                                             response, &guardrail_reason) != 0) {
      turbo_agent_state_set_guardrail_rejection(
          ctx->state, "after_model",
          guardrail_reason ? guardrail_reason : "after_model guardrail rejected response");
      turbo_agent_emit_trace(
          agent, attempt_state, TURBO_AGENT_TRACE_GUARDRAIL_REJECTED, "after_model",
          guardrail_reason ? guardrail_reason : "guardrail rejected response", response_json, -1);
      rc = -1;
    }
    if (rc == 0 && ctx->event_sink &&
        turbo_model_provider_response_emit_json_value(agent->provider, response, ctx->event_sink,
                                                      ctx->event_sink_user_data) != 0) {
      stream_emit_failed = 1;
      rc = -1;
    }
    free(response_json);
    response_json = NULL;
    turbo_free_json(&response);
    response = NULL;
    if (rc != 0) {
      if (guardrail_reason) {
        turbo_agent_state_set_model_error(ctx->state, "guardrail", guardrail_reason);
        tstr_free(guardrail_reason);
      } else if (stream_emit_failed) {
        turbo_agent_state_set_model_error(ctx->state, "stream_event",
                                          "failed to emit model stream event");
      } else {
        turbo_agent_state_set_model_error(ctx->state, "append_model_event",
                                          "failed to append model event to state");
      }
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      return -1;
    }

    tstr_free(structured_reason);
    structured_reason = NULL;
    if (turbo_agent_structured_output_valid_for_state(agent, attempt_state, &structured_reason)) {
      if (attempt_state != ctx->state) {
        const json_value_t *event =
            turbo_agent_state_event_count(attempt_state) > 0
                ? turbo_agent_state_event_at(attempt_state,
                                             turbo_agent_state_event_count(attempt_state) - 1)
                : NULL;
        json_value_t *event_clone = NULL;

        if (!event || turbo_agent_clone_json(event, &event_clone) != TURBO_GRAPH_EXEC_OK ||
            turbo_agent_append_event(ctx->state, event_clone) != 0) {
          turbo_free_json(&event_clone);
          turbo_free_json(&attempt_state);
          tstr_free(structured_reason);
          turbo_agent_state_set_model_error(ctx->state, "append_model_event",
                                            "failed to append validated retry event");
          return -1;
        }
        turbo_free_json(&attempt_state);
      }
      tstr_free(structured_reason);
      turbo_agent_state_set_model_error(ctx->state, "", "");
      turbo_agent_state_set_guardrail_rejection(ctx->state, "", "");
      return 0;
    }

    if (!agent->structured_output_schema_json || agent->structured_output_max_retries == 0 ||
        attempt >= agent->structured_output_max_retries) {
      if (attempt_state != ctx->state) {
        turbo_free_json(&attempt_state);
      }
      turbo_agent_state_set_model_error(
          ctx->state, "structured_output",
          structured_reason && structured_reason[0] != '\0'
              ? structured_reason
              : "model output did not satisfy structured output schema");
      tstr_free(structured_reason);
      return -1;
    }

    turbo_agent_emit_trace(agent, ctx->state, TURBO_AGENT_TRACE_STRUCTURED_RETRY,
                           agent->structured_output_name ? agent->structured_output_name
                                                         : "structured_output",
                           structured_reason ? structured_reason : "schema_validation_failed", NULL,
                           (int)(attempt + 1));
    if (attempt_state != ctx->state) {
      turbo_free_json(&attempt_state);
    }
    attempt_state =
        turbo_agent_build_structured_retry_state(ctx->state, attempt, structured_reason);
    tstr_free(structured_reason);
    structured_reason = NULL;
    if (!attempt_state) {
      turbo_agent_state_set_model_error(ctx->state, "structured_output",
                                        "failed to prepare structured output retry");
      return -1;
    }
  }
}
