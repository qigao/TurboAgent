#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_tool_authority_internal.h"
#include "turbo_tool_schema.h"

#include <stdlib.h>

static int turbo_agent_build_chat_turn_request_with_options(const turbo_agent_t *agent,
    json_value_t *state, turbo_provider_format_t format, char **out_request_json) {
  turbo_provider_request_t config;
  json_value_t *messages = NULL;
  json_value_t *tools = NULL;
  turbo_tool_registry_t *visible_registry = NULL;
  char *system = NULL;
  int rc = -1;
  if (!agent || !state || !out_request_json) return -1;
  *out_request_json = NULL;
  turbo_agent_request_config(agent, &config);
  config.format = format;
  if (turbo_agent_tool_authority_project(&agent->tool_policy, agent->tool_registry,
                                         &visible_registry) != TURBO_TOOL_OK) goto done;
  if (format == TURBO_PROVIDER_ANTHROPIC) {
    if (turbo_agent_request_build_anthropic_wire_messages(agent, state, &messages, &system) != 0)
      goto done;
    tools = turbo_tool_schema_build_anthropic_tools(visible_registry);
  } else {
    messages = turbo_agent_request_build_chat_messages(agent, state);
    tools = format == TURBO_PROVIDER_COMPATIBLE_CHAT
                ? turbo_tool_schema_build_openai_compatible_chat_tools(visible_registry)
                : turbo_tool_schema_build_openai_chat_tools(visible_registry);
  }
  if (!messages || !tools) goto done;
  config.input = messages;
  config.tools = tools;
  config.instructions = system;
  rc = turbo_provider_build_request(&config, out_request_json);
done:
  json_free(messages);
  json_free(tools);
  free(system);
  turbo_tool_registry_destroy(visible_registry);
  return rc;
}

int turbo_agent_build_chat_turn_request(const turbo_agent_t *agent,
    json_value_t *state, char **out_request_json) {
  return turbo_agent_build_chat_turn_request_with_options(agent, state, TURBO_PROVIDER_CHAT, out_request_json);
}

int turbo_agent_build_compatible_chat_turn_request(const turbo_agent_t *agent,
    json_value_t *state, char **out_request_json) {
  return turbo_agent_build_chat_turn_request_with_options(agent, state, TURBO_PROVIDER_COMPATIBLE_CHAT, out_request_json);
}

int turbo_agent_build_anthropic_messages_turn_request(const turbo_agent_t *agent,
    json_value_t *state, char **out_request_json) {
  return turbo_agent_build_chat_turn_request_with_options(agent, state, TURBO_PROVIDER_ANTHROPIC, out_request_json);
}
