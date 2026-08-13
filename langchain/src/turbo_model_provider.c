#include "turbo_model_provider.h"
#include "turbo_agent_util_internal.h"
#include "turbo_agent_sse.h"
#include "turbo_prompt.h"
#include <stdlib.h>
#include <string.h>

static char *turbo_model_provider_strdup(const char *src) {
  return (char *)turbo_agent_util_strdup(src);
}

static int turbo_model_provider_append_text(char **buffer, size_t *length, const char *text) {
  return turbo_agent_util_append_text(buffer, length, text);
}

static int turbo_model_provider_append_bytes(char **buffer, size_t *length, const char *data,
                                             size_t data_len) {
  return turbo_agent_util_append_bytes(buffer, length, data, data_len);
}

typedef struct {
  char *id;
  char *type;
  char *name;
  char *arguments;
} turbo_model_provider_stream_tool_call_t;

typedef struct {
  char *response_id;
  char *output_text;
  turbo_model_provider_stream_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_model_provider_responses_stream_state_t;

typedef struct {
  char *response_id;
  char *output_text;
  turbo_model_provider_stream_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_model_provider_chat_stream_state_t;

typedef struct {
  char *response_id;
  char *output_text;
  turbo_model_provider_stream_tool_call_t *tool_calls;
  size_t tool_call_count;
} turbo_model_provider_anthropic_stream_state_t;





static int turbo_model_provider_append_dynamic_text(char **buffer, const char *text, int *changed) {
  size_t length = 0;

  if (!buffer || !changed) {
    return -1;
  }
  if (!text || text[0] == '\0') {
    return 0;
  }

  if (*buffer) {
    length = strlen(*buffer);
  }
  if (turbo_model_provider_append_bytes(buffer, &length, text, strlen(text)) != 0) {
    return -1;
  }

  *changed = 1;
  return 0;
}

static int turbo_model_provider_replace_text_if_changed(char **target, const char *text,
                                                        int allow_empty, int *changed) {
  char *copy;

  if (!target || !changed) {
    return -1;
  }
  if (!text || (!allow_empty && text[0] == '\0')) {
    return 0;
  }
  if (*target && strcmp(*target, text) == 0) {
    return 0;
  }

  copy = turbo_model_provider_strdup(text);
  if (!copy) {
    return -1;
  }

  free(*target);
  *target = copy;
  *changed = 1;
  return 0;
}

static void turbo_model_provider_free_stream_tool_calls(
    turbo_model_provider_stream_tool_call_t *tool_calls, size_t tool_call_count) {
  size_t i;

  for (i = 0; i < tool_call_count; ++i) {
    tstr_free(tool_calls[i].id);
    tstr_free(tool_calls[i].type);
    tstr_free(tool_calls[i].name);
    tstr_free(tool_calls[i].arguments);
  }
  free(tool_calls);
}

static int turbo_model_provider_stream_tool_call_reserve(
    turbo_model_provider_stream_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  turbo_model_provider_stream_tool_call_t *resized;
  size_t new_count;
  size_t i;

  if (!tool_calls || !tool_call_count) {
    return -1;
  }
  if (index < *tool_call_count) {
    return 0;
  }

  new_count = index + 1;
  resized = (turbo_model_provider_stream_tool_call_t *)realloc(*tool_calls,
                                                               new_count * sizeof(*resized));
  if (!resized) {
    return -1;
  }

  for (i = *tool_call_count; i < new_count; ++i) {
    memset(&resized[i], 0, sizeof(resized[i]));
  }

  *tool_calls = resized;
  *tool_call_count = new_count;
  return 0;
}

static turbo_model_provider_stream_tool_call_t *turbo_model_provider_stream_tool_call_slot(
    turbo_model_provider_stream_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  if (turbo_model_provider_stream_tool_call_reserve(tool_calls, tool_call_count, index) != 0) {
    return NULL;
  }

  return &(*tool_calls)[index];
}

static turbo_model_provider_stream_tool_call_t *turbo_model_provider_find_stream_tool_call(
    turbo_model_provider_stream_tool_call_t *tool_calls, size_t tool_call_count, const char *call_id) {
  size_t i;

  if (!tool_calls || !call_id || call_id[0] == '\0') {
    return NULL;
  }

  for (i = 0; i < tool_call_count; ++i) {
    if (tool_calls[i].id && strcmp(tool_calls[i].id, call_id) == 0) {
      return &tool_calls[i];
    }
  }

  return NULL;
}

static void turbo_model_provider_free_responses_stream_state(
    turbo_model_provider_responses_stream_state_t *state) {
  if (!state) {
    return;
  }

  tstr_free(state->response_id);
  tstr_free(state->output_text);
  turbo_model_provider_free_stream_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

static void turbo_model_provider_free_chat_stream_state(
    turbo_model_provider_chat_stream_state_t *state) {
  if (!state) {
    return;
  }

  tstr_free(state->response_id);
  tstr_free(state->output_text);
  turbo_model_provider_free_stream_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

static void turbo_model_provider_free_anthropic_stream_state(
    turbo_model_provider_anthropic_stream_state_t *state) {
  if (!state) {
    return;
  }

  tstr_free(state->response_id);
  tstr_free(state->output_text);
  turbo_model_provider_free_stream_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

static char *turbo_model_provider_normalize_sse_newlines(const char *data, size_t len) {
  tstr_t normalized;
  size_t i;
  size_t out = 0;

  if (!data && len > 0) {
    return NULL;
  }

  normalized = tstr_new_len(NULL, len);
  if (!normalized) {
    return NULL;
  }

  for (i = 0; i < len; ++i) {
    if (data[i] != '\r') {
      ((char *)normalized)[out++] = data[i];
    }
  }

  ((char *)normalized)[out] = '\0';
  tstr_set_len(normalized, out);
  return (char *)normalized;
}

static int turbo_model_provider_collect_sse_event_data(const char *begin, const char *end,
                                                       char **out_data) {
  const char *cursor;
  char *data = NULL;
  size_t length = 0;

  if (!begin || !end || !out_data || end < begin) {
    return -1;
  }

  *out_data = NULL;
  cursor = begin;
  while (cursor < end) {
    const char *line_end = cursor;
    const char *value;

    while (line_end < end && *line_end != '\n') {
      ++line_end;
    }

    if ((line_end - cursor) >= 5 && strncmp(cursor, "data:", 5) == 0) {
      value = cursor + 5;
      if (value < line_end && *value == ' ') {
        ++value;
      }

      if (length > 0 && turbo_model_provider_append_bytes(&data, &length, "\n", 1) != 0) {
        tstr_free(data);
        return -1;
      }
      if (turbo_model_provider_append_bytes(&data, &length, value, (size_t)(line_end - value)) !=
          0) {
        tstr_free(data);
        return -1;
      }
    }

    cursor = (line_end < end) ? line_end + 1 : end;
  }

  if (!data) {
    data = turbo_model_provider_strdup("");
    if (!data) {
      return -1;
    }
  }

  *out_data = data;
  return 0;
}

static json_value_t *turbo_model_provider_tool_call_record_create(const char *call_id,
                                                                  const char *name,
                                                                  const char *arguments) {
  json_value_t *call;

  if (!call_id || !name || !arguments) {
    return NULL;
  }
  call = turbo_json_create_object();
  if (!call) {
    return NULL;
  }
  turbo_json_object_set_string(call, "call_id", call_id);
  turbo_json_object_set_string(call, "name", name);
  turbo_json_object_set_string(call, "arguments", arguments);
  return call;
}

static char *turbo_model_provider_response_id_copy(const json_value_t *response) {
  const char *id = response && turbo_json_type(response) == TURBO_JSON_OBJECT
                       ? turbo_json_get_string(response, "id")
                       : NULL;
  return turbo_model_provider_strdup(id ? id : "");
}

static char *turbo_model_provider_extract_responses_text(const json_value_t *response) {
  const char *top_level_text;
  const json_value_t *output;
  size_t i;
  char *buffer = NULL;
  size_t length = 0;

  top_level_text = turbo_json_get_string(response, "output_text");
  if (top_level_text) {
    return turbo_model_provider_strdup(top_level_text);
  }

  output = turbo_json_object_get(response, "output");
  if (!output || turbo_json_type(output) != TURBO_JSON_ARRAY) {
    return turbo_model_provider_strdup("");
  }

  for (i = 0; i < turbo_json_array_size(output); ++i) {
    const json_value_t *item = turbo_json_array_get(output, i);
    const json_value_t *content;
    size_t j;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    content = turbo_json_object_get(item, "content");
    if (!content || turbo_json_type(content) != TURBO_JSON_ARRAY) {
      continue;
    }
    for (j = 0; j < turbo_json_array_size(content); ++j) {
      const json_value_t *part = turbo_json_array_get(content, j);
      const char *part_type;
      const char *text;

      if (!part || turbo_json_type(part) != TURBO_JSON_OBJECT) {
        continue;
      }
      part_type = turbo_json_get_string(part, "type");
      if (!part_type || strcmp(part_type, "output_text") != 0) {
        continue;
      }
      text = turbo_json_get_string(part, "text");
      if (text && turbo_model_provider_append_text(&buffer, &length, text) != 0) {
        tstr_free(buffer);
        return NULL;
      }
    }
  }

  return buffer ? buffer : turbo_model_provider_strdup("");
}

static json_value_t *turbo_model_provider_collect_responses_tool_calls(
    const json_value_t *response) {
  const json_value_t *output;
  json_value_t *calls;
  size_t i;

  calls = turbo_json_create_array();
  if (!calls) {
    return NULL;
  }
  output = turbo_json_object_get(response, "output");
  if (!output || turbo_json_type(output) != TURBO_JSON_ARRAY) {
    return calls;
  }
  for (i = 0; i < turbo_json_array_size(output); ++i) {
    const json_value_t *item = turbo_json_array_get(output, i);
    const char *type;
    const char *call_id;
    const char *name;
    const char *arguments;
    json_value_t *call;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    type = turbo_json_get_string(item, "type");
    if (!type || strcmp(type, "function_call") != 0) {
      continue;
    }
    call_id = turbo_json_get_string(item, "call_id");
    name = turbo_json_get_string(item, "name");
    arguments = turbo_json_get_string(item, "arguments");
    if (!call_id || !name || !arguments) {
      continue;
    }
    call = turbo_model_provider_tool_call_record_create(call_id, name, arguments);
    if (!call) {
      turbo_free_json(&calls);
      return NULL;
    }
    turbo_json_array_add(calls, call);
  }
  return calls;
}

static char *turbo_model_provider_extract_chat_text(const json_value_t *message) {
  const char *content_text;
  const json_value_t *content;
  char *buffer = NULL;
  size_t length = 0;
  size_t i;

  if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
    return turbo_model_provider_strdup("");
  }
  content_text = turbo_json_get_string(message, "content");
  if (content_text) {
    return turbo_model_provider_strdup(content_text);
  }
  content = turbo_json_object_get(message, "content");
  if (!content || turbo_json_type(content) != TURBO_JSON_ARRAY) {
    return turbo_model_provider_strdup("");
  }
  for (i = 0; i < turbo_json_array_size(content); ++i) {
    const json_value_t *part = turbo_json_array_get(content, i);
    const char *part_type;
    const char *text;

    if (!part || turbo_json_type(part) != TURBO_JSON_OBJECT) {
      continue;
    }
    part_type = turbo_json_get_string(part, "type");
    if (part_type && strcmp(part_type, "text") != 0 && strcmp(part_type, "output_text") != 0) {
      continue;
    }
    text = turbo_json_get_string(part, "text");
    if (!text) {
      const json_value_t *text_obj = turbo_json_object_get(part, "text");
      if (text_obj && turbo_json_type(text_obj) == TURBO_JSON_OBJECT) {
        text = turbo_json_get_string(text_obj, "value");
      }
    }
    if (text && turbo_model_provider_append_text(&buffer, &length, text) != 0) {
      tstr_free(buffer);
      return NULL;
    }
  }
  return buffer ? buffer : turbo_model_provider_strdup("");
}

static json_value_t *turbo_model_provider_collect_chat_tool_calls(const json_value_t *message) {
  const json_value_t *tool_calls;
  json_value_t *calls;
  size_t i;

  calls = turbo_json_create_array();
  if (!calls) {
    return NULL;
  }
  if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
    return calls;
  }
  tool_calls = turbo_json_object_get(message, "tool_calls");
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    return calls;
  }
  for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
    const json_value_t *tool_call = turbo_json_array_get(tool_calls, i);
    const json_value_t *function_object;
    const char *call_id;
    const char *name;
    const char *arguments;
    json_value_t *call;

    if (!tool_call || turbo_json_type(tool_call) != TURBO_JSON_OBJECT) {
      continue;
    }
    function_object = turbo_json_object_get(tool_call, "function");
    if (!function_object || turbo_json_type(function_object) != TURBO_JSON_OBJECT) {
      continue;
    }
    call_id = turbo_json_get_string(tool_call, "id");
    name = turbo_json_get_string(function_object, "name");
    arguments = turbo_json_get_string(function_object, "arguments");
    if (!call_id || !name || !arguments) {
      continue;
    }
    call = turbo_model_provider_tool_call_record_create(call_id, name, arguments);
    if (!call) {
      turbo_free_json(&calls);
      return NULL;
    }
    turbo_json_array_add(calls, call);
  }
  return calls;
}

static char *turbo_model_provider_extract_anthropic_text(const json_value_t *response) {
  const json_value_t *content;
  size_t i;
  char *buffer = NULL;
  size_t length = 0;

  content = turbo_json_object_get(response, "content");
  if (!content || turbo_json_type(content) != TURBO_JSON_ARRAY) {
    return turbo_model_provider_strdup("");
  }
  for (i = 0; i < turbo_json_array_size(content); ++i) {
    const json_value_t *item = turbo_json_array_get(content, i);
    const char *type;
    const char *text;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    type = turbo_json_get_string(item, "type");
    if (!type || strcmp(type, "text") != 0) {
      continue;
    }
    text = turbo_json_get_string(item, "text");
    if (text && turbo_model_provider_append_text(&buffer, &length, text) != 0) {
      tstr_free(buffer);
      return NULL;
    }
  }
  return buffer ? buffer : turbo_model_provider_strdup("");
}

static json_value_t *turbo_model_provider_collect_anthropic_tool_calls(
    const json_value_t *response) {
  const json_value_t *content;
  json_value_t *calls;
  size_t i;

  calls = turbo_json_create_array();
  if (!calls) {
    return NULL;
  }
  content = turbo_json_object_get(response, "content");
  if (!content || turbo_json_type(content) != TURBO_JSON_ARRAY) {
    return calls;
  }
  for (i = 0; i < turbo_json_array_size(content); ++i) {
    const json_value_t *item = turbo_json_array_get(content, i);
    const json_value_t *input;
    const char *type;
    const char *call_id;
    const char *name;
    char *arguments = NULL;
    json_value_t *call;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    type = turbo_json_get_string(item, "type");
    if (!type || strcmp(type, "tool_use") != 0) {
      continue;
    }
    call_id = turbo_json_get_string(item, "id");
    name = turbo_json_get_string(item, "name");
    input = turbo_json_object_get(item, "input");
    if (!call_id || !name || !input) {
      continue;
    }
    arguments = turbo_json_serialize(input, NULL);
    if (!arguments) {
      turbo_free_json(&calls);
      return NULL;
    }
    call = turbo_model_provider_tool_call_record_create(call_id, name, arguments);
    turbo_json_serialize_free(arguments);
    if (!call) {
      turbo_free_json(&calls);
      return NULL;
    }
    turbo_json_array_add(calls, call);
  }
  return calls;
}

static char *turbo_model_provider_extract_responses_output_item_text(const json_value_t *item) {
  const json_value_t *content;
  char *buffer = NULL;
  size_t length = 0;
  size_t i;

  if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
    return turbo_model_provider_strdup("");
  }

  content = turbo_json_object_get(item, "content");
  if (!content || turbo_json_type(content) != TURBO_JSON_ARRAY) {
    return turbo_model_provider_strdup("");
  }

  for (i = 0; i < turbo_json_array_size(content); ++i) {
    const json_value_t *part = turbo_json_array_get(content, i);
    const char *part_type;
    const char *text;

    if (!part || turbo_json_type(part) != TURBO_JSON_OBJECT) {
      continue;
    }
    part_type = turbo_json_get_string(part, "type");
    if (!part_type || strcmp(part_type, "output_text") != 0) {
      continue;
    }
    text = turbo_json_get_string(part, "text");
    if (text && turbo_model_provider_append_text(&buffer, &length, text) != 0) {
      tstr_free(buffer);
      return NULL;
    }
  }

  return buffer ? buffer : turbo_model_provider_strdup("");
}

