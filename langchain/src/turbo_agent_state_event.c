#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_core_internal.h"
#include "turbo_agent_state_output_internal.h"
#include "turbo_agent_event_internal.h"

#include "turbo_parser.h"

#include <string.h>

static int turbo_agent_state_event_kind_is_local(const json_value_t *event, const char *kind);

static const json_value_t *turbo_agent_array_last_const_local(const json_value_t *array) {
  return array && turbo_json_type(array) == TURBO_JSON_ARRAY && turbo_json_array_size(array) > 0
             ? turbo_json_array_get(array, turbo_json_array_size(array) - 1)
             : NULL;
}

static const json_value_t *turbo_agent_events_last_of_kind_local(const json_value_t *events,
                                                                 const char *kind) {
  size_t i;

  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY || !kind) {
    return NULL;
  }

  for (i = turbo_json_array_size(events); i > 0; --i) {
    const json_value_t *event = turbo_json_array_get(events, i - 1);
    const char *event_kind =
        event && turbo_json_type(event) == TURBO_JSON_OBJECT ? turbo_json_get_string(event, "kind")
                                                             : NULL;
    if (event_kind && strcmp(event_kind, kind) == 0) {
      return event;
    }
  }

  return NULL;
}

CXX_C_API const json_value_t *turbo_agent_events_last_of_kind(const json_value_t *events,
                                                              const char *kind) {
  return turbo_agent_events_last_of_kind_local(events, kind);
}

static const json_value_t *
turbo_agent_state_last_event_of_kind_local(const json_value_t *state, const char *kind) {
  size_t i;
  size_t count;

  if (!state || !kind) {
    return NULL;
  }

  count = turbo_agent_state_event_count(state);
  for (i = count; i > 0; --i) {
    const json_value_t *event = turbo_agent_state_event_at(state, i - 1);
    const char *event_kind =
        event && turbo_json_type(event) == TURBO_JSON_OBJECT ? turbo_json_get_string(event, "kind")
                                                             : NULL;
    if (event_kind && strcmp(event_kind, kind) == 0) {
      return event;
    }
  }

  return NULL;
}

CXX_C_API const json_value_t *turbo_agent_state_last_event_of_kind_impl(
    const json_value_t *state, const char *kind) {
  return turbo_agent_state_last_event_of_kind_local(state, kind);
}

static const json_value_t *turbo_agent_state_last_event_local(const json_value_t *state) {
  const json_value_t *events;

  events = turbo_agent_state_get_array_const(state, "events");
  return events && turbo_json_type(events) == TURBO_JSON_ARRAY && turbo_json_array_size(events) > 0
             ? turbo_json_array_get(events, turbo_json_array_size(events) - 1)
             : NULL;
}

CXX_C_API const json_value_t *turbo_agent_state_last_event_impl(const json_value_t *state) {
  return turbo_agent_state_last_event_local(state);
}

CXX_C_API const json_value_t *turbo_agent_state_latest_handoff_event_impl(
    const json_value_t *state) {
  return turbo_agent_state_last_event_of_kind_local(state, "handoff");
}

static const json_value_t *turbo_agent_state_last_tool_results_event_local(
    const json_value_t *state) {
  const json_value_t *events;
  size_t i;

  if (!state) {
    return NULL;
  }

  events = turbo_agent_state_get_array_const(state, "events");
  if (events && turbo_json_type(events) == TURBO_JSON_ARRAY) {
    for (i = turbo_json_array_size(events); i > 0; --i) {
      const json_value_t *event = turbo_json_array_get(events, i - 1);
      if (turbo_agent_state_event_kind_is_local(event, "tool_results")) {
        return event;
      }
    }
  }

  events = turbo_agent_state_get_current_array_version_const(state, "executor_event_versions");
  if (events && turbo_json_type(events) == TURBO_JSON_ARRAY) {
    for (i = turbo_json_array_size(events); i > 0; --i) {
      const json_value_t *event = turbo_json_array_get(events, i - 1);
      if (turbo_agent_state_event_kind_is_local(event, "tool_results")) {
        return event;
      }
    }
  }

  return NULL;
}

static int turbo_agent_state_event_kind_is_local(const json_value_t *event, const char *kind) {
  const char *event_kind;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT || !kind) {
    return 0;
  }

  event_kind = turbo_json_get_string(event, "kind");
  return event_kind && strcmp(event_kind, kind) == 0 ? 1 : 0;
}

