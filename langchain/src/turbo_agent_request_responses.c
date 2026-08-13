#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_request_structured_output_internal.h"
#include "turbo_agent_request_text_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_state_memory_internal.h"

#include "turbo_model_provider.h"
#include "turbo_tool_schema.h"

#include <stdlib.h>

CXX_C_API int turbo_agent_build_responses_turn_request(const turbo_agent_t *agent,
                                                       json_value_t *state,
                                                       char **out_request_json) {
  json_value_t *request;
  json_value_t *input_clone = NULL;
  json_value_t *canonical_messages = NULL;
  json_value_t *tools = NULL;
  const json_value_t *input_source = NULL;
  char *effective_instructions = NULL;

  if (!agent || !state || !out_request_json) {
    return -1;
  }

  *out_request_json = NULL;
  request = turbo_agent_request_create(agent);
  if (!request) {
    return -1;
  }

  if (agent->context_projection_active) {
    canonical_messages = turbo_agent_request_build_canonical_messages_with_options(agent, state, 0);
    if (!canonical_messages) {
      turbo_free_json(&request);
      return -1;
    }
    input_clone = turbo_model_provider_messages_to_wire_json(
        turbo_model_provider_openai_responses(), canonical_messages, NULL);
    turbo_runtime_json_destroy(canonical_messages);
    if (!input_clone) {
      turbo_free_json(&request);
      return -1;
    }
  } else {
    input_source = turbo_agent_request_responses_input_source(state, request);
    if (!input_source) {
      turbo_free_json(&request);
      return -1;
    }

    if (turbo_json_object_get(request, "previous_response_id")) {
      if (turbo_agent_clone_json(input_source, &input_clone) != TURBO_GRAPH_EXEC_OK) {
        turbo_free_json(&request);
        return -1;
      }
    } else {
      input_clone = turbo_agent_request_build_responses_input_messages(input_source);
      if (!input_clone) {
        turbo_free_json(&request);
        return -1;
      }
    }
  }

  effective_instructions = turbo_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_agent_state_memory_layer_count(state) > 0) &&
      !effective_instructions) {
    turbo_free_json(&request);
    return -1;
  }

  turbo_json_object_add(request, "input", input_clone);
  if (effective_instructions && effective_instructions[0] != '\0') {
    turbo_json_object_set_string(request, "instructions", effective_instructions);
  }
  if (turbo_agent_request_parallel_tool_calls_enabled(agent)) {
    turbo_json_object_set_bool(request, "parallel_tool_calls", true);
  }

  tools = turbo_tool_schema_build_openai_tools(agent->tool_registry);
  if (!tools) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_agent_request_add_tools_if_any(request, tools) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  if (turbo_agent_request_add_structured_output(agent, request, 0, 0, 0) != 0) {
    free(effective_instructions);
    turbo_free_json(&request);
    return -1;
  }

  {
    int rc = turbo_agent_request_serialize_into_output(request, out_request_json);
    free(effective_instructions);
    return rc;
  }
}
