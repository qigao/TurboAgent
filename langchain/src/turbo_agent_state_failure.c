#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_flow_domain_internal.h"
#include "turbo_agent_state_json_value_internal.h"
#include "turbo_agent_state_output_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_util_internal.h"

#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

static char *turbo_agent_state_strdup_malloc_local(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }
  len = strlen(src);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len + 1);
  return copy;
}

static const json_value_t *turbo_agent_state_failure_array_last_const(
    const json_value_t *array) {
  return array && turbo_json_type(array) == TURBO_JSON_ARRAY && turbo_json_array_size(array) > 0
             ? turbo_json_array_get(array, turbo_json_array_size(array) - 1)
             : NULL;
}

static const json_value_t *turbo_agent_state_failure_latest_executor_events(
    const json_value_t *state) {
  const json_value_t *versions = turbo_agent_state_executor_event_versions(state);
  return turbo_agent_state_failure_array_last_const(versions);
}

static const json_value_t *turbo_agent_state_tool_results_event_outputs_local(
    const json_value_t *event) {
  const json_value_t *outputs;

  if (!turbo_agent_event_kind_is(event, "tool_results")) {
    return NULL;
  }

  outputs = turbo_json_object_get(event, "outputs");
  return outputs && turbo_json_type(outputs) == TURBO_JSON_ARRAY ? outputs : NULL;
}

static int turbo_agent_state_tool_result_output_fields_local(const json_value_t *output_item,
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

static int turbo_agent_state_tool_result_json_failed_local(const json_value_t *tool_json) {
  return tool_json && turbo_json_type(tool_json) == TURBO_JSON_OBJECT &&
                 !turbo_json_get_bool(tool_json, "ok", true)
              ? 1
              : 0;
}

static int turbo_agent_state_tool_result_json_like_local(const json_value_t *value) {
  return value && turbo_json_type(value) == TURBO_JSON_OBJECT &&
                 (turbo_json_object_get(value, "ok") || turbo_json_object_get(value, "summary")) &&
                 (turbo_json_object_get(value, "matches") ||
                  turbo_json_object_get(value, "content") ||
                  turbo_json_object_get(value, "stdout") ||
                  turbo_json_object_get(value, "stderr") ||
                  turbo_json_object_get(value, "changed_files") ||
                  turbo_json_object_get(value, "artifacts"))
              ? 1
              : 0;
}

static json_value_t *
turbo_agent_state_tool_result_output_parse_json_local(const json_value_t *output_item,
                                                      char **out_reason) {
  const char *output_text;
  json_value_t *output_json = NULL;

  if (out_reason) {
    *out_reason = NULL;
  }

  if (!turbo_agent_state_tool_result_output_fields_local(output_item, NULL, &output_text)) {
    if (out_reason) {
      *out_reason =
          turbo_agent_state_strdup_malloc_local("tool result payload was malformed");
    }
    return NULL;
  }

  if (output_text[0] == '\0') {
    if (out_reason) {
      *out_reason = turbo_agent_state_strdup_malloc_local("tool output was empty");
    }
    return NULL;
  }

  if (turbo_parse_json((const uint8_t *)output_text, strlen(output_text), &output_json) != 0) {
    if (out_reason) {
      *out_reason = turbo_agent_state_strdup_malloc_local("tool output was not valid JSON");
    }
    return NULL;
  }

  if (!turbo_agent_state_tool_result_json_like_local(output_json)) {
    if (out_reason) {
      *out_reason =
          turbo_agent_state_strdup_malloc_local(
              "tool output did not match canonical result envelope");
    }
    turbo_free_json(&output_json);
    return NULL;
  }

  return output_json;
}

static char *turbo_agent_state_tool_result_failure_reason_local(const json_value_t *tool_json) {
  const char *summary;
  const char *stderr_text;

  if (!turbo_agent_state_tool_result_json_failed_local(tool_json)) {
    return NULL;
  }

  summary = turbo_json_get_string(tool_json, "summary");
  if (summary && summary[0] != '\0') {
    return turbo_agent_state_strdup_malloc_local(summary);
  }

  stderr_text = turbo_json_get_string(tool_json, "stderr");
  if (stderr_text && stderr_text[0] != '\0') {
    return turbo_agent_state_strdup_malloc_local(stderr_text);
  }

  return turbo_agent_state_strdup_malloc_local("tool execution failed");
}

static int turbo_agent_state_tool_results_event_failed_local(const json_value_t *event) {
  const json_value_t *outputs;
  size_t j;

  outputs = turbo_agent_state_tool_results_event_outputs_local(event);
  if (!outputs) {
    return 0;
  }

  for (j = 0; j < turbo_json_array_size(outputs); ++j) {
    const json_value_t *output_item = turbo_json_array_get(outputs, j);
    char *parse_reason = NULL;
    json_value_t *output_json =
        turbo_agent_state_tool_result_output_parse_json_local(output_item, &parse_reason);

    if (!output_json) {
      free(parse_reason);
      return 1;
    }

    if (turbo_agent_state_tool_result_json_failed_local(output_json)) {
      turbo_free_json(&output_json);
      free(parse_reason);
      return 1;
    }

    free(parse_reason);
    turbo_free_json(&output_json);
  }

  return 0;
}

static int turbo_agent_state_output_looks_like_tool_result_json_local(const char *text) {
  json_value_t *value = NULL;
  int is_tool_result = 0;

  if (!text || text[0] != '{') {
    return 0;
  }

  if (turbo_parse_json((const uint8_t *)text, strlen(text), &value) != 0) {
    return 0;
  }

  if (value && turbo_json_type(value) == TURBO_JSON_OBJECT &&
      (turbo_json_object_get(value, "ok") || turbo_json_object_get(value, "summary")) &&
      (turbo_json_object_get(value, "matches") || turbo_json_object_get(value, "content") ||
       turbo_json_object_get(value, "stdout") || turbo_json_object_get(value, "stderr") ||
       turbo_json_object_get(value, "changed_files") ||
       turbo_json_object_get(value, "artifacts"))) {
    is_tool_result = 1;
  }

  turbo_free_json(&value);
  return is_tool_result;
}

int turbo_agent_state_executor_tool_results_failed_impl(const json_value_t *state) {
  const json_value_t *events = turbo_agent_state_failure_latest_executor_events(state);
  const json_value_t *event;
  size_t i;

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) {
    return 0;
  }

  event = turbo_agent_state_failure_array_last_const(events);
  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return 0;
  }

  if (turbo_agent_event_kind_is(event, "tool_results")) {
    return turbo_agent_state_tool_results_event_failed_local(event);
  }

  if (!turbo_agent_event_kind_is(event, "model") ||
      !turbo_agent_contains_failure_marker(turbo_json_get_string(event, "output_text"))) {
    return 0;
  }

  for (i = turbo_json_array_size(events) - 1; i > 0; --i) {
    const json_value_t *previous = turbo_json_array_get(events, i - 1);
    if (!previous || turbo_json_type(previous) != TURBO_JSON_OBJECT) {
      continue;
    }

    if (turbo_agent_event_kind_is(previous, "tool_results")) {
      return turbo_agent_state_tool_results_event_failed_local(previous);
    }

    if (turbo_agent_event_kind_is(previous, "model")) {
      break;
    }
  }

  return 0;
}

