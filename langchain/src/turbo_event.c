#include "turbo_event.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

static int turbo_event_set_string_field(json_value_t *object, const char *key,
                                        const char *value) {
  json_value_t *field_value;

  field_value = turbo_json_create_string(value ? value : "");
  if (!field_value) {
    return -1;
  }

  if (turbo_runtime_json_object_set(object, key, field_value) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field_value);
    return -1;
  }

  return 0;
}

static int turbo_event_set_optional_string_or_null_field(
    json_value_t *object, const char *key, const char *value, int set_null) {
  json_value_t *field_value = NULL;

  if (!object || !key) {
    return -1;
  }
  if (!value && !set_null) {
    return 0;
  }

  field_value = value ? turbo_json_create_string(value)
                      : turbo_json_create_null();
  if (!field_value) {
    return -1;
  }
  if (turbo_runtime_json_object_set(object, key, field_value) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field_value);
    return -1;
  }

  return 0;
}

static int turbo_event_string_or_null_schema_field(json_value_t *properties,
                                                   const char *key, int allow_null) {
  json_value_t *field = turbo_json_create_object();
  json_value_t *type_value = NULL;

  if (!properties || !key || !field) {
    turbo_runtime_json_destroy(field);
    return -1;
  }

  if (allow_null) {
    json_value_t *types = turbo_json_create_array();
    if (!types) {
      turbo_runtime_json_destroy(field);
      return -1;
    }
    if (turbo_runtime_json_array_append(
            types, turbo_json_create_string("string")) !=
            TURBO_RUNTIME_JSON_OK ||
        turbo_runtime_json_array_append(types,
                                             turbo_json_create_string("null")) !=
            TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(types);
      turbo_runtime_json_destroy(field);
      return -1;
    }
    type_value = types;
  } else {
    type_value = turbo_json_create_string("string");
    if (!type_value) {
      turbo_runtime_json_destroy(field);
      return -1;
    }
  }

  if (turbo_runtime_json_object_set(field, "type", type_value) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(type_value);
    turbo_runtime_json_destroy(field);
    return -1;
  }
  type_value = NULL;
  if (turbo_runtime_json_object_set(properties, key, field) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(field);
    return -1;
  }

  return 0;
}

static int turbo_event_tool_result_optional_field_valid(
    const json_value_t *event, const char *key, int allow_null) {
  const json_value_t *value;
  turbo_json_type_t kind;

  if (!event || !key) {
    return 0;
  }

  value = turbo_json_object_get(event, key);
  if (!value) {
    return 1;
  }

  kind = turbo_json_type(value);
  return kind == TURBO_JSON_STRING ||
                 (allow_null && kind == TURBO_JSON_NULL)
             ? 1
             : 0;
}

static int turbo_event_required_string_or_null_field_valid(
    const json_value_t *event, const char *key, int allow_null) {
  const json_value_t *value;
  turbo_json_type_t kind;

  if (!event || !key) {
    return 0;
  }

  value = turbo_json_object_get(event, key);
  if (!value) {
    return 0;
  }

  kind = turbo_json_type(value);
  return kind == TURBO_JSON_STRING ||
                 (allow_null && kind == TURBO_JSON_NULL)
             ? 1
             : 0;
}

static int turbo_event_tool_result_attach_child_fields_from_json_value_object(
    json_value_t *event, const json_value_t *object) {
  const json_value_t *value;
  const char *text = NULL;
  int allow_null = 0;
  size_t i;
  static const struct {
    const char *target_key;
    const char *primary_key;
    int allow_null;
  } field_map[] = {
      {"child_thread_id", "child_thread_id", 0},
      {"child_run_id", "child_run_id", 0},
      {"child_checkpoint_id", "child_checkpoint_id", 1},
      {"child_status", "child_status", 0},
      {"parent_agent_run_id", "parent_agent_run_id", 0},
      {"parent_tool_call_id", "parent_tool_call_id", 0},
      {"parent_tool_name", "parent_tool_name", 0},
      {"parent_graph_run_id", "parent_graph_run_id", 0},
      {"call_frame_id", "call_frame_id", 0},
  };

  if (!event || !object ||
      turbo_json_type(object) != TURBO_JSON_OBJECT) {
    return 0;
  }

  for (i = 0; i < sizeof(field_map) / sizeof(field_map[0]); ++i) {
    if (turbo_json_object_get(event, field_map[i].target_key)) {
      continue;
    }
    value = turbo_json_object_get(object, field_map[i].primary_key);
    if (!value) {
      continue;
    }
    allow_null = field_map[i].allow_null &&
                 turbo_json_type(value) == TURBO_JSON_NULL;
    text = turbo_runtime_json_value_as_string(value);
    if (!text && !allow_null) {
      continue;
    }
    if (turbo_event_set_optional_string_or_null_field(event, field_map[i].target_key, text,
                                                      allow_null) != 0) {
      return -1;
    }
  }

  return 0;
}

