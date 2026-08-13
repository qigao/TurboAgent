#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_hooks_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_action_tool.h"
#include "turbo_agent_policy.h"
#include "turbo_event.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const turbo_action_tool_registry_t *action_registry;
  turbo_agent_policy_t policy;
} turbo_agent_action_policy_guardrail_t;

CXX_C_API int turbo_agent_tool_approval_review_matches(const json_value_t *state,
                                                       const char *call_id,
                                                       const char *tool_name,
                                                       const char *arguments_json) {
  const char *note;
  json_value_t *note_json = NULL;
  const char *kind;
  const char *approved_call_id;
  const char *approved_tool_name;
  const char *approved_arguments;
  int matched = 0;

  if (!state || !call_id || !tool_name || !arguments_json ||
      !turbo_agent_state_review_approved(state)) {
    return 0;
  }

  note = turbo_agent_state_review_note(state);
  if (!note || note[0] == '\0') {
    return 0;
  }
  if (turbo_parse_json((const uint8_t *)note, strlen(note), &note_json) != 0 || !note_json ||
      turbo_json_type(note_json) != TURBO_JSON_OBJECT) {
    turbo_free_json(&note_json);
    return 0;
  }

  kind = turbo_json_get_string(note_json, "kind");
  approved_call_id = turbo_json_get_string(note_json, "call_id");
  approved_tool_name = turbo_json_get_string(note_json, "tool_name");
  approved_arguments = turbo_json_get_string(note_json, "arguments");
  matched = kind && strcmp(kind, "tool_approval") == 0 && approved_call_id &&
                    strcmp(approved_call_id, call_id) == 0 && approved_tool_name &&
                    strcmp(approved_tool_name, tool_name) == 0 && approved_arguments &&
                    strcmp(approved_arguments, arguments_json) == 0;
  turbo_free_json(&note_json);
  return matched ? 1 : 0;
}

static int turbo_agent_middleware_reserve(turbo_agent_t *agent) {
  turbo_agent_middleware_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->middleware_count < agent->middleware_capacity) {
    return 0;
  }

  new_capacity = agent->middleware_capacity == 0 ? 4 : agent->middleware_capacity * 2;
  resized =
      (turbo_agent_middleware_t *)realloc(agent->middlewares, new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->middlewares = resized;
  agent->middleware_capacity = new_capacity;
  return 0;
}

CXX_C_API int turbo_agent_add_middleware(turbo_agent_t *agent,
                                         const turbo_agent_middleware_t *middleware) {
  if (!agent || !middleware) {
    return -1;
  }

  if (turbo_agent_middleware_reserve(agent) != 0) {
    return -1;
  }

  agent->middlewares[agent->middleware_count++] = *middleware;
  return 0;
}

static int turbo_agent_guardrail_reserve(turbo_agent_t *agent) {
  turbo_agent_guardrail_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->guardrail_count < agent->guardrail_capacity) {
    return 0;
  }

  new_capacity = agent->guardrail_capacity == 0 ? 4 : agent->guardrail_capacity * 2;
  resized = (turbo_agent_guardrail_t *)realloc(agent->guardrails,
                                               new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->guardrails = resized;
  agent->guardrail_capacity = new_capacity;
  return 0;
}

CXX_C_API int turbo_agent_add_guardrail(turbo_agent_t *agent,
                                        const turbo_agent_guardrail_t *guardrail) {
  if (!agent || !guardrail) {
    return -1;
  }

  if (turbo_agent_guardrail_reserve(agent) != 0) {
    return -1;
  }

  agent->guardrails[agent->guardrail_count++] = *guardrail;
  return 0;
}

