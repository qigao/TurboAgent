#include "turbo_agent_session.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_state.h"
#include "turbo_prompt.h"

/* Consume messages on both success and failure. Publish only a complete state;
 * source-message validation and the ownership transfer have one boundary. */
static json_value_t *turbo_agent_session_take_input_messages(json_value_t *messages) {
  json_value_t *state = NULL;
  if (!messages || json_type(messages) != JSON_ARRAY) goto failed;
  for (size_t i = 0; i < json_array_size(messages); ++i) {
    if (turbo_prompt_message_validate_json_value(json_array_get(messages, i)) != TURBO_PROMPT_OK)
      goto failed;
  }
  state = turbo_agent_state_create();
  if (!state || turbo_runtime_json_object_set(state, "input", messages) != TURBO_RUNTIME_JSON_OK)
    goto failed;
  return state;
failed:
  json_free(messages);
  json_free(state);
  return NULL;
}

CXX_C_API json_value_t *turbo_agent_session_create_input_state_json_value(const char *user_text) {
  json_value_t *state = turbo_agent_state_create();
  if (state && user_text && user_text[0] && turbo_agent_state_add_user_message(state, user_text) != 0) {
    json_free(state);
    return NULL;
  }
  return state;
}

CXX_C_API json_value_t *
turbo_agent_session_create_input_messages_state_json_value(const json_value_t *messages) {
  if (!messages || json_type(messages) != JSON_ARRAY) return NULL;
  return turbo_agent_session_take_input_messages(json_clone(messages));
}

CXX_C_API json_value_t *turbo_agent_session_create_follow_up_state_json_value(
    const turbo_agent_session_t *session, const json_value_t *completed_state,
    const char *user_text) {
  json_value_t *messages;
  if (!turbo_agent_session_agent(session) || !completed_state ||
      json_type(completed_state) != JSON_OBJECT || !user_text || !user_text[0]) return NULL;
  messages = turbo_agent_request_build_history_messages(completed_state);
  if (!messages) return NULL;
  if (turbo_prompt_messages_append(messages, "user", user_text) != TURBO_PROMPT_OK) {
    json_free(messages);
    return NULL;
  }
  return turbo_agent_session_take_input_messages(messages);
}

/* The input factory transfers one newly owned state into memory enrichment. */
static json_value_t *turbo_agent_session_bind_input_memory(
    const turbo_agent_session_t *session, json_value_t *state, const char *namespace_prefix) {
  if (state && turbo_agent_session_load_memory_context(session, state, namespace_prefix) != 0) {
    json_free(state);
    return NULL;
  }
  return state;
}

CXX_C_API json_value_t *turbo_agent_session_create_input_state_with_memory_json_value(
    const turbo_agent_session_t *session, const char *user_text, const char *namespace_prefix) {
  return turbo_agent_session_bind_input_memory(
      session, turbo_agent_session_create_input_state_json_value(user_text), namespace_prefix);
}

CXX_C_API json_value_t *turbo_agent_session_create_input_messages_state_with_memory_json_value(
    const turbo_agent_session_t *session, const json_value_t *messages,
    const char *namespace_prefix) {
  return turbo_agent_session_bind_input_memory(
      session, turbo_agent_session_create_input_messages_state_json_value(messages), namespace_prefix);
}
