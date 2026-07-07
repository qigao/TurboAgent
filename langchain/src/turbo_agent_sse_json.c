#include "turbo_agent_sse_json_internal.h"

#include "turbo_prompt.h"

#include <string.h>

char *turbo_agent_sse_serialize_json_and_free(json_value_t *value) {
  char *serialized;

  if (!value) {
    return NULL;
  }

  serialized = turbo_json_serialize(value, NULL);
  turbo_free_json(&value);
  return serialized;
}

json_value_t *turbo_agent_sse_build_chat_tool_calls_array(
    const turbo_agent_sse_chat_stream_state_t *state) {
  json_value_t *tool_calls;
  size_t i;

  if (!state || state->tool_call_count == 0) {
    return NULL;
  }

  tool_calls = turbo_json_create_array();
  if (!tool_calls) {
    return NULL;
  }

  for (i = 0; i < state->tool_call_count; ++i) {
    const turbo_agent_sse_tool_call_t *tool_call = &state->tool_calls[i];
    json_value_t *tool_call_object;

    if (!tool_call->id || !tool_call->name || !tool_call->arguments) {
      turbo_free_json(&tool_calls);
      return NULL;
    }

    tool_call_object = turbo_prompt_chat_tool_call_create(
        tool_call->id, tool_call->type ? tool_call->type : "function", tool_call->name,
        tool_call->arguments);
    if (!tool_call_object) {
      turbo_free_json(&tool_calls);
      return NULL;
    }

    turbo_json_array_add(tool_calls, tool_call_object);
  }

  return tool_calls;
}

json_value_t *turbo_agent_sse_tool_call_input_object(
    const turbo_agent_sse_tool_call_t *tool_call) {
  json_value_t *input = NULL;

  if (!tool_call) {
    return NULL;
  }

  if (tool_call->arguments && tool_call->arguments[0] != '\0') {
    if (turbo_parse_json((const uint8_t *)tool_call->arguments, strlen(tool_call->arguments),
                         &input) != 0) {
      turbo_free_json(&input);
      return NULL;
    }
  } else {
    input = turbo_json_create_object();
  }

  return input;
}

json_value_t *turbo_agent_sse_chat_response_shell_create(
    const turbo_agent_sse_chat_stream_state_t *state, json_value_t **out_choice,
    json_value_t **out_message) {
  json_value_t *response;
  json_value_t *choices;
  json_value_t *choice;
  json_value_t *message;

  if (!state || !state->id || !out_choice || !out_message) {
    return NULL;
  }

  response = turbo_json_create_object();
  choices = turbo_json_create_array();
  choice = turbo_json_create_object();
  message = turbo_prompt_message_create(state->role && state->role[0] != '\0' ? state->role
                                                                               : "assistant",
                                        state->content ? state->content : "");
  if (!response || !choices || !choice || !message) {
    turbo_free_json(&response);
    turbo_free_json(&choices);
    turbo_free_json(&choice);
    turbo_free_json(&message);
    return NULL;
  }

  turbo_json_object_set_string(response, "id", state->id);
  turbo_json_object_set_string(response, "object", "chat.completion");
  turbo_json_object_set_number(choice, "index", 0);
  turbo_json_object_add(choice, "message", message);
  turbo_json_array_add(choices, choice);
  turbo_json_object_add(response, "choices", choices);

  *out_choice = choice;
  *out_message = message;
  return response;
}

json_value_t *turbo_agent_sse_anthropic_response_shell_create(
    const turbo_agent_sse_anthropic_stream_state_t *state, json_value_t **out_content) {
  json_value_t *response;
  json_value_t *content;

  if (!state || !state->id || !state->role || state->role[0] == '\0' || !out_content) {
    return NULL;
  }

  response = turbo_json_create_object();
  content = turbo_json_create_array();
  if (!response || !content) {
    turbo_free_json(&response);
    turbo_free_json(&content);
    return NULL;
  }

  turbo_json_object_set_string(response, "id", state->id);
  turbo_json_object_set_string(response, "role", state->role);
  turbo_json_object_add(response, "content", content);
  *out_content = content;
  return response;
}
