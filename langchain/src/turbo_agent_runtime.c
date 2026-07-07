#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_lifecycle_internal.h"
#include "turbo_agent_runtime_internal.h"

#include "turbo_action_tool.h"
#include "turbo_model_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define TURBO_AGENT_THREAD_LOCAL __declspec(thread)
#else
#define TURBO_AGENT_THREAD_LOCAL _Thread_local
#endif

static TURBO_AGENT_THREAD_LOCAL turbo_agent_execution_context_t
    turbo_agent_current_execution_context = {0};
static TURBO_AGENT_THREAD_LOCAL int turbo_agent_current_tool_approval = 0;

static int turbo_agent_starts_with_nocase(const char *text, const char *prefix) {
  size_t i;

  if (!text || !prefix) {
    return 0;
  }

  for (i = 0; prefix[i] != '\0'; ++i) {
    if (text[i] == '\0' ||
        tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i])) {
      return 0;
    }
  }

  return 1;
}

CXX_C_API const char *turbo_agent_provider_name(const turbo_agent_t *agent) {
  return agent && agent->provider ? agent->provider->name : NULL;
}

CXX_C_API turbo_graph_exec_status_t turbo_agent_clone_json(const json_value_t *value,
                                                           json_value_t **out_value) {
  if (!out_value) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_value = NULL;
  if (!value) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  *out_value = turbo_json_clone(value);
  return *out_value ? TURBO_GRAPH_EXEC_OK : TURBO_GRAPH_EXEC_OUT_OF_MEMORY;
}

CXX_C_API char *turbo_agent_format_tool_error(const char *name, const char *message) {
  json_value_t *result;
  char *buffer;

  if (!name) {
    name = "unknown";
  }
  if (!message) {
    message = "tool execution failed";
  }

  result = turbo_action_result_create(0, message);
  if (!result) {
    return NULL;
  }

  turbo_json_object_set_string(result, "tool", name);
  turbo_json_object_set_string(result, "stderr", message);
  buffer = turbo_json_serialize(result, NULL);
  turbo_free_json(&result);
  return buffer;
}

CXX_C_API int turbo_agent_build_turn_request(turbo_agent_t *agent, json_value_t *state,
                                             char **out_request_json) {
  if (!agent || !agent->provider || !agent->provider->build_request) {
    return -1;
  }

  return agent->provider->build_request(agent, state, out_request_json);
}

CXX_C_API int turbo_agent_append_model_event(turbo_agent_t *agent, json_value_t *state,
                                             const json_value_t *response) {
  json_value_t *event = NULL;

  if (!agent || !agent->provider || !state) {
    return -1;
  }

  if (agent->last_stream_sse && agent->last_stream_sse_len > 0) {
    event = turbo_model_provider_sse_to_event_json(agent->provider, agent->last_stream_sse,
                                                   agent->last_stream_sse_len);
  }
  if (!event && response) {
    event = turbo_model_provider_response_to_event_json(agent->provider, response);
  }
  turbo_agent_clear_last_stream_sse(agent);
  if (!event) {
    return -1;
  }

  return turbo_agent_append_event(state, event);
}

CXX_C_API int turbo_agent_copy_model_error(json_value_t *dst_state,
                                           const json_value_t *src_state) {
  if (!turbo_agent_state_model_error_phase(src_state) &&
      !turbo_agent_state_model_error_detail(src_state)) {
    return 0;
  }

  return turbo_agent_state_set_model_error(
      dst_state, turbo_agent_state_model_error_phase(src_state),
      turbo_agent_state_model_error_detail(src_state));
}

CXX_C_API int turbo_agent_copy_guardrail_rejection(json_value_t *dst_state,
                                                   const json_value_t *src_state) {
  if (!turbo_agent_state_guardrail_rejection_phase(src_state) &&
      !turbo_agent_state_guardrail_rejection_reason(src_state)) {
    return 0;
  }

  return turbo_agent_state_set_guardrail_rejection(
      dst_state, turbo_agent_state_guardrail_rejection_phase(src_state),
      turbo_agent_state_guardrail_rejection_reason(src_state));
}

CXX_C_API int turbo_agent_contains_failure_marker(const char *text) {
  const char *line_start;

  if (!text || text[0] == '\0') {
    return 0;
  }

  line_start = text;
  while (line_start && *line_start) {
    if (turbo_agent_starts_with_nocase(line_start, "FAILED:") ||
        turbo_agent_starts_with_nocase(line_start, "ERROR:") ||
        (turbo_agent_starts_with_nocase(line_start, "failed") &&
         (line_start[6] == '\0' || line_start[6] == '\n' ||
          isspace((unsigned char)line_start[6])))) {
      return 1;
    }
    line_start = strchr(line_start, '\n');
    if (line_start) {
      ++line_start;
    }
  }

  return 0;
}

CXX_C_API void turbo_agent_execution_context_get(
    turbo_agent_execution_context_t *out_context) {
  if (!out_context) {
    return;
  }
  *out_context = turbo_agent_current_execution_context;
}

CXX_C_API void turbo_agent_execution_context_set(
    const turbo_agent_execution_context_t *context) {
  if (!context) {
    memset(&turbo_agent_current_execution_context, 0,
           sizeof(turbo_agent_current_execution_context));
    turbo_agent_current_tool_approval = 0;
    return;
  }
  turbo_agent_current_execution_context = *context;
  turbo_agent_current_tool_approval = 0;
}

CXX_C_API void turbo_agent_current_tool_approval_set(int granted) {
  turbo_agent_current_tool_approval = granted ? 1 : 0;
}

CXX_C_API int turbo_agent_current_tool_approval_granted(void) {
  return turbo_agent_current_tool_approval ? 1 : 0;
}