static json_value_t *turbo_model_provider_stream_tool_calls_json_value(
    const turbo_model_provider_stream_tool_call_t *tool_calls, size_t tool_call_count) {
  json_value_t *calls;
  size_t i;

  calls = turbo_json_create_array();
  if (!calls) {
    return NULL;
  }

  for (i = 0; i < tool_call_count; ++i) {
    json_value_t *call;
    const turbo_model_provider_stream_tool_call_t *item = &tool_calls[i];

    if (!item->id || !item->name) {
      continue;
    }

    call = turbo_json_create_object();
    if (!call ||
        turbo_runtime_json_object_set(call, "call_id",
                                           turbo_json_create_string(item->id)) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_object_set(call, "name",
                                           turbo_json_create_string(item->name)) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_object_set(
            call, "arguments",
            turbo_json_create_string(item->arguments ? item->arguments : "")) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_array_append(calls, call) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(call);
      turbo_runtime_json_destroy(calls);
      return NULL;
    }
  }

  return calls;
}

static int turbo_model_provider_emit_stream_event_json_value(
    const char *response_id, const char *output_text,
    const turbo_model_provider_stream_tool_call_t *tool_calls, size_t tool_call_count,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  json_value_t *calls_json_value;
  json_value_t *event;

  calls_json_value = turbo_model_provider_stream_tool_calls_json_value(tool_calls, tool_call_count);
  if (!calls_json_value) {
    return -1;
  }

  event = turbo_event_model_create_json_value(response_id ? response_id : "", output_text ? output_text : "",
                                        calls_json_value);
  turbo_runtime_json_destroy(calls_json_value);
  if (!event) {
    return -1;
  }

  event_sink(event, event_sink_user_data);
  turbo_runtime_json_destroy(event);
  return 0;
}

