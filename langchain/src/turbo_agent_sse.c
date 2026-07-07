#include "turbo_agent_sse_state_internal.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

int turbo_agent_sse_replace_text(char **target, const char *text) {
  char *copy;

  if (!target || !text) {
    return -1;
  }

  copy = turbo_agent_util_strdup(text);
  if (!copy) {
    return -1;
  }

  tstr_free(*target);
  *target = copy;
  return 0;
}

int turbo_agent_sse_set_if_nonempty(char **target, const char *text) {
  if (!text || text[0] == '\0') {
    return 0;
  }

  return turbo_agent_sse_replace_text(target, text);
}

int turbo_agent_sse_append_dynamic_text(char **buffer, const char *text) {
  size_t length = 0;

  if (!buffer || !text) {
    return -1;
  }

  if (*buffer) {
    length = strlen(*buffer);
  }

  return turbo_agent_util_append_text(buffer, &length, text);
}

void turbo_agent_sse_free_tool_calls(turbo_agent_sse_tool_call_t *tool_calls,
                                     size_t tool_call_count) {
  size_t i;

  for (i = 0; i < tool_call_count; ++i) {
    tstr_free(tool_calls[i].id);
    tstr_free(tool_calls[i].type);
    tstr_free(tool_calls[i].name);
    tstr_free(tool_calls[i].arguments);
  }
  free(tool_calls);
}

static int turbo_agent_sse_ensure_tool_call_capacity(
    turbo_agent_sse_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  turbo_agent_sse_tool_call_t *next_tool_calls;
  size_t new_count;
  size_t i;

  if (!tool_calls || !tool_call_count) {
    return -1;
  }

  if (index < *tool_call_count) {
    return 0;
  }

  new_count = index + 1;
  next_tool_calls =
      (turbo_agent_sse_tool_call_t *)realloc(*tool_calls, new_count * sizeof(*next_tool_calls));
  if (!next_tool_calls) {
    return -1;
  }

  for (i = *tool_call_count; i < new_count; ++i) {
    memset(&next_tool_calls[i], 0, sizeof(next_tool_calls[i]));
  }

  *tool_calls = next_tool_calls;
  *tool_call_count = new_count;
  return 0;
}

turbo_agent_sse_tool_call_t *turbo_agent_sse_tool_call_slot(
    turbo_agent_sse_tool_call_t **tool_calls, size_t *tool_call_count, size_t index) {
  if (turbo_agent_sse_ensure_tool_call_capacity(tool_calls, tool_call_count, index) != 0) {
    return NULL;
  }

  return &(*tool_calls)[index];
}

void turbo_agent_sse_free_chat_state(turbo_agent_sse_chat_stream_state_t *state) {
  if (!state) {
    return;
  }

  tstr_free(state->id);
  tstr_free(state->role);
  tstr_free(state->content);
  tstr_free(state->finish_reason);
  turbo_agent_sse_free_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

void turbo_agent_sse_free_anthropic_state(turbo_agent_sse_anthropic_stream_state_t *state) {
  if (!state) {
    return;
  }

  tstr_free(state->id);
  tstr_free(state->role);
  tstr_free(state->content);
  tstr_free(state->stop_reason);
  turbo_agent_sse_free_tool_calls(state->tool_calls, state->tool_call_count);
  memset(state, 0, sizeof(*state));
}

char *turbo_agent_sse_normalize_newlines(const char *data, size_t len) {
  char *normalized;
  size_t i;
  size_t out = 0;

  if (!data && len > 0) {
    return NULL;
  }

  normalized = (char *)malloc(len + 1);
  if (!normalized) {
    return NULL;
  }

  for (i = 0; i < len; ++i) {
    if (data[i] != '\r') {
      normalized[out++] = data[i];
    }
  }

  normalized[out] = '\0';
  return normalized;
}

int turbo_agent_sse_collect_event_data(const char *begin, const char *end, char **out_data) {
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

      if (length > 0 && turbo_agent_util_append_bytes(&data, &length, "\n", 1) != 0) {
        tstr_free(data);
        return -1;
      }

      if (turbo_agent_util_append_bytes(&data, &length, value, (size_t)(line_end - value)) != 0) {
        tstr_free(data);
        return -1;
      }
    }

    cursor = (line_end < end) ? line_end + 1 : end;
  }

  if (!data) {
    data = turbo_agent_util_strdup("");
    if (!data) {
      return -1;
    }
  }

  *out_data = data;
  return 0;
}
