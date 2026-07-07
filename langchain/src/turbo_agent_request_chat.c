#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_structured_output_internal.h"
#include "turbo_agent_request_messages_internal.h"

#include "turbo_tool_schema.h"

#include <stdlib.h>

static int turbo_agent_build_chat_turn_request_with_options(const turbo_agent_t *agent,
                                                            json_value_t *state,
                                                            int compatible_mode,
                                                            char **out_request_json) {
  json_value_t *request;
  json_value_t *messages;
  json_value_t *tools = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_agent_request_create(agent);
  if (!request) {
    return -1;
  }

  messages = turbo_agent_request_build_chat_messages(agent, state);
  if (!messages) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "messages", messages);
  if (!compatible_mode && turbo_agent_request_parallel_tool_calls_enabled(agent)) {
    turbo_json_object_set_bool(request, "parallel_tool_calls", true);
  }

  tools = compatible_mode
              ? turbo_tool_schema_build_openai_compatible_chat_tools(agent->tool_registry)
              : turbo_tool_schema_build_openai_chat_tools(agent->tool_registry);
  if (!tools) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_agent_request_add_tools_if_any(request, tools) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_json_object_get(request, "tools")) {
    if (!compatible_mode) {
      turbo_json_object_set_string(request, "tool_choice", "auto");
    }
  }

  if (turbo_agent_request_add_structured_output(agent, request, 1, compatible_mode, 0) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  return turbo_agent_request_serialize_into_output(request, out_request_json);
}

CXX_C_API int turbo_agent_build_chat_turn_request(const turbo_agent_t *agent,
                                                  json_value_t *state,
                                                  char **out_request_json) {
  return turbo_agent_build_chat_turn_request_with_options(agent, state, 0, out_request_json);
}

CXX_C_API int turbo_agent_build_compatible_chat_turn_request(const turbo_agent_t *agent,
                                                             json_value_t *state,
                                                             char **out_request_json) {
  return turbo_agent_build_chat_turn_request_with_options(agent, state, 1, out_request_json);
}

CXX_C_API int turbo_agent_build_anthropic_messages_turn_request(const turbo_agent_t *agent,
                                                                json_value_t *state,
                                                                char **out_request_json) {
  json_value_t *request;
  json_value_t *messages;
  json_value_t *tools = NULL;
  char *system_from_messages = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_agent_request_create(agent);
  if (!request) {
    return -1;
  }

  if (turbo_agent_request_build_anthropic_wire_messages(agent, state, &messages,
                                                        &system_from_messages) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "messages", messages);
  if (system_from_messages && system_from_messages[0] != '\0') {
    turbo_json_object_set_string(request, "system", system_from_messages);
  }
  free(system_from_messages);

  tools = turbo_tool_schema_build_anthropic_tools(agent->tool_registry);
  if (!tools) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_agent_request_add_tools_if_any(request, tools) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_agent_request_add_structured_output(agent, request, 0, 0, 1) != 0) {
    turbo_free_json(&request);
    return -1;
  }

  return turbo_agent_request_serialize_into_output(request, out_request_json);
}
