#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_event_internal.h"
#include "turbo_agent_request_common_internal.h"
#include "turbo_agent_request_messages_internal.h"
#include "turbo_agent_request_text_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_memory_internal.h"

#include "turbo_prompt.h"

#include <tstr.h>

#include <stdlib.h>
#include <string.h>

typedef int (*turbo_agent_request_message_event_append_fn)(json_value_t *messages,
                                                           const json_value_t *event);

/* Consume a newly created message, including when insertion fails. */
static int turbo_agent_request_array_take(json_value_t *array, json_value_t *value) {
  if (turbo_runtime_json_array_append(array, value) == TURBO_RUNTIME_JSON_OK) return 0;
  json_free(value);
  return -1;
}

static int turbo_agent_request_array_add_clone(json_value_t *array, const json_value_t *value) {
  json_value_t *clone = NULL;

  if (!array || !value) {
    return -1;
  }

  if (turbo_agent_clone_json(value, &clone) != TURBO_GRAPH_EXEC_OK) {
    return -1;
  }

  return turbo_agent_request_array_take(array, clone);
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

  if (!output_item || json_type(output_item) != JSON_OBJECT) {
    return 0;
  }

  call_id_value = json_get_string(output_item, "call_id");
  output_value = json_get_string(output_item, "output");
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
  return messages && event && json_type(event) == JSON_OBJECT ? 1 : 0;
}

static const json_value_t *
turbo_agent_request_tool_results_event_outputs(const json_value_t *event) {
  const json_value_t *outputs;

  if (!turbo_agent_event_kind_is(event, "tool_results")) {
    return NULL;
  }

  outputs = json_object_get(event, "outputs");
  return outputs && json_type(outputs) == JSON_ARRAY ? outputs : NULL;
}