static int turbo_model_provider_apply_responses_output_item(
    turbo_model_provider_responses_stream_state_t *state, const json_value_t *item, int *changed) {
  const char *type;

  if (!state || !item || turbo_json_type(item) != TURBO_JSON_OBJECT || !changed) {
    return -1;
  }

  type = turbo_json_get_string(item, "type");
  if (!type) {
    return 0;
  }

  if (strcmp(type, "message") == 0) {
    char *text = turbo_model_provider_extract_responses_output_item_text(item);
    int rc;

    if (!text) {
      return -1;
    }
    rc = turbo_model_provider_replace_text_if_changed(&state->output_text, text, 1, changed);
    free(text);
    return rc;
  }

  if (strcmp(type, "function_call") == 0) {
    turbo_model_provider_stream_tool_call_t *tool_call;
    const char *call_id = turbo_json_get_string(item, "call_id");
    const char *name = turbo_json_get_string(item, "name");
    const char *arguments = turbo_json_get_string(item, "arguments");

    tool_call = turbo_model_provider_find_stream_tool_call(state->tool_calls, state->tool_call_count,
                                                           call_id);
    if (!tool_call) {
      tool_call = turbo_model_provider_stream_tool_call_slot(
          &state->tool_calls, &state->tool_call_count, state->tool_call_count);
    }
    if (!tool_call) {
      return -1;
    }
    if (turbo_model_provider_replace_text_if_changed(&tool_call->id, call_id, 0, changed) != 0 ||
        turbo_model_provider_replace_text_if_changed(&tool_call->type, type, 0, changed) != 0 ||
        turbo_model_provider_replace_text_if_changed(&tool_call->name, name, 0, changed) != 0 ||
        turbo_model_provider_replace_text_if_changed(&tool_call->arguments, arguments, 1, changed) !=
            0) {
      return -1;
    }
  }

  return 0;
}