static int turbo_event_tool_result_attach_child_fields_from_output_json(
    json_value_t *event, const char *output) {
  json_value_t *output_json = NULL;
  const json_value_t *value;
  const char *value_key = NULL;
  const char *text = NULL;
  int set_null = 0;
  size_t i;
  static const struct {
    const char *target_key;
    const char *primary_key;
    int allow_null;
  } field_map[] = {
      {"child_thread_id", "child_thread_id", 0},
      {"child_run_id", "child_run_id", 0},
      {"child_checkpoint_id", "child_checkpoint_id", 1},
      {"child_status", "child_status", 0},
      {"parent_agent_run_id", "parent_agent_run_id", 0},
      {"parent_tool_call_id", "parent_tool_call_id", 0},
      {"parent_tool_name", "parent_tool_name", 0},
      {"parent_graph_run_id", "parent_graph_run_id", 0},
      {"call_frame_id", "call_frame_id", 0},
  };

  if (!event || !output || output[0] != '{') {
    return 0;
  }
  if (turbo_parse_json((const uint8_t *)output, strlen(output), &output_json) != 0 ||
      !output_json || turbo_json_type(output_json) != TURBO_JSON_OBJECT) {
    turbo_free_json(&output_json);
    return 0;
  }

  for (i = 0; i < sizeof(field_map) / sizeof(field_map[0]); ++i) {
    if (turbo_json_object_get(event, field_map[i].target_key)) {
      continue;
    }
    value_key = NULL;
    value = turbo_json_object_get(output_json, field_map[i].primary_key);
    if (value) {
      value_key = field_map[i].primary_key;
    }
    if (!value) {
      continue;
    }
    set_null = field_map[i].allow_null && turbo_json_type(value) == TURBO_JSON_NULL;
    text = turbo_json_type(value) == TURBO_JSON_STRING && value_key
               ? turbo_json_get_string(output_json, value_key)
               : NULL;
    if (!text && !set_null) {
      continue;
    }
    if (turbo_event_set_optional_string_or_null_field(event, field_map[i].target_key, text,
                                                      set_null) != 0) {
      turbo_free_json(&output_json);
      return -1;
    }
  }

  turbo_free_json(&output_json);
  return 0;
}

