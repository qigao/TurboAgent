#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_event_internal.h"

#include "turbo_model_provider.h"

CXX_C_API const json_value_t *turbo_agent_request_responses_input_source(
    const json_value_t *state, json_value_t *request) {
  const json_value_t *last_event;
  const json_value_t *last_model_event;
  const json_value_t *input_source = NULL;

  if (!state || !request) {
    return NULL;
  }

  last_event = turbo_agent_state_last_event(state);
  last_model_event = turbo_agent_state_last_event_of_kind(state, "model");

  if (last_event && turbo_json_type(last_event) == TURBO_JSON_OBJECT) {
    input_source = turbo_json_object_get(last_event, "outputs");
    if (input_source && turbo_json_type(input_source) == TURBO_JSON_ARRAY) {
      const char *response_id = turbo_agent_model_event_response_id(last_model_event);
      if (response_id) {
        turbo_json_object_set_string(request, "previous_response_id", response_id);
      } else {
        input_source = NULL;
      }
    } else {
      input_source = NULL;
    }
  }

  if (!input_source) {
    input_source = turbo_agent_state_get_array_const(state, "input");
  }

  return input_source;
}

CXX_C_API json_value_t *
turbo_agent_request_build_responses_input_messages(const json_value_t *input_source) {
  json_value_t *messages_json_value;
  json_value_t *wire_messages;

  if (!input_source) {
    return NULL;
  }

  messages_json_value = turbo_json_clone(input_source);
  if (!messages_json_value) {
    return NULL;
  }

  wire_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_openai_responses(), messages_json_value, NULL);
  turbo_runtime_json_destroy(messages_json_value);
  return wire_messages;
}

CXX_C_API json_value_t *turbo_agent_request_build_chat_messages(const turbo_agent_t *agent,
                                                                const json_value_t *state) {
  json_value_t *canonical_messages;
  json_value_t *messages_json_value;
  json_value_t *wire_messages;

  canonical_messages = turbo_agent_request_build_canonical_messages(agent, state);
  if (!canonical_messages) {
    return NULL;
  }

  messages_json_value = turbo_json_clone(canonical_messages);
  turbo_free_json(&canonical_messages);
  if (!messages_json_value) {
    return NULL;
  }

  wire_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_openai_chat_completions(), messages_json_value, NULL);
  turbo_runtime_json_destroy(messages_json_value);
  return wire_messages;
}

CXX_C_API int turbo_agent_request_build_anthropic_wire_messages(const turbo_agent_t *agent,
                                                                const json_value_t *state,
                                                                json_value_t **out_messages,
                                                                char **out_system) {
  json_value_t *canonical_messages;
  json_value_t *messages_json_value;

  if (!out_messages) {
    return -1;
  }

  *out_messages = NULL;
  if (out_system) {
    *out_system = NULL;
  }

  canonical_messages = turbo_agent_request_build_canonical_messages(agent, state);
  if (!canonical_messages) {
    return -1;
  }

  messages_json_value = turbo_json_clone(canonical_messages);
  turbo_free_json(&canonical_messages);
  if (!messages_json_value) {
    return -1;
  }

  *out_messages = turbo_model_provider_messages_to_wire_json(
      turbo_model_provider_anthropic_messages(), messages_json_value, out_system);
  turbo_runtime_json_destroy(messages_json_value);
  return *out_messages ? 0 : -1;
}