static int turbo_agent_action_policy_before_tool(
    turbo_agent_t *agent, const json_value_t *state, const char *call_id,
    const char *tool_name, const char *arguments_json, const char **out_reason,
    void *user_data) {
  turbo_agent_action_policy_guardrail_t *guardrail_data =
      (turbo_agent_action_policy_guardrail_t *)user_data;
  const turbo_action_tool_definition_t *definition;
  turbo_agent_policy_decision_t decision;
  json_value_t *args = NULL;
  const char *reason = NULL;

  (void)agent;
  (void)state;
  (void)call_id;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!guardrail_data || !guardrail_data->action_registry || !tool_name || !arguments_json) {
    return -1;
  }

  definition = turbo_action_tool_registry_find(guardrail_data->action_registry, tool_name);
  if (!definition) {
    if (out_reason) {
      *out_reason = "action_not_found";
    }
    return -1;
  }

  if (turbo_parse_json((const uint8_t *)arguments_json, strlen(arguments_json), &args) != 0) {
    if (out_reason) {
      *out_reason = "invalid_tool_arguments";
    }
    return -1;
  }

  decision = turbo_agent_policy_check_action(&guardrail_data->policy, definition, args, &reason);
  turbo_free_json(&args);
  if (decision == TURBO_AGENT_POLICY_ALLOW) {
    return 0;
  }

  if (out_reason) {
    if (decision == TURBO_AGENT_POLICY_REQUIRE_APPROVAL) {
      if (turbo_agent_tool_approval_review_matches(state, call_id, tool_name, arguments_json)) {
        return 0;
      }
      *out_reason = "approval_required";
    } else {
      *out_reason = reason && reason[0] != '\0' ? reason : "policy_denied";
    }
  }
  return -1;
}

CXX_C_API int turbo_agent_add_action_policy_guardrail(
    turbo_agent_t *agent, const turbo_action_tool_registry_t *action_tool_registry,
    const turbo_agent_policy_t *policy) {
  turbo_agent_action_policy_guardrail_t *guardrail_data;
  turbo_agent_guardrail_t guardrail;

  if (!agent || !action_tool_registry) {
    return -1;
  }

  guardrail_data =
      (turbo_agent_action_policy_guardrail_t *)calloc(1, sizeof(*guardrail_data));
  if (!guardrail_data) {
    return -1;
  }

  guardrail_data->action_registry = action_tool_registry;
  guardrail_data->policy = policy ? *policy : turbo_agent_policy_default();
  memset(&guardrail, 0, sizeof(guardrail));
  guardrail.before_tool = turbo_agent_action_policy_before_tool;
  guardrail.user_data = guardrail_data;
  guardrail.user_data_free = turbo_agent_util_free_user_data;
  if (turbo_agent_add_guardrail(agent, &guardrail) != 0) {
    free(guardrail_data);
    return -1;
  }

  return 0;
}

static int turbo_agent_trace_sink_reserve(turbo_agent_t *agent) {
  turbo_agent_trace_sink_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->trace_sink_count < agent->trace_sink_capacity) {
    return 0;
  }

  new_capacity = agent->trace_sink_capacity == 0 ? 2 : agent->trace_sink_capacity * 2;
  resized =
      (turbo_agent_trace_sink_t *)realloc(agent->trace_sinks, new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->trace_sinks = resized;
  agent->trace_sink_capacity = new_capacity;
  return 0;
}