CXX_C_API char *turbo_agent_executor_failure_reason(const json_value_t *state) {
  const json_value_t *events = turbo_agent_state_failure_latest_executor_events(state);
  const char *output_text = turbo_agent_state_latest_executor_output_text(state);
  size_t i;

  if (events && turbo_json_type(events) == TURBO_JSON_ARRAY) {
    for (i = 0; i < turbo_json_array_size(events); ++i) {
      const json_value_t *event = turbo_json_array_get(events, i);
      const json_value_t *outputs;
      size_t j;

      outputs = turbo_agent_state_tool_results_event_outputs_local(event);
      if (!outputs) {
        continue;
      }

      for (j = 0; j < turbo_json_array_size(outputs); ++j) {
        const json_value_t *output_item = turbo_json_array_get(outputs, j);
        char *parse_reason = NULL;
        json_value_t *tool_json =
            turbo_agent_state_tool_result_output_parse_json_local(output_item, &parse_reason);
        char *reason;

        if (!tool_json) {
          if (parse_reason) {
            return parse_reason;
          }
          continue;
        }

        reason = turbo_agent_state_tool_result_failure_reason_local(tool_json);
        if (!reason) {
          turbo_free_json(&tool_json);
          free(parse_reason);
          continue;
        }

        turbo_free_json(&tool_json);
        free(parse_reason);
        return reason;
      }
    }
  }

  if (output_text && output_text[0] != '\0') {
    return turbo_agent_state_strdup_malloc_local(output_text);
  }

  return turbo_agent_state_strdup_malloc_local("step failed");
}

const char *turbo_agent_state_final_answer_text_impl(const json_value_t *state) {
  const char *executor_text = turbo_agent_state_latest_executor_output_text(state);
  const char *last_text = turbo_agent_state_last_output_text(state);
  const char *stored_text =
      turbo_agent_state_current_version_string_field(state, "final_answer_versions", "text");

  if (stored_text && stored_text[0] != '\0') {
    return stored_text;
  }

  if (executor_text && executor_text[0] != '\0' &&
      !turbo_agent_state_output_looks_like_tool_result_json_local(executor_text)) {
    return executor_text;
  }

  if (last_text && last_text[0] != '\0' &&
      !turbo_agent_state_output_looks_like_tool_result_json_local(last_text)) {
    return last_text;
  }

  return NULL;
}

int turbo_agent_state_parse_final_output_json_impl(const json_value_t *state,
                                                   json_value_t **out_json) {
  const char *text;

  if (!out_json) {
    return -1;
  }

  *out_json = NULL;
  text = turbo_agent_state_final_answer_text(state);
  if (!text || text[0] == '\0') {
    return -1;
  }

  return turbo_parse_json((const uint8_t *)text, strlen(text), out_json) == 0 ? 0 : -1;
}
