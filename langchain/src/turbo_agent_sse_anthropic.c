#include "turbo_agent_sse_state_internal.h"
#include "turbo_agent_sse_json_internal.h"

#include <json_parser.h>
#include "turbo_prompt.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

static int turbo_agent_sse_anthropic_apply_event(
    turbo_agent_sse_anthropic_stream_state_t *state, const json_value_t *event) {
  const char *type;

  if (!state || !event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  type = turbo_json_get_string(event, "type");
  if (!type) {
    return -1;
  }

  if (strcmp(type, "message_start") == 0) {
    const json_value_t *message = turbo_json_object_get(event, "message");
    if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
      return -1;
    }
    if (turbo_agent_sse_set_if_nonempty(&state->id, turbo_json_get_string(message, "id")) != 0 ||
        turbo_agent_sse_set_if_nonempty(&state->role, turbo_json_get_string(message, "role")) !=
            0) {
      return -1;
    }
  } else if (strcmp(type, "content_block_start") == 0) {
    const json_value_t *content_block = turbo_json_object_get(event, "content_block");
    const char *block_type;
    int index;

    if (!content_block || turbo_json_type(content_block) != TURBO_JSON_OBJECT) {
      return -1;
    }

    block_type = turbo_json_get_string(content_block, "type");
    index = turbo_json_get_int(event, "index", 0);
    if (index < 0) {
      return -1;
    }

    if (block_type && strcmp(block_type, "text") == 0) {
      const char *text = turbo_json_get_string(content_block, "text");
      if (text && turbo_agent_sse_append_dynamic_text(&state->content, text) != 0) {
        return -1;
      }
    } else if (block_type && strcmp(block_type, "tool_use") == 0) {
      turbo_agent_sse_tool_call_t *tool_call;
      json_value_t *input = turbo_json_object_get(content_block, "input");
      char *serialized_input = NULL;

      tool_call =
          turbo_agent_sse_tool_call_slot(&state->tool_calls, &state->tool_call_count, (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (turbo_agent_sse_set_if_nonempty(&tool_call->id,
                                          turbo_json_get_string(content_block, "id")) != 0 ||
          turbo_agent_sse_set_if_nonempty(&tool_call->type, "tool_use") != 0 ||
          turbo_agent_sse_set_if_nonempty(&tool_call->name,
                                          turbo_json_get_string(content_block, "name")) != 0) {
        return -1;
      }

      if (input && turbo_json_type(input) == TURBO_JSON_OBJECT && turbo_json_object_size(input) > 0) {
        serialized_input = turbo_json_serialize(input, NULL);
      } else if (input && turbo_json_type(input) != TURBO_JSON_OBJECT) {
        serialized_input = turbo_agent_util_strdup("{}");
      }

      if (serialized_input) {
        if (turbo_agent_sse_replace_text(&tool_call->arguments, serialized_input) != 0) {
          free(serialized_input);
          return -1;
        }
        free(serialized_input);
      }
    }
  } else if (strcmp(type, "content_block_delta") == 0) {
    const json_value_t *delta = turbo_json_object_get(event, "delta");
    const char *delta_type;
    int index;

    if (!delta || turbo_json_type(delta) != TURBO_JSON_OBJECT) {
      return -1;
    }

    delta_type = turbo_json_get_string(delta, "type");
    index = turbo_json_get_int(event, "index", 0);
    if (index < 0) {
      return -1;
    }

    if (delta_type && strcmp(delta_type, "text_delta") == 0) {
      const char *text = turbo_json_get_string(delta, "text");
      if (text && turbo_agent_sse_append_dynamic_text(&state->content, text) != 0) {
        return -1;
      }
    } else if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
      const char *partial_json = turbo_json_get_string(delta, "partial_json");
      turbo_agent_sse_tool_call_t *tool_call;

      tool_call =
          turbo_agent_sse_tool_call_slot(&state->tool_calls, &state->tool_call_count, (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (turbo_agent_sse_set_if_nonempty(&tool_call->type, "tool_use") != 0) {
        return -1;
      }

      if (partial_json &&
          turbo_agent_sse_append_dynamic_text(&tool_call->arguments, partial_json) != 0) {
        return -1;
      }
    }
  } else if (strcmp(type, "message_delta") == 0) {
    const json_value_t *delta = turbo_json_object_get(event, "delta");
    if (!delta || turbo_json_type(delta) != TURBO_JSON_OBJECT) {
      return -1;
    }
    if (turbo_agent_sse_set_if_nonempty(&state->stop_reason,
                                        turbo_json_get_string(delta, "stop_reason")) != 0) {
      return -1;
    }
  }

  return 0;
}

static char *turbo_agent_sse_build_anthropic_response_json(
    const turbo_agent_sse_anthropic_stream_state_t *state) {
  json_value_t *response;
  json_value_t *content;
  size_t i;

  if (!state || !state->id) {
    return NULL;
  }

  response = turbo_agent_sse_anthropic_response_shell_create(state, &content);
  if (!response) {
    return NULL;
  }

  if (state->content && state->content[0] != '\0') {
    json_value_t *text_block = turbo_prompt_content_text_part_create(state->content);
    if (!text_block) {
      turbo_free_json(&response);
      return NULL;
    }
    turbo_json_array_add(content, text_block);
  }

  for (i = 0; i < state->tool_call_count; ++i) {
    const turbo_agent_sse_tool_call_t *tool_call = &state->tool_calls[i];
    json_value_t *tool_use;
    json_value_t *input;

    if (!tool_call->id || !tool_call->name) {
      turbo_free_json(&response);
      return NULL;
    }

    input = turbo_agent_sse_tool_call_input_object(tool_call);
    if (!input) {
      turbo_free_json(&response);
      return NULL;
    }

    tool_use = turbo_prompt_tool_use_part_create(tool_call->id, tool_call->name, input);
    if (!tool_use || !input) {
      turbo_free_json(&tool_use);
      turbo_free_json(&input);
      turbo_free_json(&response);
      return NULL;
    }

    turbo_json_array_add(content, tool_use);
  }

  turbo_json_object_set_string(response, "stop_reason",
                               state->stop_reason ? state->stop_reason : "end_turn");
  return turbo_agent_sse_serialize_json_and_free(response);
}

int turbo_agent_anthropic_messages_sse_to_json(const char *sse_data, size_t sse_len,
                                               char **out_response_json) {
  char *normalized;
  char *cursor;
  turbo_agent_sse_anthropic_stream_state_t state = {0};
  int saw_event = 0;

  if (!sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  normalized = turbo_agent_sse_normalize_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    const char *type;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_agent_sse_collect_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      turbo_agent_sse_free_anthropic_state(&state);
      return -1;
    }

    if (strcmp(data, "[DONE]") == 0) {
      tstr_free(data);
      break;
    }

    if (data[0] != '\0') {
      if (turbo_parse_json((const uint8_t *)data, strlen(data), &event) != 0) {
        tstr_free(data);
        free(normalized);
        turbo_agent_sse_free_anthropic_state(&state);
        return -1;
      }

      type = turbo_json_get_string(event, "type");
      if (type && strcmp(type, "ping") != 0 &&
          turbo_agent_sse_anthropic_apply_event(&state, event) != 0) {
        turbo_free_json(&event);
        tstr_free(data);
        free(normalized);
        turbo_agent_sse_free_anthropic_state(&state);
        return -1;
      }
      if (type && strcmp(type, "ping") != 0) {
        saw_event = 1;
      }
      if (type && strcmp(type, "message_stop") == 0) {
        turbo_free_json(&event);
        tstr_free(data);
        break;
      }
    }

    turbo_free_json(&event);
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (!saw_event) {
    turbo_agent_sse_free_anthropic_state(&state);
    return -1;
  }

  *out_response_json = turbo_agent_sse_build_anthropic_response_json(&state);
  turbo_agent_sse_free_anthropic_state(&state);
  return *out_response_json ? 0 : -1;
}
