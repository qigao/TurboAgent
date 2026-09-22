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

  output_items = turbo_json_create_array();
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
      turbo_free_json(&output_items);
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
        turbo_free_json(&output_items);
        turbo_free_json(&completed_response);
        return -1;
      }

      type = turbo_json_get_string(event, "type");
      response = turbo_json_object_get(event, "response");
      item = turbo_json_object_get(event, "item");
      if (type && strcmp(type, "response.output_item.done") == 0) {
        if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
          turbo_free_json(&event);
          tstr_free(data);
          free(normalized);
          turbo_free_json(&output_items);
          turbo_free_json(&completed_response);
          return -1;
        }
        json_value_t *item_clone = turbo_json_clone(item);
        if (!item_clone) {
          turbo_free_json(&event);
          tstr_free(data);
          free(normalized);
          turbo_free_json(&output_items);
          turbo_free_json(&completed_response);
          return -1;
        }
        turbo_json_array_add(output_items, item_clone);
      } else if (type && strcmp(type, "response.completed") == 0) {
        if (!response || turbo_json_type(response) != TURBO_JSON_OBJECT) {
          turbo_free_json(&event);
          tstr_free(data);
          free(normalized);
          turbo_free_json(&output_items);
          turbo_free_json(&completed_response);
          return -1;
        }
        turbo_free_json(&completed_response);
        completed_response = turbo_json_clone(response);
        if (!completed_response) {
          turbo_free_json(&event);
          tstr_free(data);
          free(normalized);
          turbo_free_json(&output_items);
          return -1;
        }
      }
    }

    turbo_free_json(&event);
    tstr_free(data);
    cursor = (*event_end == '\0') ? event_end : event_end + 2;
  }

  free(normalized);
  if (completed_response && turbo_json_type(completed_response) == TURBO_JSON_OBJECT) {
    const char *completed_id = turbo_json_get_string(completed_response, "id");
    const json_value_t *completed_output = turbo_json_object_get(completed_response, "output");
    if (!completed_id || completed_id[0] == '\0') {
      turbo_free_json(&completed_response);
      turbo_free_json(&output_items);
      return -1;
    }
    if (output_items && turbo_json_array_size(output_items) > 0 &&
        (!completed_output || turbo_json_type(completed_output) != TURBO_JSON_ARRAY ||
         turbo_json_array_size(completed_output) == 0)) {
      json_value_t *response = turbo_json_create_object();
      json_value_t *rebuilt_output = turbo_json_create_array();
      size_t i;

      if (!response || !rebuilt_output) {
        turbo_free_json(&completed_response);
        turbo_free_json(&response);
        turbo_free_json(&rebuilt_output);
        turbo_free_json(&output_items);
        return -1;
      }

      turbo_json_object_set_string(response, "id", completed_id);

      for (i = 0; i < turbo_json_array_size(output_items); ++i) {
        const json_value_t *output_item = turbo_json_array_get(output_items, i);
        json_value_t *output_clone = turbo_json_clone(output_item);
        if (!output_clone) {
          turbo_free_json(&response);
          turbo_free_json(&rebuilt_output);
          turbo_free_json(&completed_response);
          turbo_free_json(&output_items);
          return -1;
        }
        turbo_json_array_add(rebuilt_output, output_clone);
      }

      turbo_json_object_add(response, "output", rebuilt_output);
      *out_response_json = turbo_json_serialize(response, NULL);
      turbo_free_json(&response);
      turbo_free_json(&completed_response);
      turbo_free_json(&output_items);
      return *out_response_json ? 0 : -1;
    }

    *out_response_json = turbo_json_serialize(completed_response, NULL);
    turbo_free_json(&completed_response);
    turbo_free_json(&output_items);
    return *out_response_json ? 0 : -1;
  }

  turbo_free_json(&output_items);
  return -1;
}