CXX_C_API int turbo_agent_event_kind_is(const json_value_t *event, const char *kind) {
  return turbo_agent_state_event_kind_is_local(event, kind);
}

static const char *turbo_agent_handoff_event_field_local(const json_value_t *event,
                                                         const char *key) {
  if (!turbo_agent_state_event_kind_is_local(event, "handoff") || !key) {
    return NULL;
  }

  return turbo_json_get_string(event, key);
}

CXX_C_API const char *turbo_agent_state_handoff_event_phase_impl(const json_value_t *event) {
  return turbo_agent_handoff_event_field_local(event, "phase");
}

CXX_C_API const char *turbo_agent_state_handoff_event_from_agent_impl(
    const json_value_t *event) {
  return turbo_agent_handoff_event_field_local(event, "from_agent");
}

CXX_C_API const char *turbo_agent_state_handoff_event_target_agent_impl(
    const json_value_t *event) {
  return turbo_agent_handoff_event_field_local(event, "target_agent");
}

CXX_C_API const char *turbo_agent_state_handoff_event_reason_impl(const json_value_t *event) {
  return turbo_agent_handoff_event_field_local(event, "reason");
}

CXX_C_API const char *turbo_agent_state_handoff_event_active_agent_impl(
    const json_value_t *event) {
  return turbo_agent_handoff_event_field_local(event, "active_agent");
}

CXX_C_API const char *turbo_agent_event_output_text(const json_value_t *event) {
  return event && turbo_json_type(event) == TURBO_JSON_OBJECT
             ? turbo_json_get_string(event, "output_text")
             : NULL;
}

static const json_value_t *turbo_agent_state_model_event_tool_calls_local(
    const json_value_t *event) {
  const json_value_t *tool_calls;

  if (!turbo_agent_state_event_kind_is_local(event, "model")) {
    return NULL;
  }

  tool_calls = turbo_json_object_get(event, "tool_calls");
  return tool_calls && turbo_json_type(tool_calls) == TURBO_JSON_ARRAY ? tool_calls : NULL;
}

CXX_C_API const json_value_t *turbo_agent_model_event_tool_calls(const json_value_t *event) {
  return turbo_agent_state_model_event_tool_calls_local(event);
}

CXX_C_API const json_value_t *turbo_agent_state_latest_tool_results_event_impl(
    const json_value_t *state) {
  return turbo_agent_state_last_tool_results_event_local(state);
}

CXX_C_API const json_value_t *turbo_agent_state_tool_results_outputs_impl(
    const json_value_t *event) {
  const json_value_t *outputs;

  if (!turbo_agent_state_event_kind_is_local(event, "tool_results")) {
    return NULL;
  }

  outputs = turbo_json_object_get(event, "outputs");
  return outputs && turbo_json_type(outputs) == TURBO_JSON_ARRAY ? outputs : NULL;
}

static const char *turbo_agent_tool_result_output_child_field_local(
    const json_value_t *output_item, const char *key) {
  return output_item && turbo_json_type(output_item) == TURBO_JSON_OBJECT && key
             ? turbo_json_get_string(output_item, key)
             : NULL;
}

CXX_C_API const char *turbo_agent_state_tool_result_child_thread_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item, "child_thread_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_child_run_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item, "child_run_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_child_checkpoint_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item,
                                                          "child_checkpoint_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_child_status_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item, "child_status");
}

CXX_C_API const char *turbo_agent_state_tool_result_parent_agent_run_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item,
                                                          "parent_agent_run_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_call_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item,
                                                          "parent_tool_call_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_name_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item,
                                                          "parent_tool_name");
}

CXX_C_API const char *turbo_agent_state_tool_result_parent_graph_run_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item,
                                                          "parent_graph_run_id");
}

CXX_C_API const char *turbo_agent_state_tool_result_call_frame_id_impl(
    const json_value_t *output_item) {
  return turbo_agent_tool_result_output_child_field_local(output_item, "call_frame_id");
}

CXX_C_API size_t turbo_agent_model_event_tool_call_count(const json_value_t *event) {
  const json_value_t *tool_calls = turbo_agent_state_model_event_tool_calls_local(event);
  return tool_calls ? turbo_json_array_size(tool_calls) : 0;
}

CXX_C_API const char *turbo_agent_model_event_response_id(const json_value_t *event) {
  return turbo_agent_state_event_kind_is_local(event, "model")
             ? turbo_json_get_string(event, "response_id")
             : NULL;
}