static json_value_t *turbo_event_tool_call_schema_json_value(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = turbo_json_create_object();
  json_value_t *required = turbo_json_create_array();
  json_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_json_object_set(schema, "properties", properties);
  turbo_runtime_json_object_set(schema, "required", required);

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "call_id", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("call_id"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "name", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("name"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "arguments", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("arguments"));

  return schema;
}

const char *turbo_event_kind_json_value(const json_value_t *event) {
  const json_value_t *kind;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  kind = turbo_json_object_get(event, "kind");
  if (!kind || turbo_json_type(kind) != TURBO_JSON_STRING) {
    return NULL;
  }

  return turbo_runtime_json_value_as_string(kind);
}

int turbo_event_stream_mode_accepts_json_value(
    turbo_event_stream_mode_t mode, const json_value_t *event) {
  const char *kind = turbo_event_kind_json_value(event);

  if (!kind) {
    return 0;
  }
  switch (mode) {
  case TURBO_EVENT_STREAM_ALL:
    return 1;
  case TURBO_EVENT_STREAM_MESSAGES:
    return strcmp(kind, "model") == 0;
  case TURBO_EVENT_STREAM_UPDATES:
    return strcmp(kind, "model") != 0;
  case TURBO_EVENT_STREAM_TOOLS:
    return strcmp(kind, "tool_result") == 0;
  case TURBO_EVENT_STREAM_DEBUG:
    return strcmp(kind, "trace") == 0;
  default:
    return 0;
  }
}

void turbo_event_stream_filter_sink_json_value(
    const json_value_t *event, void *user_data) {
  turbo_event_stream_filter_t *filter = (turbo_event_stream_filter_t *)user_data;

  if (!filter || !filter->sink) {
    return;
  }
  if (!turbo_event_stream_mode_accepts_json_value(filter->mode, event)) {
    return;
  }
  filter->sink(event, filter->sink_user_data);
}

int turbo_event_validate_json_value(const json_value_t *event) {
  const char *kind = turbo_event_kind_json_value(event);

  if (!kind) {
    return -1;
  }

  if (strcmp(kind, "model") == 0) {
    return turbo_event_model_validate_json_value(event);
  }
  if (strcmp(kind, "trace") == 0) {
    return turbo_event_trace_validate_json_value(event);
  }
  if (strcmp(kind, "tool_result") == 0) {
    return turbo_event_tool_result_validate_json_value(event);
  }
  if (strcmp(kind, "handoff") == 0) {
    return turbo_event_handoff_validate_json_value(event);
  }

  return -1;
}

json_value_t *turbo_event_model_schema_json_value(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = turbo_json_create_object();
  json_value_t *required = turbo_json_create_array();
  json_value_t *field = NULL;
  json_value_t *items = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_json_object_set(schema, "properties", properties);
  turbo_runtime_json_object_set(schema, "required", required);

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "kind", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("kind"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "response_id", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("response_id"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "output_text", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("output_text"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "array");
  items = turbo_event_tool_call_schema_json_value();
  if (!field || !items) {
    turbo_runtime_json_destroy(field);
    turbo_runtime_json_destroy(items);
    turbo_runtime_json_destroy(schema);
    return NULL;
  }
  turbo_runtime_json_object_set(field, "items", items);
  turbo_runtime_json_object_set(properties, "tool_calls", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("tool_calls"));

  return schema;
}

json_value_t *turbo_event_trace_schema_json_value(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = turbo_json_create_object();
  json_value_t *required = turbo_json_create_array();
  json_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_json_object_set(schema, "properties", properties);
  turbo_runtime_json_object_set(schema, "required", required);

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "kind", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("kind"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "name", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("name"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "detail", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("detail"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "payload", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("payload"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "integer");
  turbo_runtime_json_object_set(properties, "status", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("status"));

  return schema;
}

json_value_t *turbo_event_tool_result_schema_json_value(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = turbo_json_create_object();
  json_value_t *required = turbo_json_create_array();
  json_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_json_object_set(schema, "properties", properties);
  turbo_runtime_json_object_set(schema, "required", required);

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "kind", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("kind"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "name", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("name"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "arguments_json", field);
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("arguments_json"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "output", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("output"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "integer");
  turbo_runtime_json_object_set(properties, "status", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("status"));

  if (turbo_event_string_or_null_schema_field(properties, "child_thread_id", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "child_run_id", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "child_checkpoint_id", 1) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "child_status", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "parent_agent_run_id", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "parent_tool_call_id", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "parent_tool_name", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "parent_graph_run_id", 0) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "call_frame_id", 0) != 0) {
    turbo_runtime_json_destroy(schema);
    return NULL;
  }

  return schema;
}

json_value_t *turbo_event_handoff_schema_json_value(void) {
  json_value_t *schema = turbo_json_create_object();
  json_value_t *properties = turbo_json_create_object();
  json_value_t *required = turbo_json_create_array();
  json_value_t *field = NULL;

  if (!schema || !properties || !required) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    return NULL;
  }

  turbo_event_set_string_field(schema, "type", "object");
  turbo_runtime_json_object_set(schema, "properties", properties);
  turbo_runtime_json_object_set(schema, "required", required);

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "kind", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("kind"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "string");
  turbo_runtime_json_object_set(properties, "phase", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("phase"));

  if (turbo_event_string_or_null_schema_field(properties, "from_agent", 1) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "target_agent", 1) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "reason", 1) != 0 ||
      turbo_event_string_or_null_schema_field(properties, "active_agent", 1) != 0) {
    turbo_runtime_json_destroy(schema);
    return NULL;
  }
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("from_agent"));
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("target_agent"));
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("reason"));
  turbo_runtime_json_array_append(required,
                                       turbo_json_create_string("active_agent"));

  field = turbo_json_create_object();
  turbo_event_set_string_field(field, "type", "integer");
  turbo_runtime_json_object_set(properties, "status", field);
  turbo_runtime_json_array_append(required, turbo_json_create_string("status"));

  return schema;
}

