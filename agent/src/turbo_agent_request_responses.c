#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_request_text_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_state_memory_internal.h"
#include "turbo_tool_schema.h"

#include <stdlib.h>

int turbo_agent_build_responses_turn_request(const turbo_agent_t *agent,
    json_value_t *state, char **out_request_json) {
  turbo_provider_request_t config;
  json_value_t *input = NULL;
  json_value_t *canonical_messages = NULL;
  json_value_t *tools = NULL;
  const json_value_t *input_source;
  char *instructions = NULL;
  int rc = -1;
  if (!agent || !state || !out_request_json) return -1;
  *out_request_json = NULL;
  turbo_agent_request_config(agent, &config);
  config.format = TURBO_PROVIDER_RESPONSES;

  if (agent->context_projection_active) {
    canonical_messages = turbo_agent_request_build_canonical_messages_with_options(agent, state, 0);
    if (!canonical_messages) goto done;
    input = turbo_provider_messages_to_wire_json(TURBO_PROVIDER_RESPONSES, canonical_messages, NULL);
  } else {
    input_source = turbo_agent_request_responses_input_source(state, &config.previous_response_id);
    if (!input_source) goto done;
    input = config.previous_response_id ? json_clone(input_source)
                                       : turbo_agent_request_build_responses_input_messages(input_source);
  }
  if (!input) goto done;
  instructions = turbo_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_agent_state_memory_layer_count(state) > 0) && !instructions)
    goto done;
  tools = turbo_tool_schema_build_openai_tools(agent->tool_registry);
  if (!tools) goto done;
  config.input = input;
  config.tools = tools;
  config.instructions = instructions;
  rc = turbo_provider_build_request(&config, out_request_json);
done:
  free(instructions);
  json_free(input);
  json_free(canonical_messages);
  json_free(tools);
  return rc;
}