CXX_C_API const json_value_t *turbo_agent_last_model_tool_calls(const json_value_t *state) {
  const json_value_t *event = turbo_agent_state_last_event_of_kind_local(state, "model");
  return turbo_agent_state_model_event_tool_calls_local(event);
}

CXX_C_API int turbo_agent_tool_call_record_fields(const json_value_t *tool_call,
                                                  const char **out_call_id,
                                                  const char **out_name,
                                                  const char **out_arguments) {
  const char *call_id_value;
  const char *name_value;
  const char *arguments_value;

  if (out_call_id) {
    *out_call_id = NULL;
  }
  if (out_name) {
    *out_name = NULL;
  }
  if (out_arguments) {
    *out_arguments = NULL;
  }

  if (!tool_call || turbo_json_type(tool_call) != TURBO_JSON_OBJECT) {
    return 0;
  }

  call_id_value = turbo_json_get_string(tool_call, "call_id");
  name_value = turbo_json_get_string(tool_call, "name");
  arguments_value = turbo_json_get_string(tool_call, "arguments");
  if (!call_id_value || !name_value || !arguments_value) {
    return 0;
  }

  if (out_call_id) {
    *out_call_id = call_id_value;
  }
  if (out_name) {
    *out_name = name_value;
  }
  if (out_arguments) {
    *out_arguments = arguments_value;
  }
  return 1;
}

CXX_C_API int turbo_agent_append_event(json_value_t *state, json_value_t *event) {
  json_value_t *events = turbo_agent_state_get_array(state, "events");
  if (!events || !event) {
    return -1;
  }

  turbo_json_array_add(events, event);
  return 0;
}

static void turbo_agent_tool_result_output_item_set_child_ref_local(
    json_value_t *output_item, const json_value_t *output_json, const char *target_key,
    const char *primary_key) {
  const char *value = NULL;

  if (!output_item || !output_json || turbo_json_type(output_item) != TURBO_JSON_OBJECT ||
      turbo_json_type(output_json) != TURBO_JSON_OBJECT || !target_key) {
    return;
  }

  if (primary_key) {
    value = turbo_json_get_string(output_json, primary_key);
  }
  if (value && value[0] != '\0') {
    turbo_json_object_set_string(output_item, target_key, value);
  }
}

static void turbo_agent_tool_result_output_item_try_attach_child_refs(
    json_value_t *output_item, const char *output) {
  json_value_t *output_json = NULL;

  if (!output_item || !output || output[0] != '{') {
    return;
  }

  if (turbo_parse_json((const uint8_t *)output, strlen(output), &output_json) != 0 ||
      !output_json || turbo_json_type(output_json) != TURBO_JSON_OBJECT) {
    turbo_free_json(&output_json);
    return;
  }

  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "child_thread_id", "child_thread_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "child_run_id", "child_run_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "child_checkpoint_id", "child_checkpoint_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "child_status", "child_status");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "parent_agent_run_id", "parent_agent_run_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "parent_tool_call_id", "parent_tool_call_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "parent_tool_name", "parent_tool_name");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "parent_graph_run_id", "parent_graph_run_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "call_frame_id", "call_frame_id");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "active_agent", "active_agent");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "handoff_target_agent", "handoff_target_agent");
  turbo_agent_tool_result_output_item_set_child_ref_local(
      output_item, output_json, "handoff_reason", "handoff_reason");

  turbo_free_json(&output_json);
}

CXX_C_API json_value_t *turbo_agent_tool_result_output_item_create(const char *call_id,
                                                                   const char *output) {
  json_value_t *output_item;

  if (!call_id || !output) {
    return NULL;
  }

  output_item = turbo_json_create_object();
  if (!output_item) {
    return NULL;
  }

  turbo_json_object_set_string(output_item, "type", "function_call_output");
  turbo_json_object_set_string(output_item, "call_id", call_id);
  turbo_json_object_set_string(output_item, "output", output);
  turbo_agent_tool_result_output_item_try_attach_child_refs(output_item, output);
  return output_item;
}

CXX_C_API json_value_t *turbo_agent_event_create(const char *kind) {
  json_value_t *event;

  if (!kind) {
    return NULL;
  }

  event = turbo_json_create_object();
  if (!event) {
    return NULL;
  }

  turbo_json_object_set_string(event, "kind", kind);
  return event;
}