int turbo_event_model_validate_json_value(const json_value_t *event) {
  const json_value_t *kind;
  const json_value_t *response_id;
  const json_value_t *output_text;
  const json_value_t *tool_calls;
  size_t i;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  kind = turbo_json_object_get(event, "kind");
  response_id = turbo_json_object_get(event, "response_id");
  output_text = turbo_json_object_get(event, "output_text");
  tool_calls = turbo_json_object_get(event, "tool_calls");

  if (!kind || !response_id || !output_text || !tool_calls) {
    return -1;
  }
  if (strcmp(turbo_runtime_json_value_as_string(kind), "model") != 0) {
    return -1;
  }
  if (turbo_json_type(response_id) != TURBO_JSON_STRING ||
      turbo_json_type(output_text) != TURBO_JSON_STRING ||
      turbo_json_type(tool_calls) != TURBO_JSON_ARRAY) {
    return -1;
  }

  for (i = 0; i < turbo_runtime_json_value_size(tool_calls); ++i) {
    const json_value_t *call = turbo_json_array_get(tool_calls, i);

    if (!call || turbo_json_type(call) != TURBO_JSON_OBJECT) {
      return -1;
    }
    if (!turbo_json_object_get(call, "call_id") ||
        !turbo_json_object_get(call, "name") ||
        !turbo_json_object_get(call, "arguments")) {
      return -1;
    }
  }

  return 0;
}

int turbo_event_trace_validate_json_value(const json_value_t *event) {
  const json_value_t *kind;
  const json_value_t *name;
  const json_value_t *detail;
  const json_value_t *payload;
  const json_value_t *status;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  kind = turbo_json_object_get(event, "kind");
  name = turbo_json_object_get(event, "name");
  detail = turbo_json_object_get(event, "detail");
  payload = turbo_json_object_get(event, "payload");
  status = turbo_json_object_get(event, "status");
  if (!kind || !name || !detail || !payload || !status) {
    return -1;
  }
  if (strcmp(turbo_runtime_json_value_as_string(kind), "trace") != 0) {
    return -1;
  }
  if (turbo_json_type(name) != TURBO_JSON_STRING ||
      turbo_json_type(detail) != TURBO_JSON_STRING ||
      turbo_json_type(payload) != TURBO_JSON_STRING) {
    return -1;
  }
  return turbo_json_type(status) == TURBO_JSON_NUMBER ? 0 : -1;
}

int turbo_event_tool_result_validate_json_value(const json_value_t *event) {
  const json_value_t *kind;
  const json_value_t *name;
  const json_value_t *arguments_json;
  const json_value_t *output;
  const json_value_t *status;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  kind = turbo_json_object_get(event, "kind");
  name = turbo_json_object_get(event, "name");
  arguments_json = turbo_json_object_get(event, "arguments_json");
  output = turbo_json_object_get(event, "output");
  status = turbo_json_object_get(event, "status");
  if (!kind || !name || !arguments_json || !output || !status) {
    return -1;
  }
  if (strcmp(turbo_runtime_json_value_as_string(kind), "tool_result") != 0) {
    return -1;
  }
  if (turbo_json_type(name) != TURBO_JSON_STRING ||
      turbo_json_type(arguments_json) != TURBO_JSON_STRING ||
      turbo_json_type(output) != TURBO_JSON_STRING ||
      turbo_json_type(status) != TURBO_JSON_NUMBER) {
    return -1;
  }
  if (!turbo_event_tool_result_optional_field_valid(event, "child_thread_id", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "child_run_id", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "child_checkpoint_id", 1) ||
      !turbo_event_tool_result_optional_field_valid(event, "child_status", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "parent_agent_run_id", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "parent_tool_call_id", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "parent_tool_name", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "parent_graph_run_id", 0) ||
      !turbo_event_tool_result_optional_field_valid(event, "call_frame_id", 0)) {
    return -1;
  }

  return 0;
}

int turbo_event_handoff_validate_json_value(const json_value_t *event) {
  const json_value_t *kind;
  const json_value_t *phase;
  const json_value_t *status;

  if (!event || turbo_json_type(event) != TURBO_JSON_OBJECT) {
    return -1;
  }

  kind = turbo_json_object_get(event, "kind");
  phase = turbo_json_object_get(event, "phase");
  status = turbo_json_object_get(event, "status");
  if (!kind || !phase || !status) {
    return -1;
  }
  if (strcmp(turbo_runtime_json_value_as_string(kind), "handoff") != 0) {
    return -1;
  }
  if (turbo_json_type(phase) != TURBO_JSON_STRING ||
      turbo_json_type(status) != TURBO_JSON_NUMBER) {
    return -1;
  }
  if (!turbo_event_required_string_or_null_field_valid(event, "from_agent", 1) ||
      !turbo_event_required_string_or_null_field_valid(event, "target_agent", 1) ||
      !turbo_event_required_string_or_null_field_valid(event, "reason", 1) ||
      !turbo_event_required_string_or_null_field_valid(event, "active_agent", 1)) {
    return -1;
  }

  return 0;
}