static int turbo_model_provider_apply_chat_chunk(turbo_model_provider_chat_stream_state_t *state,
                                                 const json_value_t *chunk, int *changed) {
  const char *id;
  const json_value_t *choices;
  const json_value_t *choice;
  const json_value_t *delta;
  const json_value_t *tool_calls;
  const char *content;
  size_t i;

  if (!state || !chunk || turbo_json_type(chunk) != TURBO_JSON_OBJECT || !changed) {
    return -1;
  }

  id = turbo_json_get_string(chunk, "id");
  {
    int meta_changed = 0;
    if (turbo_model_provider_replace_text_if_changed(&state->response_id, id, 0, &meta_changed) != 0) {
      return -1;
    }
  }

  choices = turbo_json_object_get(chunk, "choices");
  choice = choices && turbo_json_type(choices) == TURBO_JSON_ARRAY && turbo_json_array_size(choices) > 0
               ? turbo_json_array_get(choices, 0)
               : NULL;
  delta = choice && turbo_json_type(choice) == TURBO_JSON_OBJECT
              ? turbo_json_object_get(choice, "delta")
              : NULL;
  if (!delta || turbo_json_type(delta) != TURBO_JSON_OBJECT) {
    return 0;
  }

  content = turbo_json_get_string(delta, "content");
  if (turbo_model_provider_append_dynamic_text(&state->output_text, content, changed) != 0) {
    return -1;
  }

  tool_calls = turbo_json_object_get(delta, "tool_calls");
  if (!tool_calls || turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    return 0;
  }

  for (i = 0; i < turbo_json_array_size(tool_calls); ++i) {
    const json_value_t *tool_call = turbo_json_array_get(tool_calls, i);
    const json_value_t *function_object;
    turbo_model_provider_stream_tool_call_t *stream_tool_call;
    int index;

    if (!tool_call || turbo_json_type(tool_call) != TURBO_JSON_OBJECT) {
      continue;
    }

    index = turbo_json_get_int(tool_call, "index", (int)i);
    if (index < 0) {
      return -1;
    }

    stream_tool_call = turbo_model_provider_stream_tool_call_slot(&state->tool_calls,
                                                                  &state->tool_call_count,
                                                                  (size_t)index);
    if (!stream_tool_call) {
      return -1;
    }

    if (turbo_model_provider_replace_text_if_changed(&stream_tool_call->id,
                                                     turbo_json_get_string(tool_call, "id"), 0,
                                                     changed) != 0 ||
        turbo_model_provider_replace_text_if_changed(&stream_tool_call->type,
                                                     turbo_json_get_string(tool_call, "type"), 0,
                                                     changed) != 0) {
      return -1;
    }

    function_object = turbo_json_object_get(tool_call, "function");
    if (!function_object || turbo_json_type(function_object) != TURBO_JSON_OBJECT) {
      continue;
    }

    if (turbo_model_provider_append_dynamic_text(&stream_tool_call->name,
                                                 turbo_json_get_string(function_object, "name"),
                                                 changed) != 0 ||
        turbo_model_provider_append_dynamic_text(
            &stream_tool_call->arguments, turbo_json_get_string(function_object, "arguments"),
            changed) != 0) {
      return -1;
    }
  }

  return 0;
}

