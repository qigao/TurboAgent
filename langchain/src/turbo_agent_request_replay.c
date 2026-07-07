#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_memory_internal.h"
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_text_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_runtime_internal.h"

#include "turbo_prompt.h"

#include <stdlib.h>
#include <string.h>

typedef int (*turbo_agent_request_message_event_append_fn)(json_value_t *messages,
                                                           const json_value_t *event);

static int turbo_agent_request_array_add_clone(json_value_t *array, const json_value_t *value) {
  json_value_t *clone = NULL;

  if (!array || !value) {
    return -1;
  }

  if (turbo_agent_clone_json(value, &clone) != TURBO_GRAPH_EXEC_OK) {
    return -1;
  }

  turbo_json_array_add(array, clone);
  return 0;
}

static int turbo_agent_request_tool_result_output_fields(const json_value_t *output_item,
                                                         const char **call_id,
                                                         const char **output) {
  const char *call_id_value;
  const char *output_value;

  if (call_id) {
    *call_id = NULL;
  }
  if (output) {
    *output = NULL;
  }

  if (!output_item || turbo_json_type(output_item) != TURBO_JSON_OBJECT) {
    return 0;
  }

  call_id_value = turbo_json_get_string(output_item, "call_id");
  output_value = turbo_json_get_string(output_item, "output");
  if (!call_id_value || !output_value) {
    return 0;
  }

  if (call_id) {
    *call_id = call_id_value;
  }
  if (output) {
    *output = output_value;
  }
  return 1;
}

static int turbo_agent_request_message_event_inputs_valid(const json_value_t *event,
                                                          const json_value_t *messages) {
  return messages && event && turbo_json_type(event) == TURBO_JSON_OBJECT ? 1 : 0;
}

static const json_value_t *
turbo_agent_request_tool_results_event_outputs(const json_value_t *event) {
  const json_value_t *outputs;

  if (!turbo_agent_event_kind_is(event, "tool_results")) {
    return NULL;
  }

  outputs = turbo_json_object_get(event, "outputs");
  return outputs && turbo_json_type(outputs) == TURBO_JSON_ARRAY ? outputs : NULL;
}

static int turbo_agent_request_append_chat_model_event_message(json_value_t *messages,
                                                               const json_value_t *event) {
  const json_value_t *tool_calls = turbo_agent_model_event_tool_calls(event);
  const char *output_text = turbo_agent_event_output_text(event);
  size_t tool_call_count = turbo_agent_model_event_tool_call_count(event);

  if (!turbo_agent_request_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if (tool_call_count > 0) {
    json_value_t *assistant_tool_calls = turbo_json_create_array();
    json_value_t *assistant = NULL;
    size_t j;

    if (!assistant_tool_calls) {
      turbo_free_json(&assistant_tool_calls);
      return -1;
    }

    for (j = 0; j < tool_call_count; ++j) {
      const json_value_t *tool_call = turbo_json_array_get(tool_calls, j);
      const char *call_id;
      const char *name;
      const char *arguments;
      json_value_t *chat_tool_call;

      if (!turbo_agent_tool_call_record_fields(tool_call, &call_id, &name, &arguments)) {
        turbo_free_json(&assistant);
        turbo_free_json(&assistant_tool_calls);
        return -1;
      }

      chat_tool_call = turbo_prompt_chat_tool_call_create(call_id, "function", name, arguments);
      if (!chat_tool_call) {
        turbo_free_json(&assistant);
        turbo_free_json(&assistant_tool_calls);
        return -1;
      }
      turbo_json_array_add(assistant_tool_calls, chat_tool_call);
    }

    assistant = turbo_prompt_chat_assistant_message_create(output_text, assistant_tool_calls);
    if (!assistant) {
      return -1;
    }

    turbo_json_array_add(messages, assistant);
    return 0;
  }

  if (output_text) {
    json_value_t *assistant = turbo_prompt_message_create("assistant", output_text);
    if (!assistant) {
      return -1;
    }
    turbo_json_array_add(messages, assistant);
  }

  return 0;
}

static int turbo_agent_request_append_chat_tool_results_messages(json_value_t *messages,
                                                                 const json_value_t *event) {
  const json_value_t *outputs = turbo_agent_request_tool_results_event_outputs(event);
  size_t j;

  if (!turbo_agent_request_message_event_inputs_valid(event, messages)) {
    return -1;
  }

  if (!outputs) {
    return -1;
  }

  for (j = 0; j < turbo_json_array_size(outputs); ++j) {
    const json_value_t *output = turbo_json_array_get(outputs, j);
    const char *call_id;
    const char *content;
    json_value_t *tool_message;

    if (!turbo_agent_request_tool_result_output_fields(output, &call_id, &content)) {
      return -1;
    }

    tool_message = turbo_prompt_chat_tool_message_create(call_id, content);
    if (!tool_message) {
      return -1;
    }
    turbo_json_array_add(messages, tool_message);
  }

  return 0;
}

static int turbo_agent_request_replay_message_events(
    const json_value_t *events, json_value_t *messages,
    turbo_agent_request_message_event_append_fn append_model_event,
    turbo_agent_request_message_event_append_fn append_tool_results_event) {
  size_t i;

  if (!messages) {
    return -1;
  }

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return 0;
  }

  for (i = 0; i < turbo_json_array_size(events); ++i) {
    const json_value_t *event = turbo_json_array_get(events, i);
    if (turbo_agent_event_kind_is(event, "model")) {
      if (!append_model_event || append_model_event(messages, event) != 0) {
        return -1;
      }
    } else if (turbo_agent_event_kind_is(event, "tool_results")) {
      if (!append_tool_results_event || append_tool_results_event(messages, event) != 0) {
        return -1;
      }
    }
  }

  return 0;
}

CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages(
    const turbo_agent_t *agent, const json_value_t *state) {
  const json_value_t *input;
  const json_value_t *events;
  json_value_t *messages;
  char *effective_instructions;
  size_t i;

  if (!agent || !state) {
    return NULL;
  }

  messages = turbo_prompt_messages_create();
  if (!messages) {
    return NULL;
  }

  effective_instructions = turbo_agent_build_effective_instructions(agent, state);
  if ((agent->instructions || turbo_agent_state_memory_layer_count(state) > 0) &&
      !effective_instructions) {
    turbo_free_json(&messages);
    return NULL;
  }

  if (effective_instructions && effective_instructions[0] != '\0') {
    json_value_t *system_message = turbo_prompt_message_create("system", effective_instructions);
    if (!system_message) {
      free(effective_instructions);
      turbo_free_json(&messages);
      return NULL;
    }
    turbo_json_array_add(messages, system_message);
  }
  free(effective_instructions);

  input = turbo_agent_state_get_array_const(state, "input");
  if (!input) {
    turbo_free_json(&messages);
    return NULL;
  }

  for (i = 0; i < turbo_json_array_size(input); ++i) {
    const json_value_t *message = turbo_json_array_get(input, i);
    if (turbo_agent_request_array_add_clone(messages, message) != 0) {
      turbo_free_json(&messages);
      return NULL;
    }
  }

  events = turbo_agent_state_get_array_const(state, "events");
  if (turbo_agent_request_replay_message_events(events, messages,
                                                turbo_agent_request_append_chat_model_event_message,
                                                turbo_agent_request_append_chat_tool_results_messages) !=
      0) {
    turbo_free_json(&messages);
    return NULL;
  }

  return messages;
}