json_value_t *
turbo_event_model_create_json_value(const char *response_id, const char *output_text,
                              const json_value_t *tool_calls) {
  json_value_t *event = turbo_json_create_object();
  json_value_t *tool_calls_copy;

  if (!event) {
    return NULL;
  }

  if (turbo_event_set_string_field(event, "kind", "model") != 0 ||
      turbo_event_set_string_field(event, "response_id", response_id ? response_id : "") != 0 ||
      turbo_event_set_string_field(event, "output_text", output_text ? output_text : "") != 0) {
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  if (tool_calls) {
    tool_calls_copy = turbo_json_clone(tool_calls);
  } else {
    tool_calls_copy = turbo_json_create_array();
  }
  if (!tool_calls_copy) {
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  if (turbo_runtime_json_object_set(event, "tool_calls", tool_calls_copy) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(tool_calls_copy);
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  return event;
}

json_value_t *turbo_event_trace_create_json_value(const char *name,
                                                               const char *detail,
                                                               const char *payload,
                                                               int64_t status) {
  json_value_t *event = turbo_json_create_object();
  json_value_t *status_value;

  if (!event) {
    return NULL;
  }

  status_value = turbo_json_create_int64(status);
  if (!status_value || turbo_event_set_string_field(event, "kind", "trace") != 0 ||
      turbo_event_set_string_field(event, "name", name ? name : "") != 0 ||
      turbo_event_set_string_field(event, "detail", detail ? detail : "") != 0 ||
      turbo_event_set_string_field(event, "payload", payload ? payload : "") != 0 ||
      turbo_runtime_json_object_set(event, "status", status_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(status_value);
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  return event;
}

json_value_t *turbo_event_tool_result_create_json_value(
    const char *name, const char *arguments_json, const char *output,
    const json_value_t *output_value, int64_t status) {
  json_value_t *event = turbo_json_create_object();
  json_value_t *status_value;
  json_value_t *output_value_copy = NULL;

  if (!event) {
    return NULL;
  }

  status_value = turbo_json_create_int64(status);
  if (!status_value || turbo_event_set_string_field(event, "kind", "tool_result") != 0 ||
      turbo_event_set_string_field(event, "name", name ? name : "") != 0 ||
      turbo_event_set_string_field(event, "arguments_json", arguments_json ? arguments_json : "{}") !=
          0 ||
      turbo_event_set_string_field(event, "output", output ? output : "") != 0 ||
      turbo_runtime_json_object_set(event, "status", status_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(status_value);
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  if (output_value) {
    output_value_copy = turbo_json_clone(output_value);
    if (!output_value_copy ||
        turbo_runtime_json_object_set(event, "output_value", output_value_copy) !=
            TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(output_value_copy);
      turbo_runtime_json_destroy(event);
      return NULL;
    }
  }

  if ((output_value &&
       turbo_event_tool_result_attach_child_fields_from_json_value_object(event, output_value) != 0) ||
      turbo_event_tool_result_attach_child_fields_from_output_json(event, output) != 0) {
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  return event;
}

json_value_t *turbo_event_handoff_create_json_value(
    const char *phase, const char *from_agent, const char *target_agent, const char *reason,
    const char *active_agent, int64_t status) {
  json_value_t *event = turbo_json_create_object();
  json_value_t *status_value;

  if (!event) {
    return NULL;
  }

  status_value = turbo_json_create_int64(status);
  if (!status_value || turbo_event_set_string_field(event, "kind", "handoff") != 0 ||
      turbo_event_set_string_field(event, "phase", phase ? phase : "") != 0 ||
      turbo_event_set_optional_string_or_null_field(event, "from_agent", from_agent, 1) != 0 ||
      turbo_event_set_optional_string_or_null_field(event, "target_agent", target_agent, 1) != 0 ||
      turbo_event_set_optional_string_or_null_field(event, "reason", reason, 1) != 0 ||
      turbo_event_set_optional_string_or_null_field(event, "active_agent", active_agent, 1) != 0 ||
      turbo_runtime_json_object_set(event, "status", status_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(status_value);
    turbo_runtime_json_destroy(event);
    return NULL;
  }

  return event;
}