static int turbo_model_provider_apply_anthropic_event(
    turbo_model_provider_anthropic_stream_state_t *state, const json_value_t *event, int *changed) {
  const char *type;

  if (!state || !event || turbo_json_type(event) != TURBO_JSON_OBJECT || !changed) {
    return -1;
  }

  type = turbo_json_get_string(event, "type");
  if (!type) {
    return 0;
  }

  if (strcmp(type, "message_start") == 0) {
    const json_value_t *message = turbo_json_object_get(event, "message");
    int meta_changed = 0;
    if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
      return -1;
    }
    return turbo_model_provider_replace_text_if_changed(&state->response_id,
                                                        turbo_json_get_string(message, "id"), 0,
                                                        &meta_changed);
  }

  if (strcmp(type, "content_block_start") == 0) {
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
      return turbo_model_provider_append_dynamic_text(&state->output_text,
                                                      turbo_json_get_string(content_block, "text"),
                                                      changed);
    }

    if (block_type && strcmp(block_type, "tool_use") == 0) {
      turbo_model_provider_stream_tool_call_t *tool_call;
      const json_value_t *input = turbo_json_object_get(content_block, "input");
      char *serialized_input = NULL;

      tool_call = turbo_model_provider_stream_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                             (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (input && turbo_json_type(input) == TURBO_JSON_OBJECT && turbo_json_object_size(input) > 0) {
        serialized_input = turbo_json_serialize(input, NULL);
        if (!serialized_input) {
          return -1;
        }
      }

      if (turbo_model_provider_replace_text_if_changed(&tool_call->id,
                                                       turbo_json_get_string(content_block, "id"), 0,
                                                       changed) != 0 ||
          turbo_model_provider_replace_text_if_changed(&tool_call->type, "tool_use", 0, changed) !=
              0 ||
          turbo_model_provider_replace_text_if_changed(&tool_call->name,
                                                       turbo_json_get_string(content_block, "name"), 0,
                                                       changed) != 0 ||
          turbo_model_provider_replace_text_if_changed(&tool_call->arguments,
                                                       serialized_input, 1,
                                                       changed) != 0) {
        free(serialized_input);
        return -1;
      }

      free(serialized_input);
    }

    return 0;
  }

  if (strcmp(type, "content_block_delta") == 0) {
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
      return turbo_model_provider_append_dynamic_text(&state->output_text,
                                                      turbo_json_get_string(delta, "text"),
                                                      changed);
    }

    if (delta_type && strcmp(delta_type, "input_json_delta") == 0) {
      turbo_model_provider_stream_tool_call_t *tool_call;

      tool_call = turbo_model_provider_stream_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                             (size_t)index);
      if (!tool_call) {
        return -1;
      }

      if (turbo_model_provider_replace_text_if_changed(&tool_call->type, "tool_use", 0, changed) !=
          0 ||
          turbo_model_provider_append_dynamic_text(&tool_call->arguments,
                                                   turbo_json_get_string(delta, "partial_json"),
                                                   changed) != 0) {
        return -1;
      }
    }
  }

  return 0;
}

static int turbo_model_provider_apply_responses_event(
    turbo_model_provider_responses_stream_state_t *state, const json_value_t *event, int *changed) {
  const char *type;
  const json_value_t *response;
  const json_value_t *item;
  int meta_changed = 0;

  if (!state || !event || turbo_json_type(event) != TURBO_JSON_OBJECT || !changed) {
    return -1;
  }

  type = turbo_json_get_string(event, "type");
  if (!type) {
    return 0;
  }

  response = turbo_json_object_get(event, "response");
  if (response && turbo_json_type(response) == TURBO_JSON_OBJECT &&
      turbo_model_provider_replace_text_if_changed(&state->response_id,
                                                   turbo_json_get_string(response, "id"), 0,
                                                   &meta_changed) != 0) {
    return -1;
  }

  if (strcmp(type, "response.output_text.delta") == 0) {
    return turbo_model_provider_append_dynamic_text(&state->output_text,
                                                    turbo_json_get_string(event, "delta"), changed);
  }

  if (strcmp(type, "response.function_call_arguments.delta") == 0) {
    turbo_model_provider_stream_tool_call_t *tool_call;
    size_t index = state->tool_call_count == 0 ? 0 : state->tool_call_count - 1;

    tool_call = turbo_model_provider_stream_tool_call_slot(&state->tool_calls, &state->tool_call_count,
                                                           index);
    if (!tool_call) {
      return -1;
    }

    return turbo_model_provider_append_dynamic_text(&tool_call->arguments,
                                                    turbo_json_get_string(event, "delta"), changed);
  }

  item = turbo_json_object_get(event, "item");
  if (item && turbo_json_type(item) == TURBO_JSON_OBJECT &&
      (strcmp(type, "response.output_item.added") == 0 ||
       strcmp(type, "response.output_item.done") == 0)) {
    return turbo_model_provider_apply_responses_output_item(state, item, changed);
  }

  return 0;
}