static int turbo_agent_trace_json_value_sink_reserve(turbo_agent_t *agent) {
  turbo_agent_trace_json_value_sink_t *resized;
  size_t new_capacity;

  if (!agent) {
    return -1;
  }

  if (agent->trace_json_value_sink_count < agent->trace_json_value_sink_capacity) {
    return 0;
  }

  new_capacity = agent->trace_json_value_sink_capacity == 0 ? 2 : agent->trace_json_value_sink_capacity * 2;
  resized = (turbo_agent_trace_json_value_sink_t *)realloc(agent->trace_json_value_sinks,
                                                     new_capacity * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  agent->trace_json_value_sinks = resized;
  agent->trace_json_value_sink_capacity = new_capacity;
  return 0;
}

CXX_C_API int turbo_agent_add_trace_sink(turbo_agent_t *agent,
                                         const turbo_agent_trace_sink_t *sink) {
  if (!agent || !sink || !sink->callback) {
    return -1;
  }

  if (turbo_agent_trace_sink_reserve(agent) != 0) {
    return -1;
  }

  agent->trace_sinks[agent->trace_sink_count++] = *sink;
  return 0;
}

CXX_C_API int turbo_agent_add_trace_json_value_sink(
    turbo_agent_t *agent, const turbo_agent_trace_json_value_sink_t *sink) {
  if (!agent || !sink || !sink->callback) {
    return -1;
  }

  if (turbo_agent_trace_json_value_sink_reserve(agent) != 0) {
    return -1;
  }

  agent->trace_json_value_sinks[agent->trace_json_value_sink_count++] = *sink;
  return 0;
}

CXX_C_API int turbo_agent_set_trace_history_enabled(turbo_agent_t *agent, int enabled) {
  if (!agent) {
    return -1;
  }

  agent->capture_trace_history = enabled ? 1 : 0;
  return 0;
}

static const char *turbo_agent_trace_kind_name(turbo_agent_trace_event_kind_t kind) {
  switch (kind) {
    case TURBO_AGENT_TRACE_MODEL_REQUEST:
      return "model_request";
    case TURBO_AGENT_TRACE_MODEL_RESPONSE:
      return "model_response";
    case TURBO_AGENT_TRACE_TOOL_DISPATCH:
      return "tool_dispatch";
    case TURBO_AGENT_TRACE_TOOL_RESULT:
      return "tool_result";
    case TURBO_AGENT_TRACE_STRUCTURED_RETRY:
      return "structured_retry";
    case TURBO_AGENT_TRACE_REPLAN_REQUESTED:
      return "replan_requested";
    case TURBO_AGENT_TRACE_REVIEW_REQUIRED:
      return "review_required";
    case TURBO_AGENT_TRACE_REVIEW_APPROVED:
      return "review_approved";
    case TURBO_AGENT_TRACE_GUARDRAIL_REJECTED:
      return "guardrail_rejected";
    case TURBO_AGENT_TRACE_MEMORY_LOAD:
      return "memory_load";
    case TURBO_AGENT_TRACE_MEMORY_SAVE:
      return "memory_save";
    default:
      return "unknown";
  }
}

static int turbo_agent_append_trace_history(json_value_t *state,
                                            turbo_agent_trace_event_kind_t kind,
                                            const char *name, const char *detail,
                                            const char *payload, int status) {
  json_value_t *events;
  json_value_t *event_json_value;
  json_value_t *event;

  if (!state) {
    return -1;
  }

  events = turbo_agent_state_get_or_create_array(state, "trace_events");
  if (!events) {
    return -1;
  }

  event_json_value = turbo_event_trace_create_json_value(turbo_agent_trace_kind_name(kind),
                                             detail ? detail : "", payload ? payload : "",
                                             status);
  if (!event_json_value) {
    return -1;
  }
  event = turbo_json_clone(event_json_value);
  turbo_runtime_json_destroy(event_json_value);
  if (!event) {
    return -1;
  }

  turbo_json_object_set_number(event, "kind_code", (double)kind);
  turbo_json_object_set_string(event, "name", name ? name : "");
  turbo_json_array_add(events, event);
  return 0;
}

CXX_C_API void turbo_agent_emit_trace(turbo_agent_t *agent, json_value_t *state,
                                      turbo_agent_trace_event_kind_t kind, const char *name,
                                      const char *detail, const char *payload, int status) {
  size_t i;

  if (!agent) {
    return;
  }

  if (agent->capture_trace_history) {
    turbo_agent_append_trace_history(state, kind, name, detail, payload, status);
  }

  for (i = 0; i < agent->trace_sink_count; ++i) {
    turbo_agent_trace_sink_t *sink = &agent->trace_sinks[i];
    if (sink->callback) {
      sink->callback(agent, state, kind, name, detail, payload, status, sink->user_data);
    }
  }

  if (agent->trace_json_value_sink_count > 0) {
    json_value_t *event =
        turbo_event_trace_create_json_value(turbo_agent_trace_kind_name(kind), detail, payload,
                                      status);
    if (event) {
      for (i = 0; i < agent->trace_json_value_sink_count; ++i) {
        turbo_agent_trace_json_value_sink_t *sink = &agent->trace_json_value_sinks[i];
        if (sink->callback) {
          sink->callback(agent, event, sink->user_data);
        }
      }
      turbo_runtime_json_destroy(event);
    }
  }
}

CXX_C_API int turbo_agent_invoke_before_model_guardrails(turbo_agent_t *agent,
                                                         const json_value_t *state,
                                                         const char *request_json,
                                                         char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !request_json) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->before_model &&
        guardrail->before_model(agent, state, request_json, &reason,
                                guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_agent_util_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_after_model_guardrails(turbo_agent_t *agent,
                                                        const json_value_t *state,
                                                        const char *response_json,
                                                        const json_value_t *response,
                                                        char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->after_model &&
        guardrail->after_model(agent, state, response_json, response, &reason,
                               guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_agent_util_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_before_tool_guardrails(turbo_agent_t *agent,
                                                        const json_value_t *state,
                                                        const char *call_id,
                                                        const char *tool_name,
                                                        const char *arguments_json,
                                                        char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !tool_name || !arguments_json) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->before_tool &&
        guardrail->before_tool(agent, state, call_id, tool_name, arguments_json, &reason,
                               guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_agent_util_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_after_tool_guardrails(turbo_agent_t *agent,
                                                       const json_value_t *state,
                                                       const char *call_id,
                                                       const char *tool_name,
                                                       const char *arguments_json,
                                                       const char *output,
                                                       turbo_tool_status_t tool_status,
                                                       char **out_reason) {
  size_t i;

  if (out_reason) {
    *out_reason = NULL;
  }
  if (!agent || !tool_name || !arguments_json || !output) {
    return -1;
  }

  for (i = 0; i < agent->guardrail_count; ++i) {
    turbo_agent_guardrail_t *guardrail = &agent->guardrails[i];
    const char *reason = NULL;
    if (guardrail->after_tool &&
        guardrail->after_tool(agent, state, call_id, tool_name, arguments_json, output,
                              tool_status, &reason, guardrail->user_data) != 0) {
      if (out_reason && reason) {
        *out_reason = turbo_agent_util_strdup(reason);
      }
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_before_model_middlewares(turbo_agent_t *agent,
                                                          json_value_t *state,
                                                          char **inout_request_json) {
  size_t i;

  if (!agent || !inout_request_json || !*inout_request_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->before_model &&
        middleware->before_model(agent, state, inout_request_json,
                                 middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_request_json) {
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_after_model_middlewares(turbo_agent_t *agent,
                                                         json_value_t *state,
                                                         const char *request_json,
                                                         char **inout_response_json,
                                                         int transport_status) {
  size_t i;

  if (!agent || !inout_response_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->after_model &&
        middleware->after_model(agent, state, request_json, inout_response_json,
                                transport_status, middleware->user_data) != 0) {
      return -1;
    }
    if (transport_status == 0 && !*inout_response_json) {
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_before_tool_middlewares(turbo_agent_t *agent,
                                                         json_value_t *state,
                                                         const char *call_id,
                                                         const char *tool_name,
                                                         char **inout_arguments_json) {
  size_t i;

  if (!agent || !tool_name || !inout_arguments_json || !*inout_arguments_json) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->before_tool &&
        middleware->before_tool(agent, state, call_id, tool_name, inout_arguments_json,
                                middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_arguments_json) {
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_invoke_after_tool_middlewares(turbo_agent_t *agent,
                                                        json_value_t *state,
                                                        const char *call_id,
                                                        const char *tool_name,
                                                        const char *arguments_json,
                                                        char **inout_output,
                                                        turbo_tool_status_t tool_status) {
  size_t i;

  if (!agent || !tool_name || !arguments_json || !inout_output || !*inout_output) {
    return -1;
  }

  for (i = 0; i < agent->middleware_count; ++i) {
    turbo_agent_middleware_t *middleware = &agent->middlewares[i];

    if (middleware->after_tool &&
        middleware->after_tool(agent, state, call_id, tool_name, arguments_json, inout_output,
                               tool_status, middleware->user_data) != 0) {
      return -1;
    }
    if (!*inout_output) {
      return -1;
    }
  }

  return 0;
}
