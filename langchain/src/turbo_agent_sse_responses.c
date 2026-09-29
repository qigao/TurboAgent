#include "turbo_agent_sse_state_internal.h"
#include "turbo_agent_util_internal.h"

#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

int turbo_agent_responses_sse_to_json(const char *sse_data, size_t sse_len,
                                      char **out_response_json) {
  char *normalized;
  char *cursor;
  json_value_t *completed_response = NULL;
  json_value_t *output_items = NULL;

  if (!sse_data || !out_response_json) {
    return -1;
  }

  *out_response_json = NULL;
  normalized = turbo_agent_sse_normalize_newlines(sse_data, sse_len);
  if (!normalized) {
    return -1;
  }

  output_items = json_create_array();
  if (!output_items) {
    free(normalized);
    return -1;
  }

  cursor = normalized;
  while (*cursor != '\0') {
    char *event_end = strstr(cursor, "\n\n");
    char *data = NULL;
    json_value_t *event = NULL;
    const char *type;
    json_value_t *response;
    const json_value_t *item;

    if (!event_end) {
      event_end = cursor + strlen(cursor);
    }

    if (event_end == cursor) {
      cursor = (*event_end == '\0') ? event_end : event_end + 2;
      continue;
    }

    if (turbo_agent_sse_collect_event_data(cursor, event_end, &data) != 0) {
      free(normalized);
      json_free(output_items); output_items = NULL;
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
        json_free(output_items); output_items = NULL;
        json_free(completed_response); completed_response = NULL;
        return -1;
      }

      type = json_get_string(event, "type");
      response = json_object_get(event, "response");
      item = json_object_get(event, "item");
      if (type && strcmp(type, "response.output_item.done") == 0) {
        if (!item || json_type(item) != JSON_OBJECT) {
          json_free(event); event = NULL;
          tstr_free(data);
          free(normalized);
          json_free(output_items); output_items = NULL;
          json_free(completed_response); completed_response = NULL;
          return -1;
        }
        json_value_t *item_clone = json_clone(item);
        if (!item_clone) {
          json_free(event); event = NULL;
          tstr_free(data);
          free(normalized);
          json_free(output_items); output_items = NULL;
          json_free(completed_response); completed_response = NULL;
          return -1;
        }
        json_array_add(output_items, item_clone);
      } else if (type && strcmp(type, "response.completed") == 0) {
        if (!response || json_type(response) != JSON_OBJECT) {
          json_free(event); event = NULL;
          tstr_free(data);
          free(normalized);
          json_free(output_items); output_items = NULL;
          json_free(completed_response); completed_response = NULL;
          return -1;
        }
        json_free(completed_response); completed_response = NULL;
        completed_response = json_clone(response);
        if (!completed_response) {
          json_free(event); event = NULL;
          tstr_free(data);
          free(normalized);
          json_free(output_items); output_items = NULL;
          return -1;
        }
      }
    }

    json_free(event); event = NULL;
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (completed_response && json_type(completed_response) == JSON_OBJECT) {
    const char *completed_id = json_get_string(completed_response, "id");
    const json_value_t *completed_output = json_object_get(completed_response, "output");
    if (!completed_id || completed_id[0] == '\0') {
      json_free(completed_response); completed_response = NULL;
      json_free(output_items); output_items = NULL;
      return -1;
    }
    if (output_items && json_array_size(output_items) > 0 &&
        (!completed_output || json_type(completed_output) != JSON_ARRAY ||
         json_array_size(completed_output) == 0)) {
      json_value_t *response = json_create_object();
      json_value_t *rebuilt_output = json_create_array();
      size_t i;

      if (!response || !rebuilt_output) {
        json_free(completed_response); completed_response = NULL;
        json_free(response); response = NULL;
        json_free(rebuilt_output); rebuilt_output = NULL;
        json_free(output_items); output_items = NULL;
        return -1;
      }

      json_object_set_string(response, "id", completed_id);

      for (i = 0; i < json_array_size(output_items); ++i) {
        const json_value_t *output_item = json_array_get(output_items, i);
        json_value_t *output_clone = json_clone(output_item);
        if (!output_clone) {
          json_free(response); response = NULL;
          json_free(rebuilt_output); rebuilt_output = NULL;
          json_free(completed_response); completed_response = NULL;
          json_free(output_items); output_items = NULL;
          return -1;
        }
        json_array_add(rebuilt_output, output_clone);
      }

      json_object_add(response, "output", rebuilt_output);
      *out_response_json = json_serialize(response, NULL);
      json_free(response); response = NULL;
      json_free(completed_response); completed_response = NULL;
      json_free(output_items); output_items = NULL;
      return *out_response_json ? 0 : -1;
    }

    *out_response_json = json_serialize(completed_response, NULL);
    json_free(completed_response); completed_response = NULL;
    json_free(output_items); output_items = NULL;
    return *out_response_json ? 0 : -1;
  }

  json_free(output_items); output_items = NULL;
  return -1;
}