static json_value_t *turbo_model_provider_build_model_event_json_value(
    const char *response_id, char *output_text, json_value_t *tool_calls) {
  json_value_t *tool_calls_json_value;
  json_value_t *event_json_value;

  if (!output_text || !tool_calls) {
    tstr_free(output_text);
    turbo_free_json(&tool_calls);
    return NULL;
  }
  tool_calls_json_value = turbo_json_clone(tool_calls);
  if (!tool_calls_json_value) {
    tstr_free(output_text);
    turbo_free_json(&tool_calls);
    return NULL;
  }
  event_json_value =
  turbo_event_model_create_json_value(response_id ? response_id : "", output_text, tool_calls_json_value);
  turbo_runtime_json_destroy(tool_calls_json_value);
  turbo_free_json(&tool_calls);
  tstr_free(output_text);
  return event_json_value;
}

static json_value_t *turbo_model_provider_build_model_event(const char *response_id,
                                                            char *output_text,
                                                            json_value_t *tool_calls) {
  json_value_t *event_json_value;
  json_value_t *event;

  event_json_value = turbo_model_provider_build_model_event_json_value(response_id, output_text, tool_calls);
  if (!event_json_value) {
    return NULL;
  }

  event = turbo_json_clone(event_json_value);
  turbo_runtime_json_destroy(event_json_value);
  return event;
}

static int turbo_model_provider_sse_to_response_json(const turbo_model_provider_t *provider,
                                                     const char *sse_data, size_t sse_len,
                                                     char **out_response_json) {
  if (!provider || !sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;

  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_agent_responses_sse_to_json(sse_data, sse_len, out_response_json);
  }

  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_agent_chat_sse_to_json(sse_data, sse_len, out_response_json);
  }

  if (turbo_model_provider_is_anthropic_messages(provider)) {
    return turbo_agent_anthropic_messages_sse_to_json(sse_data, sse_len, out_response_json);
  }

  return -1;
}

const turbo_model_provider_t *turbo_model_provider_by_name(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }

  if (strcmp(name, "openai") == 0 || strcmp(name, "openai_responses") == 0 ||
      strcmp(name, "responses") == 0) {
    return turbo_model_provider_openai_responses();
  }

  if (strcmp(name, "openai_chat_completions") == 0 || strcmp(name, "chat_completions") == 0) {
    return turbo_model_provider_openai_chat_completions();
  }

  if (strcmp(name, "openai_compatible_chat_completions") == 0 ||
      strcmp(name, "openai_compatible_chat") == 0) {
    return turbo_model_provider_openai_compatible_chat_completions();
  }

  if (strcmp(name, "anthropic_messages") == 0 || strcmp(name, "anthropic") == 0) {
    return turbo_model_provider_anthropic_messages();
  }

  return NULL;
}

int turbo_model_provider_is_legacy_chat(const turbo_model_provider_t *provider) {
  return provider == turbo_model_provider_openai_chat_completions() ||
                 provider == turbo_model_provider_openai_compatible_chat_completions()
             ? 1
             : 0;
}

int turbo_model_provider_is_anthropic_messages(const turbo_model_provider_t *provider) {
  return provider == turbo_model_provider_anthropic_messages() ? 1 : 0;
}

const char *turbo_model_provider_select_api_key(const turbo_model_provider_t *provider,
                                                const char *openai_api_key,
                                                const char *anthropic_auth_token) {
  if (turbo_model_provider_is_anthropic_messages(provider) && anthropic_auth_token &&
      anthropic_auth_token[0] != '\0') {
    return anthropic_auth_token;
  }

  return openai_api_key;
}

const char *turbo_model_provider_select_base_url(const turbo_model_provider_t *provider,
                                                 const char *openai_base_url,
                                                 const char *anthropic_base_url,
                                                 const char *default_openai_base_url) {
  if (turbo_model_provider_is_anthropic_messages(provider) && anthropic_base_url &&
      anthropic_base_url[0] != '\0') {
    return anthropic_base_url;
  }

  if (openai_base_url && openai_base_url[0] != '\0') {
    return openai_base_url;
  }

  return default_openai_base_url;
}

const char *turbo_model_provider_select_endpoint_path(
    const turbo_model_provider_t *provider, const char *endpoint_path_override) {
  if (endpoint_path_override && endpoint_path_override[0] != '\0') {
    return endpoint_path_override;
  }

  return provider ? provider->default_endpoint_path : NULL;
}

json_value_t *turbo_model_provider_messages_to_wire_json(
    const turbo_model_provider_t *provider, const json_value_t *messages,
    char **out_system) {
  if (out_system) {
    *out_system = NULL;
  }

  if (!provider || !messages) {
    return NULL;
  }

  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_prompt_messages_to_openai_responses_json(messages);
  }

  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_prompt_messages_to_openai_chat_json(messages);
  }

  if (turbo_model_provider_is_anthropic_messages(provider)) {
    json_value_t *json_messages = NULL;

    if (turbo_prompt_messages_to_anthropic_json(messages, &json_messages, out_system) !=
        TURBO_PROMPT_OK) {
      return NULL;
    }
    return json_messages;
  }

  return NULL;
}