static int turbo_agent_request_append_chat_model_event_message(json_value_t *messages,
                                                               const json_value_t *event) {
  const json_value_t *tool_calls = json_object_get(event, "tool_calls");
  const json_value_t *text = json_object_get(event, "output_text");
  const char *output_text = turbo_agent_event_output_text(event);
  size_t tool_call_count;

  if (!turbo_agent_request_message_event_inputs_valid(event, messages)) {
    return -1;
  }
  /* Optional fields may be absent; malformed fields must not become empty
   * history through the tolerant state inspection accessors. */
  if ((tool_calls && json_type(tool_calls) != JSON_ARRAY) ||
      (text && json_type(text) != JSON_STRING && json_type(text) != JSON_NULL)) {
    return -1;
  }
  tool_call_count = tool_calls ? json_array_size(tool_calls) : 0;

  if (tool_call_count > 0) {
    json_value_t *assistant_tool_calls = json_create_array();
    json_value_t *assistant = NULL;
    size_t j;

    if (!assistant_tool_calls) {
      json_free(assistant_tool_calls); assistant_tool_calls = NULL;
      return -1;
    }

    for (j = 0; j < tool_call_count; ++j) {
      const json_value_t *tool_call = json_array_get(tool_calls, j);
      const char *call_id;
      const char *name;
      const char *arguments;
      json_value_t *chat_tool_call;

      if (!turbo_agent_tool_call_record_fields(tool_call, &call_id, &name, &arguments)) {
        json_free(assistant); assistant = NULL;
        json_free(assistant_tool_calls); assistant_tool_calls = NULL;
        return -1;
      }

      chat_tool_call = turbo_prompt_chat_tool_call_create_json_value(call_id, "function", name,
                                                                     arguments);
      if (!chat_tool_call) {
        json_free(assistant); assistant = NULL;
        json_free(assistant_tool_calls); assistant_tool_calls = NULL;
        return -1;
      }
      if (turbo_agent_request_array_take(assistant_tool_calls, chat_tool_call) != 0) {
        json_free(assistant_tool_calls);
        return -1;
      }
    }

    assistant = turbo_prompt_chat_assistant_message_create_json_value(output_text, assistant_tool_calls);
    if (!assistant) {
      return -1;
    }

    return turbo_agent_request_array_take(messages, assistant);
  }

  if (output_text) {
    json_value_t *assistant = turbo_prompt_message_create_json_value("assistant", output_text);
    if (!assistant) {
      return -1;
    }
    return turbo_agent_request_array_take(messages, assistant);
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

  for (j = 0; j < json_array_size(outputs); ++j) {
    const json_value_t *output = json_array_get(outputs, j);
    const char *call_id;
    const char *content;
    json_value_t *tool_message;

    if (!turbo_agent_request_tool_result_output_fields(output, &call_id, &content)) {
      return -1;
    }

    tool_message = turbo_prompt_chat_tool_message_create_json_value(call_id, content);
    if (!tool_message) {
      return -1;
    }
    if (turbo_agent_request_array_take(messages, tool_message) != 0) return -1;
  }

  return 0;
}

static int turbo_agent_request_replay_message_events(
    const json_value_t *events, size_t start, json_value_t *messages,
    turbo_agent_request_message_event_append_fn append_model_event,
    turbo_agent_request_message_event_append_fn append_tool_results_event) {
  size_t i;

  if (!messages) {
    return -1;
  }

  if (!events) return start == 0 ? 0 : -1;
  if (json_type(events) != JSON_ARRAY) return -1;

  if (start > json_array_size(events)) {
    return -1;
  }

  for (i = start; i < json_array_size(events); ++i) {
    const json_value_t *event = json_array_get(events, i);
    const json_value_t *kind;
    if (!event || json_type(event) != JSON_OBJECT) return -1;
    kind = json_object_get(event, "kind");
    if (!kind || json_type(kind) != JSON_STRING) return -1;
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

static json_value_t *turbo_agent_request_project_messages(
    const turbo_agent_t *agent, const json_value_t *state, int include_instructions,
    int apply_context_projection) {
  const json_value_t *input;
  const json_value_t *events;
  json_value_t *messages;
  char *effective_instructions;
  size_t i;
  int context_active;

  if (!state || ((include_instructions || apply_context_projection) && !agent)) {
    return NULL;
  }
  context_active = apply_context_projection && agent->context_projection_active;

  messages = turbo_prompt_messages_create();
  if (!messages) {
    return NULL;
  }

  effective_instructions = NULL;
  if (include_instructions) {
    effective_instructions = turbo_agent_build_effective_instructions(agent, state);
    if ((agent->instructions || turbo_agent_state_memory_layer_count(state) > 0) &&
        !effective_instructions) {
      json_free(messages); messages = NULL;
      return NULL;
    }
  }

  if (effective_instructions && effective_instructions[0] != '\0') {
    json_value_t *system_message = turbo_prompt_message_create("system", effective_instructions);
    if (!system_message) {
      free(effective_instructions);
      json_free(messages); messages = NULL;
      return NULL;
    }
    if (turbo_agent_request_array_take(messages, system_message) != 0) {
      free(effective_instructions);
      json_free(messages);
      return NULL;
    }
  }
  free(effective_instructions);

  if (context_active) {
    char *summary_json;
    tstr summary_text;
    json_value_t *summary_message;

    if (!agent->context_summary) {
      json_free(messages); messages = NULL;
      return NULL;
    }
    summary_json = json_serialize(agent->context_summary, NULL);
    if (!summary_json) {
      json_free(messages); messages = NULL;
      return NULL;
    }
    summary_text = tstr_dup("Committed conversation summary:\n");
    if (summary_text) {
      tstr grown = tstr_cat(summary_text, summary_json);
      if (!grown) tstr_free(summary_text);
      summary_text = grown;
    }
    json_serialize_free(summary_json);
    if (!summary_text) {
      json_free(messages); messages = NULL;
      return NULL;
    }
    summary_message = turbo_prompt_message_create("system", summary_text);
    tstr_free(summary_text);
    if (!summary_message) {
      json_free(messages); messages = NULL;
      return NULL;
    }
    if (turbo_agent_request_array_take(messages, summary_message) != 0) {
      json_free(messages);
      return NULL;
    }
  }

  input = turbo_agent_state_get_array_const(state, "input");
  if (!input) {
    json_free(messages); messages = NULL;
    return NULL;
  }

  for (i = 0; i < json_array_size(input); ++i) {
    const json_value_t *message = json_array_get(input, i);
    if (turbo_agent_request_array_add_clone(messages, message) != 0) {
      json_free(messages); messages = NULL;
      return NULL;
    }
  }

  events = json_object_get(state, "events");
  if (turbo_agent_request_replay_message_events(
          events, context_active ? agent->context_event_start : 0, messages,
          turbo_agent_request_append_chat_model_event_message,
          turbo_agent_request_append_chat_tool_results_messages) != 0) {
    json_free(messages); messages = NULL;
    return NULL;
  }

  return messages;
}

CXX_C_API json_value_t *turbo_agent_request_build_history_messages(const json_value_t *state) {
  return turbo_agent_request_project_messages(NULL, state, 0, 0);
}

CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages_with_options(
    const turbo_agent_t *agent, const json_value_t *state, int include_instructions) {
  return turbo_agent_request_project_messages(agent, state, include_instructions, 1);
}

CXX_C_API json_value_t *turbo_agent_request_build_canonical_messages(const turbo_agent_t *agent,
                                                                     const json_value_t *state) {
  return turbo_agent_request_build_canonical_messages_with_options(agent, state, 1);
}
