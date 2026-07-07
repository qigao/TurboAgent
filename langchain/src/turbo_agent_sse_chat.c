#include "turbo_agent_sse_state_internal.h"
#include "turbo_agent_sse_json_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_parser.h"
#include "turbo_prompt.h"

#include <stdlib.h>
#include <string.h>

static int turbo_agent_sse_chat_apply_chunk(turbo_agent_sse_chat_stream_state_t *state,
                                            const json_value_t *chunk) {
  const char *id;
  const json_value_t *choices;
  const json_value_t *choice;
  const json_value_t *delta;
  const json_value_t *tool_calls;
  const char *role;
  const char *content;
  const char *finish_reason;
  size_t i;

  if (!state || !chunk || turbo_json_type(chunk) != TURBO_JSON_OBJECT) {
    return -1;
  }

  id = turbo_json_get_string(chunk, "id");
  if (turbo_agent_sse_set_if_nonempty(&state->id, id) != 0) {
    return -1;
  }

  choices = turbo_json_object_get(chunk, "choices");
  choice = choices && turbo_json_type(choices) == TURBO_JSON_ARRAY &&
                   turbo_json_array_size(choices) > 0
               ? turbo_json_array_get(choices, 0)
               : NULL;
  if (!choice || turbo_json_type(choice) != TURBO_JSON_OBJECT) {
    return -1;
  }

  delta = turbo_json_object_get(choice, "delta");
  if (delta && turbo_json_type(delta) == TURBO_JSON_OBJECT) {
    role = turbo_json_get_string(delta, "role");
    if (turbo_agent_sse_set_if_nonempty(&state->role, role) != 0) {
      return -1;
    }

    content = turbo_json_get_string(delta, "content");
    if (content && turbo_agent_sse_append_dynamic_text(&state->content, content) != 0) {
      return -1;
    }

    tool_calls = turbo_json_object_get(delta, "tool_calls");
    if (tool_calls && turbo_json_type(tool_calls) == TURBO_JSON_ARRAY) {
      for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
        const json_value_t *tool_call_json = turbo_json_array_get(tool_calls, i);
        turbo_agent_sse_tool_call_t *tool_call;
        const json_value_t *function_json;
        int index;

        if (!tool_call_json || turbo_json_type(tool_call_json) != TURBO_JSON_OBJECT) {
          return -1;
        }

        index = turbo_json_get_int(tool_call_json, "index", (int)i);
        if (index < 0) {
          return -1;
        }

        tool_call = turbo_agent_sse_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                   (size_t)index);
        if (!tool_call) {
          return -1;
        }

        function_json = turbo_json_object_get(tool_call_json, "function");
        if (turbo_agent_sse_set_if_nonempty(&tool_call->id,
                                            turbo_json_get_string(tool_call_json, "id")) != 0 ||
            turbo_agent_sse_set_if_nonempty(
                &tool_call->type, turbo_json_get_string(tool_call_json, "type")) != 0) {
          return -1;
        }

        if (function_json && turbo_json_type(function_json) == TURBO_JSON_OBJECT) {
          if (turbo_agent_sse_set_if_nonempty(&tool_call->name,
                                              turbo_json_get_string(function_json, "name")) != 0) {
            return -1;
          }

          if (turbo_agent_sse_append_dynamic_text(
                  &tool_call->arguments,
                  turbo_json_get_string(function_json, "arguments")
                      ? turbo_json_get_string(function_json, "arguments")
                      : "") != 0) {
            return -1;
          }
        }
      }
    }
  }

  finish_reason = turbo_json_get_string(choice, "finish_reason");
  if (turbo_agent_sse_set_if_nonempty(&state->finish_reason, finish_reason) != 0) {
    return -1;
  }

  return 0;
}

static char *turbo_agent_sse_build_chat_response_json(
    const turbo_agent_sse_chat_stream_state_t *state) {
  json_value_t *response;
  json_value_t *choice;
  json_value_t *message;
  json_value_t *tool_calls = NULL;

  if (!state || !state->id) {
    return NULL;
  }

  response = turbo_agent_sse_chat_response_shell_create(state, &choice, &message);
  if (!response) {
    return NULL;
  }

  tool_calls = turbo_agent_sse_build_chat_tool_calls_array(state);
  if (state->tool_call_count > 0 && !tool_calls) {
    turbo_free_json(&response);
    return NULL;
  }
  if (tool_calls) {
    turbo_json_object_add(message, "tool_calls", tool_calls);
  }

  turbo_json_object_set_string(choice, "finish_reason",
                               state->finish_reason ? state->finish_reason
                                                    : (state->tool_call_count > 0 ? "tool_calls"
                                                                                  : "stop"));
  return turbo_agent_sse_serialize_json_and_free(response);
}

int turbo_agent_chat_sse_to_json(const char *sse_data, size_t sse_len,
                                 char **out_response_json) {
  char *normalized;
  char *cursor;
  turbo_agent_sse_chat_stream_state_t state = {0};
  int saw_chunk = 0;

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
    json_value_t *chunk = NULL;
    int is_done = 0;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_agent_sse_collect_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      turbo_agent_sse_free_chat_state(&state);
      return -1;
    }

    if (strcmp(data, "[DONE]") == 0) {
      is_done = 1;
    } else if (data[0] != '\0') {
      if (turbo_parse_json((const uint8_t *)data, strlen(data), &chunk) != 0 ||
          turbo_agent_sse_chat_apply_chunk(&state, chunk) != 0) {
        tstr_free(data);
        turbo_free_json(&chunk);
        free(normalized);
        turbo_agent_sse_free_chat_state(&state);
        return -1;
      }
      saw_chunk = 1;
    }

    tstr_free(data);
    turbo_free_json(&chunk);

    if (is_done) {
      break;
    }

    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (!saw_chunk) {
    turbo_agent_sse_free_chat_state(&state);
    return -1;
  }

  *out_response_json = turbo_agent_sse_build_chat_response_json(&state);
  turbo_agent_sse_free_chat_state(&state);
  return *out_response_json ? 0 : -1;
}