json_value_t *turbo_model_provider_response_to_event_json(
    const turbo_model_provider_t *provider, const json_value_t *response) {
  char *response_id;
  char *output_text;
  json_value_t *tool_calls;
  const json_value_t *choices;
  const json_value_t *choice;
  const json_value_t *message;

  if (!provider || !response || turbo_json_type(response) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  response_id = turbo_model_provider_response_id_copy(response);
  if (!response_id) {
    return NULL;
  }

  if (provider == turbo_model_provider_openai_responses()) {
    output_text = turbo_model_provider_extract_responses_text(response);
    tool_calls = turbo_model_provider_collect_responses_tool_calls(response);
  } else if (turbo_model_provider_is_legacy_chat(provider)) {
    choices = turbo_json_object_get(response, "choices");
    choice = choices && turbo_json_type(choices) == TURBO_JSON_ARRAY && turbo_json_array_size(choices) > 0
                 ? turbo_json_array_get(choices, 0)
                 : NULL;
    message = choice && turbo_json_type(choice) == TURBO_JSON_OBJECT
                  ? turbo_json_object_get(choice, "message")
                  : NULL;
    output_text = turbo_model_provider_extract_chat_text(message);
    tool_calls = turbo_model_provider_collect_chat_tool_calls(message);
  } else if (turbo_model_provider_is_anthropic_messages(provider)) {
    output_text = turbo_model_provider_extract_anthropic_text(response);
    tool_calls = turbo_model_provider_collect_anthropic_tool_calls(response);
  } else {
    tstr_free(response_id);
    return NULL;
  }

  {
    json_value_t *event = turbo_model_provider_build_model_event(response_id, output_text, tool_calls);
    tstr_free(response_id);
    return event;
  }
}

json_value_t *turbo_model_provider_sse_to_event_json(
    const turbo_model_provider_t *provider, const char *sse_data, size_t sse_len) {
  char *response_json = NULL;
  json_value_t *response = NULL;
  json_value_t *event = NULL;

  if (!provider || !sse_data) {
    return NULL;
  }

  if (turbo_model_provider_sse_to_response_json(provider, sse_data, sse_len, &response_json) != 0 ||
      !response_json) {
    turbo_json_serialize_free(response_json);
    return NULL;
  }

  if (turbo_parse_json((const uint8_t *)response_json, strlen(response_json), &response) != 0 ||
      !response) {
    turbo_json_serialize_free(response_json);
    turbo_free_json(&response);
    return NULL;
  }

  event = turbo_model_provider_response_to_event_json(provider, response);
  turbo_json_serialize_free(response_json);
  turbo_free_json(&response);
  return event;
}

json_value_t *turbo_model_provider_response_to_event_json_value(
    const turbo_model_provider_t *provider, const json_value_t *response) {
  char *response_id;
  char *output_text;
  json_value_t *tool_calls;
  const json_value_t *choices;
  const json_value_t *choice;
  const json_value_t *message;
  json_value_t *event_json_value;

  if (!provider || !response || turbo_json_type(response) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  response_id = turbo_model_provider_response_id_copy(response);
  if (!response_id) {
    return NULL;
  }

  if (provider == turbo_model_provider_openai_responses()) {
    output_text = turbo_model_provider_extract_responses_text(response);
    tool_calls = turbo_model_provider_collect_responses_tool_calls(response);
  } else if (turbo_model_provider_is_legacy_chat(provider)) {
    choices = turbo_json_object_get(response, "choices");
    choice = choices && turbo_json_type(choices) == TURBO_JSON_ARRAY && turbo_json_array_size(choices) > 0
                 ? turbo_json_array_get(choices, 0)
                 : NULL;
    message = choice && turbo_json_type(choice) == TURBO_JSON_OBJECT
                  ? turbo_json_object_get(choice, "message")
                  : NULL;
    output_text = turbo_model_provider_extract_chat_text(message);
    tool_calls = turbo_model_provider_collect_chat_tool_calls(message);
  } else if (turbo_model_provider_is_anthropic_messages(provider)) {
    output_text = turbo_model_provider_extract_anthropic_text(response);
    tool_calls = turbo_model_provider_collect_anthropic_tool_calls(response);
  } else {
    tstr_free(response_id);
    return NULL;
  }

  event_json_value = turbo_model_provider_build_model_event_json_value(response_id, output_text, tool_calls);
  tstr_free(response_id);
  if (!event_json_value || turbo_event_model_validate_json_value(event_json_value) != 0) {
    turbo_runtime_json_destroy(event_json_value);
    return NULL;
  }

  return event_json_value;
}

int turbo_model_provider_response_emit_json_value(const turbo_model_provider_t *provider,
                                            const json_value_t *response,
                                            turbo_event_sink_json_value_fn event_sink,
                                            void *event_sink_user_data) {
  json_value_t *event;

  if (!provider || !response || !event_sink) {
    return -1;
  }

  event = turbo_model_provider_response_to_event_json_value(provider, response);
  if (!event) {
    return -1;
  }

  event_sink(event, event_sink_user_data);
  turbo_runtime_json_destroy(event);
  return 0;
}

json_value_t *turbo_model_provider_sse_to_event_json_value(
    const turbo_model_provider_t *provider, const char *sse_data, size_t sse_len) {
  char *response_json = NULL;
  json_value_t *response = NULL;
  json_value_t *event_json_value = NULL;

  if (!provider || !sse_data) {
    return NULL;
  }

  if (turbo_model_provider_sse_to_response_json(provider, sse_data, sse_len, &response_json) != 0 ||
      !response_json) {
    turbo_json_serialize_free(response_json);
    return NULL;
  }

  if (turbo_parse_json((const uint8_t *)response_json, strlen(response_json), &response) != 0 ||
      !response) {
    turbo_json_serialize_free(response_json);
    turbo_free_json(&response);
    return NULL;
  }

  event_json_value = turbo_model_provider_response_to_event_json_value(provider, response);
  turbo_json_serialize_free(response_json);
  turbo_free_json(&response);
  return event_json_value;
}

static int turbo_model_provider_chat_sse_emit_json_value(const char *sse_data, size_t sse_len,
                                                   turbo_event_sink_json_value_fn event_sink,
                                                   void *event_sink_user_data) {
  char *normalized;
  char *cursor;
  turbo_model_provider_chat_stream_state_t state = {0};

  normalized = turbo_model_provider_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *chunk = NULL;
    int changed = 0;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }
    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_model_provider_collect_sse_event_data(cursor, event_end, &data) != 0) {
      tstr_free(normalized);
      turbo_model_provider_free_chat_stream_state(&state);
      return -1;
    }
    if (strcmp(data, "[DONE]") == 0) {
      tstr_free(data);
      break;
    }

    if (data[0] != '\0' &&
        turbo_parse_json((const uint8_t *)data, strlen(data), &chunk) == 0 && chunk) {
      if (turbo_model_provider_apply_chat_chunk(&state, chunk, &changed) != 0) {
        turbo_free_json(&chunk);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_chat_stream_state(&state);
        return -1;
      }
      if (changed &&
          turbo_model_provider_emit_stream_event_json_value(state.response_id, state.output_text,
                                                      state.tool_calls, state.tool_call_count,
                                                      event_sink, event_sink_user_data) != 0) {
        turbo_free_json(&chunk);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_chat_stream_state(&state);
        return -1;
      }
    }

    turbo_free_json(&chunk);
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  tstr_free(normalized);
  turbo_model_provider_free_chat_stream_state(&state);
  return 0;
}

static int turbo_model_provider_responses_sse_emit_json_value(const char *sse_data, size_t sse_len,
                                                        turbo_event_sink_json_value_fn event_sink,
                                                        void *event_sink_user_data) {
  char *normalized;
  char *cursor;
  turbo_model_provider_responses_stream_state_t state = {0};

  normalized = turbo_model_provider_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    int changed = 0;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }
    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_model_provider_collect_sse_event_data(cursor, event_end, &data) != 0) {
      tstr_free(normalized);
      turbo_model_provider_free_responses_stream_state(&state);
      return -1;
    }
    if (strcmp(data, "[DONE]") == 0) {
      tstr_free(data);
      break;
    }

    if (data[0] != '\0' &&
        turbo_parse_json((const uint8_t *)data, strlen(data), &event) == 0 && event) {
      if (turbo_model_provider_apply_responses_event(&state, event, &changed) != 0) {
        turbo_free_json(&event);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_responses_stream_state(&state);
        return -1;
      }
      if (changed &&
          turbo_model_provider_emit_stream_event_json_value(state.response_id, state.output_text,
                                                      state.tool_calls, state.tool_call_count,
                                                      event_sink, event_sink_user_data) != 0) {
        turbo_free_json(&event);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_responses_stream_state(&state);
        return -1;
      }
    }

    turbo_free_json(&event);
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  tstr_free(normalized);
  turbo_model_provider_free_responses_stream_state(&state);
  return 0;
}

static int turbo_model_provider_anthropic_sse_emit_json_value(const char *sse_data, size_t sse_len,
                                                        turbo_event_sink_json_value_fn event_sink,
                                                        void *event_sink_user_data) {
  char *normalized;
  char *cursor;
  turbo_model_provider_anthropic_stream_state_t state = {0};

  normalized = turbo_model_provider_normalize_sse_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    int changed = 0;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }
    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_model_provider_collect_sse_event_data(cursor, event_end, &data) != 0) {
      tstr_free(normalized);
      turbo_model_provider_free_anthropic_stream_state(&state);
      return -1;
    }
    if (strcmp(data, "[DONE]") == 0) {
      tstr_free(data);
      break;
    }

    if (data[0] != '\0' &&
        turbo_parse_json((const uint8_t *)data, strlen(data), &event) == 0 && event) {
      if (turbo_model_provider_apply_anthropic_event(&state, event, &changed) != 0) {
        turbo_free_json(&event);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_anthropic_stream_state(&state);
        return -1;
      }
      if (changed &&
          turbo_model_provider_emit_stream_event_json_value(state.response_id, state.output_text,
                                                      state.tool_calls, state.tool_call_count,
                                                      event_sink, event_sink_user_data) != 0) {
        turbo_free_json(&event);
        tstr_free(data);
        tstr_free(normalized);
        turbo_model_provider_free_anthropic_stream_state(&state);
        return -1;
      }
    }

    turbo_free_json(&event);
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  tstr_free(normalized);
  turbo_model_provider_free_anthropic_stream_state(&state);
  return 0;
}

int turbo_model_provider_sse_emit_json_value(const turbo_model_provider_t *provider,
                                       const char *sse_data, size_t sse_len,
                                       turbo_event_sink_json_value_fn event_sink,
                                       void *event_sink_user_data) {
  if (!provider || !sse_data || !event_sink) {
    return -1;
  }

  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_model_provider_responses_sse_emit_json_value(sse_data, sse_len, event_sink,
                                                        event_sink_user_data);
  }
  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_model_provider_chat_sse_emit_json_value(sse_data, sse_len, event_sink,
                                                   event_sink_user_data);
  }
  if (turbo_model_provider_is_anthropic_messages(provider)) {
    return turbo_model_provider_anthropic_sse_emit_json_value(sse_data, sse_len, event_sink,
                                                        event_sink_user_data);
  }

  return -1;
}
