#include "turbo_prompt.h"
#include "turbo_agent_util_internal.h"
#include <fmt.h>
#include <mustache.h>
#include <mustache_json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *turbo_prompt_strdup(const char *src) {
  return (char *)turbo_agent_util_strdup(src);
}

static char *turbo_prompt_cstrdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    src = "";
  }

  len = strlen(src);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len + 1);
  return copy;
}

static char *turbo_prompt_join_strings(const char *left, const char *separator, const char *right) {
  return (char *)tstr_cat_typed(tstr_new(), "{}{}{}", left ? left : "",
                                separator ? separator : "", right ? right : "");
}

static turbo_runtime_data_bind_value_t *turbo_prompt_schema_string_node(const char *value);
static int turbo_prompt_schema_object_put(turbo_runtime_data_bind_value_t *object, const char *key,
                                          turbo_runtime_data_bind_value_t *value);
static turbo_runtime_data_bind_value_t *turbo_prompt_schema_type_node(const char *value);

static turbo_runtime_data_bind_value_t *turbo_prompt_schema_string_node(const char *value) {
  return turbo_runtime_data_bind_value_create_string(value);
}

static int turbo_prompt_schema_object_put(turbo_runtime_data_bind_value_t *object, const char *key,
                                          turbo_runtime_data_bind_value_t *value) {
  if (!object || !key || !value) {
    turbo_runtime_data_bind_value_destroy(value);
    return -1;
  }

  if (turbo_runtime_data_bind_object_set(object, key, value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(value);
    return -1;
  }

  return 0;
}

static turbo_runtime_data_bind_value_t *turbo_prompt_schema_type_node(const char *value) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();

  if (!schema) {
    return NULL;
  }
  if (turbo_prompt_schema_object_put(schema, "type", turbo_prompt_schema_string_node(value)) != 0) {
    turbo_runtime_data_bind_value_destroy(schema);
    return NULL;
  }
  return schema;
}

static int turbo_prompt_schema_array_append(turbo_runtime_data_bind_value_t *array,
                                            turbo_runtime_data_bind_value_t *value) {
  if (!array || !value) {
    turbo_runtime_data_bind_value_destroy(value);
    return -1;
  }
  if (turbo_runtime_data_bind_array_append(array, value) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(value);
    return -1;
  }
  return 0;
}



static json_value_t *turbo_prompt_content_part_to_json(
    const turbo_runtime_data_bind_value_t *part) {
  const char *type_name;
  json_value_t *json_part;

  if (!part || turbo_runtime_data_bind_value_kind(part) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return NULL;
  }

  type_name = turbo_runtime_data_bind_value_as_string(
      turbo_runtime_data_bind_object_get(part, "type"));
  if (!type_name) {
    return NULL;
  }

  json_part = turbo_runtime_data_bind_value_to_json(part);
  if (!json_part) {
    return NULL;
  }

  if (strcmp(type_name, "tool_use") == 0) {
    const turbo_runtime_data_bind_value_t *input =
        turbo_runtime_data_bind_object_get(part, "input");
    json_value_t *input_json;

    if (!input) {
      turbo_free_json(&json_part);
      return NULL;
    }
    input_json = turbo_runtime_data_bind_value_to_json(input);
    if (!input_json) {
      turbo_free_json(&json_part);
      return NULL;
    }
    turbo_json_object_add(json_part, "input", input_json);
  }

  return json_part;
}

static json_value_t *turbo_prompt_content_bind_to_json(
    const turbo_runtime_data_bind_value_t *content) {
  size_t i;
  json_value_t *json_content;

  if (!content) {
    return NULL;
  }

  switch (turbo_runtime_data_bind_value_kind(content)) {
    case TURBO_RUNTIME_DATA_BIND_VALUE_NULL:
      return turbo_json_create_null();
    case TURBO_RUNTIME_DATA_BIND_VALUE_STRING:
      return turbo_json_create_string(turbo_runtime_data_bind_value_as_string(content));
    case TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY:
      json_content = turbo_json_create_array();
      if (!json_content) {
        return NULL;
      }
      for (i = 0; i < turbo_runtime_data_bind_value_size(content); ++i) {
        const turbo_runtime_data_bind_value_t *part =
            turbo_runtime_data_bind_array_get(content, i);
        json_value_t *json_part = turbo_prompt_content_part_to_json(part);

        if (!json_part) {
          turbo_free_json(&json_content);
          return NULL;
        }
        turbo_json_array_add(json_content, json_part);
      }
      return json_content;
    default:
      return NULL;
  }
}

static json_value_t *turbo_prompt_tool_call_to_json(
    const turbo_runtime_data_bind_value_t *tool_call) {
  return turbo_runtime_data_bind_value_to_json(tool_call);
}

static json_value_t *turbo_prompt_message_to_openai_chat_json(
    const turbo_runtime_data_bind_value_t *message) {
  const char *role;
  const turbo_runtime_data_bind_value_t *content;
  const turbo_runtime_data_bind_value_t *tool_calls;
  const turbo_runtime_data_bind_value_t *tool_call_id;
  json_value_t *json_message;
  json_value_t *json_content;
  size_t i;

  if (turbo_prompt_message_validate_bind(message) != TURBO_PROMPT_OK) {
    return NULL;
  }

  role = turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(message, "role"));
  content = turbo_runtime_data_bind_object_get(message, "content");
  tool_calls = turbo_runtime_data_bind_object_get(message, "tool_calls");
  tool_call_id = turbo_runtime_data_bind_object_get(message, "tool_call_id");
  if (!role || !content) {
    return NULL;
  }

  json_message = turbo_json_create_object();
  if (!json_message) {
    return NULL;
  }

  turbo_json_object_set_string(json_message, "role", role);
  json_content = turbo_prompt_content_bind_to_json(content);
  if (!json_content) {
    turbo_free_json(&json_message);
    return NULL;
  }
  turbo_json_object_add(json_message, "content", json_content);

  if (tool_call_id) {
    turbo_json_object_set_string(
        json_message, "tool_call_id", turbo_runtime_data_bind_value_as_string(tool_call_id));
  }

  if (tool_calls) {
    json_value_t *json_tool_calls = turbo_json_create_array();
    if (!json_tool_calls) {
      turbo_free_json(&json_message);
      return NULL;
    }
    for (i = 0; i < turbo_runtime_data_bind_value_size(tool_calls); ++i) {
      json_value_t *json_tool_call = turbo_prompt_tool_call_to_json(
          turbo_runtime_data_bind_array_get(tool_calls, i));
      if (!json_tool_call) {
        turbo_free_json(&json_tool_calls);
        turbo_free_json(&json_message);
        return NULL;
      }
      turbo_json_array_add(json_tool_calls, json_tool_call);
    }
    turbo_json_object_add(json_message, "tool_calls", json_tool_calls);
  }

  return json_message;
}

static json_value_t *turbo_prompt_tool_call_to_anthropic_part(
    const turbo_runtime_data_bind_value_t *tool_call) {
  const turbo_runtime_data_bind_value_t *function_value;
  const turbo_runtime_data_bind_value_t *arguments_value;
  json_value_t *part;
  json_value_t *input_json;

  if (!tool_call) {
    return NULL;
  }

  function_value = turbo_runtime_data_bind_object_get(tool_call, "function");
  arguments_value = function_value
                        ? turbo_runtime_data_bind_object_get(function_value, "arguments")
                        : NULL;
  if (!function_value || !arguments_value) {
    return NULL;
  }

  part = turbo_json_create_object();
  if (!part) {
    return NULL;
  }

  turbo_json_object_set_string(part, "type", "tool_use");
  turbo_json_object_set_string(
      part, "id",
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(tool_call, "id")));
  turbo_json_object_set_string(
      part, "name",
      turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(function_value, "name")));

  if (turbo_runtime_data_bind_value_kind(arguments_value) == TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    if (turbo_parse_json(
            (const uint8_t *)turbo_runtime_data_bind_value_as_string(arguments_value),
            strlen(turbo_runtime_data_bind_value_as_string(arguments_value)), &input_json) != 0) {
      input_json = turbo_json_create_object();
    }
  } else {
    input_json = turbo_runtime_data_bind_value_to_json(arguments_value);
  }
  if (!input_json) {
    turbo_free_json(&part);
    return NULL;
  }
  turbo_json_object_add(part, "input", input_json);
  return part;
}

static json_value_t *turbo_prompt_message_to_anthropic_json(
    const turbo_runtime_data_bind_value_t *message) {
  const char *role;
  const turbo_runtime_data_bind_value_t *content;
  const turbo_runtime_data_bind_value_t *tool_calls;
  const turbo_runtime_data_bind_value_t *tool_call_id;
  json_value_t *json_message;
  json_value_t *json_content;
  size_t i;

  if (turbo_prompt_message_validate_bind(message) != TURBO_PROMPT_OK) {
    return NULL;
  }

  role = turbo_runtime_data_bind_value_as_string(turbo_runtime_data_bind_object_get(message, "role"));
  content = turbo_runtime_data_bind_object_get(message, "content");
  tool_calls = turbo_runtime_data_bind_object_get(message, "tool_calls");
  tool_call_id = turbo_runtime_data_bind_object_get(message, "tool_call_id");
  if (!role || !content || strcmp(role, "system") == 0) {
    return NULL;
  }

  json_message = turbo_json_create_object();
  if (!json_message) {
    return NULL;
  }

  if (strcmp(role, "assistant") == 0) {
    turbo_json_object_set_string(json_message, "role", "assistant");
    if (tool_calls) {
      json_content = turbo_json_create_array();
      if (!json_content) {
        turbo_free_json(&json_message);
        return NULL;
      }
      if (turbo_runtime_data_bind_value_kind(content) == TURBO_RUNTIME_DATA_BIND_VALUE_STRING &&
          turbo_runtime_data_bind_value_as_string(content) &&
          turbo_runtime_data_bind_value_as_string(content)[0] != '\0') {
        json_value_t *text_part =
            turbo_prompt_content_text_part_create(turbo_runtime_data_bind_value_as_string(content));
        if (!text_part) {
          turbo_free_json(&json_content);
          turbo_free_json(&json_message);
          return NULL;
        }
        turbo_json_array_add(json_content, text_part);
      } else if (turbo_runtime_data_bind_value_kind(content) ==
                 TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
        size_t part_index;
        for (part_index = 0; part_index < turbo_runtime_data_bind_value_size(content); ++part_index) {
          json_value_t *json_part = turbo_prompt_content_part_to_json(
              turbo_runtime_data_bind_array_get(content, part_index));
          if (!json_part) {
            turbo_free_json(&json_content);
            turbo_free_json(&json_message);
            return NULL;
          }
          turbo_json_array_add(json_content, json_part);
        }
      }
      for (i = 0; i < turbo_runtime_data_bind_value_size(tool_calls); ++i) {
        json_value_t *tool_use_part = turbo_prompt_tool_call_to_anthropic_part(
            turbo_runtime_data_bind_array_get(tool_calls, i));
        if (!tool_use_part) {
          turbo_free_json(&json_content);
          turbo_free_json(&json_message);
          return NULL;
        }
        turbo_json_array_add(json_content, tool_use_part);
      }
    } else {
      json_content = turbo_prompt_content_bind_to_json(content);
      if (!json_content) {
        turbo_free_json(&json_message);
        return NULL;
      }
    }
  } else if (strcmp(role, "tool") == 0) {
    const char *tool_result_text = turbo_runtime_data_bind_value_as_string(content);

    if (!tool_call_id || !tool_result_text) {
      turbo_free_json(&json_message);
      return NULL;
    }
    turbo_json_object_set_string(json_message, "role", "user");
    json_content = turbo_json_create_array();
    if (!json_content) {
      turbo_free_json(&json_message);
      return NULL;
    }
    turbo_json_array_add(
        json_content, turbo_prompt_tool_result_part_create(
                          turbo_runtime_data_bind_value_as_string(tool_call_id), tool_result_text));
  } else {
    turbo_json_object_set_string(json_message, "role", "user");
    json_content = turbo_prompt_content_bind_to_json(content);
    if (!json_content) {
      turbo_free_json(&json_message);
      return NULL;
    }
  }

  turbo_json_object_add(json_message, "content", json_content);
  return json_message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_content_part_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *type_schema = turbo_prompt_schema_type_node("string");

  if (!schema || !properties || !required || !type_schema) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(type_schema);
    return NULL;
  }

  if (turbo_prompt_schema_object_put(properties, "type", type_schema) != 0 ||
      turbo_prompt_schema_object_put(properties, "text",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "id",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "name",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "input",
                                     turbo_prompt_schema_type_node("object")) != 0 ||
      turbo_prompt_schema_object_put(properties, "tool_use_id",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "content",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("type")) != 0 ||
      turbo_prompt_schema_object_put(schema, "type",
                                     turbo_prompt_schema_string_node("object")) != 0 ||
      turbo_prompt_schema_object_put(schema, "properties", properties) != 0 ||
      turbo_prompt_schema_object_put(schema, "required", required) != 0 ||
      turbo_prompt_schema_object_put(schema, "additionalProperties",
                                     turbo_runtime_data_bind_value_create_bool(0)) != 0) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(type_schema);
    return NULL;
  }

  return schema;
}

turbo_runtime_data_bind_value_t *turbo_prompt_tool_call_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *function = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *function_properties =
      turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *function_required =
      turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *arguments_variants =
      turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *arguments_schema = turbo_runtime_data_bind_value_create_object();

  if (!schema || !properties || !required || !function || !function_properties ||
      !function_required || !arguments_variants || !arguments_schema) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(function);
    turbo_runtime_data_bind_value_destroy(function_properties);
    turbo_runtime_data_bind_value_destroy(function_required);
    turbo_runtime_data_bind_value_destroy(arguments_variants);
    turbo_runtime_data_bind_value_destroy(arguments_schema);
    return NULL;
  }

  if (turbo_prompt_schema_array_append(arguments_variants,
                                       turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_array_append(arguments_variants,
                                       turbo_prompt_schema_type_node("object")) != 0 ||
      turbo_prompt_schema_object_put(arguments_schema, "anyOf", arguments_variants) != 0 ||
      turbo_prompt_schema_object_put(function_properties, "name",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(function_properties, "arguments",
                                     arguments_schema) != 0 ||
      turbo_prompt_schema_array_append(function_required,
                                       turbo_prompt_schema_string_node("name")) != 0 ||
      turbo_prompt_schema_array_append(function_required,
                                       turbo_prompt_schema_string_node("arguments")) != 0 ||
      turbo_prompt_schema_object_put(function, "type",
                                     turbo_prompt_schema_string_node("object")) != 0 ||
      turbo_prompt_schema_object_put(function, "properties", function_properties) != 0 ||
      turbo_prompt_schema_object_put(function, "required", function_required) != 0 ||
      turbo_prompt_schema_object_put(function, "additionalProperties",
                                     turbo_runtime_data_bind_value_create_bool(0)) != 0 ||
      turbo_prompt_schema_object_put(properties, "id",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "type",
                                     turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_object_put(properties, "function", function) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("id")) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("type")) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("function")) != 0 ||
      turbo_prompt_schema_object_put(schema, "type",
                                     turbo_prompt_schema_string_node("object")) != 0 ||
      turbo_prompt_schema_object_put(schema, "properties", properties) != 0 ||
      turbo_prompt_schema_object_put(schema, "required", required) != 0 ||
      turbo_prompt_schema_object_put(schema, "additionalProperties",
                                     turbo_runtime_data_bind_value_create_bool(0)) != 0) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(function);
    turbo_runtime_data_bind_value_destroy(function_properties);
    turbo_runtime_data_bind_value_destroy(function_required);
    turbo_runtime_data_bind_value_destroy(arguments_variants);
    turbo_runtime_data_bind_value_destroy(arguments_schema);
    return NULL;
  }

  return schema;
}

turbo_runtime_data_bind_value_t *turbo_prompt_message_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *properties = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *required = turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *role = turbo_prompt_schema_type_node("string");
  turbo_runtime_data_bind_value_t *content = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *content_variants =
      turbo_runtime_data_bind_value_create_array();
  turbo_runtime_data_bind_value_t *content_parts = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *content_part_schema = turbo_prompt_content_part_schema_bind();
  turbo_runtime_data_bind_value_t *tool_calls = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *tool_calls_items = turbo_prompt_tool_call_schema_bind();
  turbo_runtime_data_bind_value_t *tool_call_id = turbo_prompt_schema_type_node("string");

  if (!schema || !properties || !required || !role || !content || !tool_calls ||
      !tool_calls_items || !tool_call_id || !content_variants || !content_parts ||
      !content_part_schema) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(role);
    turbo_runtime_data_bind_value_destroy(content);
    turbo_runtime_data_bind_value_destroy(content_variants);
    turbo_runtime_data_bind_value_destroy(content_parts);
    turbo_runtime_data_bind_value_destroy(content_part_schema);
    turbo_runtime_data_bind_value_destroy(tool_calls);
    turbo_runtime_data_bind_value_destroy(tool_calls_items);
    turbo_runtime_data_bind_value_destroy(tool_call_id);
    return NULL;
  }

  if (turbo_prompt_schema_array_append(content_variants,
                                       turbo_prompt_schema_type_node("string")) != 0 ||
      turbo_prompt_schema_array_append(content_variants,
                                       turbo_prompt_schema_type_node("null")) != 0 ||
      turbo_prompt_schema_object_put(content_parts, "type",
                                     turbo_prompt_schema_string_node("array")) != 0 ||
      turbo_prompt_schema_object_put(content_parts, "items", content_part_schema) != 0 ||
      turbo_prompt_schema_array_append(content_variants, content_parts) != 0 ||
      turbo_prompt_schema_object_put(content, "anyOf", content_variants) != 0 ||
      turbo_prompt_schema_object_put(tool_calls, "type",
                                     turbo_prompt_schema_string_node("array")) != 0 ||
      turbo_prompt_schema_object_put(tool_calls, "items", tool_calls_items) != 0 ||
      turbo_prompt_schema_object_put(properties, "role", role) != 0 ||
      turbo_prompt_schema_object_put(properties, "content", content) != 0 ||
      turbo_prompt_schema_object_put(properties, "tool_calls", tool_calls) != 0 ||
      turbo_prompt_schema_object_put(properties, "tool_call_id", tool_call_id) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("role")) != 0 ||
      turbo_prompt_schema_array_append(required, turbo_prompt_schema_string_node("content")) != 0 ||
      turbo_prompt_schema_object_put(schema, "type", turbo_prompt_schema_string_node("object")) !=
          0 ||
      turbo_prompt_schema_object_put(schema, "properties", properties) != 0 ||
      turbo_prompt_schema_object_put(schema, "required", required) != 0 ||
      turbo_prompt_schema_object_put(schema, "additionalProperties",
                                     turbo_runtime_data_bind_value_create_bool(0)) != 0) {
    turbo_runtime_data_bind_value_destroy(schema);
    turbo_runtime_data_bind_value_destroy(properties);
    turbo_runtime_data_bind_value_destroy(required);
    turbo_runtime_data_bind_value_destroy(role);
    turbo_runtime_data_bind_value_destroy(content);
    turbo_runtime_data_bind_value_destroy(content_variants);
    turbo_runtime_data_bind_value_destroy(content_parts);
    turbo_runtime_data_bind_value_destroy(content_part_schema);
    turbo_runtime_data_bind_value_destroy(tool_calls);
    turbo_runtime_data_bind_value_destroy(tool_calls_items);
    turbo_runtime_data_bind_value_destroy(tool_call_id);
    return NULL;
  }

  return schema;
}

turbo_runtime_data_bind_value_t *turbo_prompt_messages_schema_bind(void) {
  turbo_runtime_data_bind_value_t *schema = NULL;
  turbo_runtime_data_bind_value_t *items = NULL;

  schema = turbo_runtime_data_bind_value_create_object();
  if (!schema) {
    return NULL;
  }

  items = turbo_prompt_message_schema_bind();
  if (!items ||
      turbo_prompt_schema_object_put(schema, "type", turbo_prompt_schema_string_node("array")) != 0 ||
      turbo_prompt_schema_object_put(schema, "items", items) != 0) {
    turbo_runtime_data_bind_value_destroy(items);
    turbo_runtime_data_bind_value_destroy(schema);
    return NULL;
  }

  return schema;
}

turbo_prompt_status_t
turbo_prompt_message_validate_bind(const turbo_runtime_data_bind_value_t *message) {
  const turbo_runtime_data_bind_value_t *role;
  const turbo_runtime_data_bind_value_t *content;
  const turbo_runtime_data_bind_value_t *tool_call_id;
  const turbo_runtime_data_bind_value_t *tool_calls;
  size_t i;

  if (!message ||
      turbo_runtime_data_bind_value_kind(message) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  role = turbo_runtime_data_bind_object_get(message, "role");
  content = turbo_runtime_data_bind_object_get(message, "content");
  if (!role || !content ||
      turbo_runtime_data_bind_value_kind(role) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (turbo_runtime_data_bind_value_kind(content) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING &&
      turbo_runtime_data_bind_value_kind(content) != TURBO_RUNTIME_DATA_BIND_VALUE_NULL &&
      turbo_runtime_data_bind_value_kind(content) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (turbo_runtime_data_bind_value_kind(content) == TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    for (i = 0; i < turbo_runtime_data_bind_value_size(content); ++i) {
      const turbo_runtime_data_bind_value_t *part =
          turbo_runtime_data_bind_array_get(content, i);
      const turbo_runtime_data_bind_value_t *part_type;
      const char *part_type_name;

      if (!part ||
          turbo_runtime_data_bind_value_kind(part) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      part_type = turbo_runtime_data_bind_object_get(part, "type");
      if (!part_type ||
          turbo_runtime_data_bind_value_kind(part_type) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      part_type_name = turbo_runtime_data_bind_value_as_string(part_type);
      if (!part_type_name) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      if (strcmp(part_type_name, "text") == 0) {
        const turbo_runtime_data_bind_value_t *text =
            turbo_runtime_data_bind_object_get(part, "text");
        if (!text ||
            turbo_runtime_data_bind_value_kind(text) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else if (strcmp(part_type_name, "tool_use") == 0) {
        const turbo_runtime_data_bind_value_t *id_value =
            turbo_runtime_data_bind_object_get(part, "id");
        const turbo_runtime_data_bind_value_t *name_value =
            turbo_runtime_data_bind_object_get(part, "name");
        const turbo_runtime_data_bind_value_t *input_value =
            turbo_runtime_data_bind_object_get(part, "input");

        if (!id_value || !name_value || !input_value ||
            turbo_runtime_data_bind_value_kind(id_value) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
            turbo_runtime_data_bind_value_kind(name_value) !=
                TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
            turbo_runtime_data_bind_value_kind(input_value) !=
                TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else if (strcmp(part_type_name, "tool_result") == 0) {
        const turbo_runtime_data_bind_value_t *tool_use_id =
            turbo_runtime_data_bind_object_get(part, "tool_use_id");
        const turbo_runtime_data_bind_value_t *part_content =
            turbo_runtime_data_bind_object_get(part, "content");

        if (!tool_use_id || !part_content ||
            turbo_runtime_data_bind_value_kind(tool_use_id) !=
                TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
            turbo_runtime_data_bind_value_kind(part_content) !=
                TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
    }
  }

  tool_call_id = turbo_runtime_data_bind_object_get(message, "tool_call_id");
  if (tool_call_id &&
      turbo_runtime_data_bind_value_kind(tool_call_id) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  tool_calls = turbo_runtime_data_bind_object_get(message, "tool_calls");
  if (tool_calls) {
    if (turbo_runtime_data_bind_value_kind(tool_calls) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
      return TURBO_PROMPT_INVALID_ARGUMENT;
    }
    for (i = 0; i < turbo_runtime_data_bind_value_size(tool_calls); ++i) {
      const turbo_runtime_data_bind_value_t *tool_call =
          turbo_runtime_data_bind_array_get(tool_calls, i);
      const turbo_runtime_data_bind_value_t *id_value;
      const turbo_runtime_data_bind_value_t *type_value;
      const turbo_runtime_data_bind_value_t *function_value;
      const turbo_runtime_data_bind_value_t *name_value;
      const turbo_runtime_data_bind_value_t *arguments_value;

      if (!tool_call ||
          turbo_runtime_data_bind_value_kind(tool_call) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      id_value = turbo_runtime_data_bind_object_get(tool_call, "id");
      type_value = turbo_runtime_data_bind_object_get(tool_call, "type");
      function_value = turbo_runtime_data_bind_object_get(tool_call, "function");
      if (!id_value || !type_value || !function_value ||
          turbo_runtime_data_bind_value_kind(id_value) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
          turbo_runtime_data_bind_value_kind(type_value) != TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
          turbo_runtime_data_bind_value_kind(function_value) != TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      name_value = turbo_runtime_data_bind_object_get(function_value, "name");
      arguments_value = turbo_runtime_data_bind_object_get(function_value, "arguments");
      if (!name_value || !arguments_value ||
          turbo_runtime_data_bind_value_kind(name_value) !=
              TURBO_RUNTIME_DATA_BIND_VALUE_STRING ||
          (turbo_runtime_data_bind_value_kind(arguments_value) !=
               TURBO_RUNTIME_DATA_BIND_VALUE_STRING &&
           turbo_runtime_data_bind_value_kind(arguments_value) !=
               TURBO_RUNTIME_DATA_BIND_VALUE_OBJECT)) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
    }
  }

  return TURBO_PROMPT_OK;
}

json_value_t *turbo_prompt_messages_to_openai_chat_json(
    const turbo_runtime_data_bind_value_t *messages) {
  json_value_t *json_messages;
  size_t i;

  if (!messages ||
      turbo_runtime_data_bind_value_kind(messages) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return NULL;
  }

  json_messages = turbo_json_create_array();
  if (!json_messages) {
    return NULL;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(messages); ++i) {
    json_value_t *json_message = turbo_prompt_message_to_openai_chat_json(
        turbo_runtime_data_bind_array_get(messages, i));
    if (!json_message) {
      turbo_free_json(&json_messages);
      return NULL;
    }
    turbo_json_array_add(json_messages, json_message);
  }

  return json_messages;
}

json_value_t *turbo_prompt_messages_to_openai_responses_json(
    const turbo_runtime_data_bind_value_t *messages) {
  return turbo_prompt_messages_to_openai_chat_json(messages);
}

turbo_prompt_status_t
turbo_prompt_messages_to_anthropic_json(
    const turbo_runtime_data_bind_value_t *messages, json_value_t **out_messages,
    char **out_system) {
  json_value_t *json_messages;
  char *system_text = NULL;
  size_t i;

  if (!messages || !out_messages ||
      turbo_runtime_data_bind_value_kind(messages) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  *out_messages = NULL;
  if (out_system) {
    *out_system = NULL;
  }

  json_messages = turbo_json_create_array();
  if (!json_messages) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  for (i = 0; i < turbo_runtime_data_bind_value_size(messages); ++i) {
    const turbo_runtime_data_bind_value_t *message =
        turbo_runtime_data_bind_array_get(messages, i);
    const char *role;

    if (turbo_prompt_message_validate_bind(message) != TURBO_PROMPT_OK) {
      turbo_free_json(&json_messages);
      tstr_free(system_text);
      return TURBO_PROMPT_INVALID_ARGUMENT;
    }

    role = turbo_runtime_data_bind_value_as_string(
        turbo_runtime_data_bind_object_get(message, "role"));
    if (role && strcmp(role, "system") == 0) {
      const char *content_text = turbo_runtime_data_bind_value_as_string(
          turbo_runtime_data_bind_object_get(message, "content"));
      char *joined;

      if (!content_text) {
        turbo_free_json(&json_messages);
        tstr_free(system_text);
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      joined = system_text ? turbo_prompt_join_strings(system_text, "\n\n", content_text)
                           : turbo_prompt_strdup(content_text);
      if (!joined) {
        turbo_free_json(&json_messages);
        tstr_free(system_text);
        return TURBO_PROMPT_OUT_OF_MEMORY;
      }
      tstr_free(system_text);
      system_text = joined;
      continue;
    }

    {
      json_value_t *json_message = turbo_prompt_message_to_anthropic_json(message);
      if (!json_message) {
        turbo_free_json(&json_messages);
        tstr_free(system_text);
        return TURBO_PROMPT_OUT_OF_MEMORY;
      }
      turbo_json_array_add(json_messages, json_message);
    }
  }

  *out_messages = json_messages;
  if (out_system) {
    if (system_text) {
      *out_system = tstr_to_cstr(system_text);
      tstr_free(system_text);
      if (!*out_system) {
        turbo_free_json(&json_messages);
        *out_messages = NULL;
        return TURBO_PROMPT_OUT_OF_MEMORY;
      }
    }
  } else {
    tstr_free(system_text);
  }
  return TURBO_PROMPT_OK;
}

static char *turbo_prompt_render_value(const json_value_t *value) {
  if (!value) {
    return turbo_prompt_strdup("");
  }

  if (turbo_json_type(value) == TURBO_JSON_STRING) {
    return turbo_prompt_strdup(turbo_json_string(value));
  }

  return turbo_json_serialize(value, NULL);
}

static char *turbo_prompt_render_bind_value(const turbo_runtime_data_bind_value_t *value) {
  json_value_t *json_value;
  char *rendered;

  if (!value) {
    return turbo_prompt_strdup("");
  }

  switch (turbo_runtime_data_bind_value_kind(value)) {
  case TURBO_RUNTIME_DATA_BIND_VALUE_STRING:
    return turbo_prompt_strdup(turbo_runtime_data_bind_value_as_string(value));
  case TURBO_RUNTIME_DATA_BIND_VALUE_BOOL:
    return turbo_prompt_strdup(
        turbo_runtime_data_bind_value_as_bool(value, 0) ? "true" : "false");
  case TURBO_RUNTIME_DATA_BIND_VALUE_INT64: {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%lld",
             (long long)turbo_runtime_data_bind_value_as_int64(value, 0));
    return turbo_prompt_strdup(buffer);
  }
  case TURBO_RUNTIME_DATA_BIND_VALUE_DOUBLE: {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.17g",
             turbo_runtime_data_bind_value_as_double(value, 0.0));
    return turbo_prompt_strdup(buffer);
  }
  case TURBO_RUNTIME_DATA_BIND_VALUE_NULL:
    return turbo_prompt_strdup("null");
  default:
    json_value = turbo_runtime_data_bind_value_to_json(value);
    if (!json_value) {
      return NULL;
    }
    rendered = turbo_json_serialize(json_value, NULL);
    turbo_free_json(&json_value);
    return rendered;
  }
}

static char *turbo_prompt_trimmed_key(const char *start, size_t len) {
  const char *left = start;
  const char *right = start + len;

  while (left < right && (*left == ' ' || *left == '\t' || *left == '\r' || *left == '\n')) {
    left++;
  }
  while (right > left &&
         (right[-1] == ' ' || right[-1] == '\t' || right[-1] == '\r' || right[-1] == '\n')) {
    right--;
  }

  len = (size_t)(right - left);
  if (len == 0) {
    return turbo_prompt_strdup("");
  }

  {
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
      return NULL;
    }
    memcpy(copy, left, len);
    copy[len] = '\0';
    return copy;
  }
}

char *turbo_prompt_render_template(const char *template_text, const json_value_t *input) {
  MUSTACHE_TEMPLATE *templ;
  MUSTACHE_STRING_RENDERER renderer;
  json_value_t *empty_input = NULL;
  const json_value_t *render_input = input;
  char *result = NULL;

  if (!template_text) {
    return turbo_prompt_cstrdup("");
  }

  templ = mustache_compile(template_text, strlen(template_text), NULL, NULL, 0);
  if (!templ) {
    return NULL;
  }

  if (!render_input) {
    empty_input = turbo_json_create_object();
    if (!empty_input) {
      mustache_release(templ);
      return NULL;
    }
    render_input = empty_input;
  }

  if (mustache_string_renderer_init(&renderer) == 0) {
    if (mustache_render_json(templ, (json_value_t *)render_input, &renderer.base, &renderer, NULL,
                             NULL) == 0) {
      result = mustache_string_renderer_get(&renderer);
    }
    mustache_string_renderer_free(&renderer);
  }

  turbo_free_json(&empty_input);
  mustache_release(templ);
  return result;
}

char *turbo_prompt_render_template_bind(const char *template_text,
                                        const turbo_runtime_data_bind_value_t *input) {
  json_value_t *json_input;
  char *result;

  if (!template_text) {
    return turbo_prompt_cstrdup("");
  }

  json_input = turbo_runtime_data_bind_value_to_json(input);
  result = turbo_prompt_render_template(template_text, json_input);
  turbo_free_json(&json_input);

  return result;
}

json_value_t *turbo_prompt_messages_create(void) { return turbo_json_create_array(); }

turbo_runtime_data_bind_value_t *turbo_prompt_messages_create_bind(void) {
  return turbo_runtime_data_bind_value_create_array();
}

json_value_t *turbo_prompt_message_create(const char *role, const char *content) {
  json_value_t *message = turbo_json_create_object();

  if (!message) {
    return NULL;
  }

  turbo_json_object_set_string(message, "role", role ? role : "user");
  turbo_json_object_set_string(message, "content", content ? content : "");
  return message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_message_create_bind(const char *role,
                                                                  const char *content) {
  turbo_runtime_data_bind_value_t *message = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *role_value = NULL;
  turbo_runtime_data_bind_value_t *content_value = NULL;

  if (!message) {
    return NULL;
  }

  role_value = turbo_runtime_data_bind_value_create_string(role ? role : "user");
  content_value = turbo_runtime_data_bind_value_create_string(content ? content : "");
  if (!role_value || !content_value ||
      turbo_runtime_data_bind_object_set(message, "role", role_value) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(message, "content", content_value) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(role_value);
    turbo_runtime_data_bind_value_destroy(content_value);
    turbo_runtime_data_bind_value_destroy(message);
    return NULL;
  }

  return message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_chat_assistant_message_create_bind(
    const char *content, turbo_runtime_data_bind_value_t *tool_calls) {
  turbo_runtime_data_bind_value_t *message =
      turbo_prompt_message_create_bind("assistant", content ? content : "");

  if (!message) {
    turbo_runtime_data_bind_value_destroy(tool_calls);
    return NULL;
  }
  if (!content || content[0] == '\0') {
    if (turbo_runtime_data_bind_object_set(message, "content",
                                           turbo_runtime_data_bind_value_create_null()) !=
        TURBO_RUNTIME_DATA_BIND_OK) {
      turbo_runtime_data_bind_value_destroy(tool_calls);
      turbo_runtime_data_bind_value_destroy(message);
      return NULL;
    }
  }
  if (tool_calls &&
      turbo_runtime_data_bind_object_set(message, "tool_calls", tool_calls) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(tool_calls);
    turbo_runtime_data_bind_value_destroy(message);
    return NULL;
  }
  return message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_chat_tool_message_create_bind(
    const char *tool_call_id, const char *content) {
  turbo_runtime_data_bind_value_t *message;

  if (!tool_call_id) {
    return NULL;
  }
  message = turbo_prompt_message_create_bind("tool", content ? content : "");
  if (!message) {
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(message, "tool_call_id",
                                         turbo_runtime_data_bind_value_create_string(tool_call_id)) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(message);
    return NULL;
  }
  return message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_chat_tool_call_create_bind(
    const char *id, const char *type, const char *name, const char *arguments) {
  turbo_runtime_data_bind_value_t *tool_call = turbo_runtime_data_bind_value_create_object();
  turbo_runtime_data_bind_value_t *function_object = turbo_runtime_data_bind_value_create_object();

  if (!id || !name || !arguments || !tool_call || !function_object) {
    turbo_runtime_data_bind_value_destroy(tool_call);
    turbo_runtime_data_bind_value_destroy(function_object);
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(tool_call, "id",
                                         turbo_runtime_data_bind_value_create_string(id)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          tool_call, "type",
          turbo_runtime_data_bind_value_create_string(type ? type : "function")) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(function_object, "name",
                                         turbo_runtime_data_bind_value_create_string(name)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          function_object, "arguments",
          turbo_runtime_data_bind_value_create_string(arguments)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(tool_call, "function", function_object) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(function_object);
    turbo_runtime_data_bind_value_destroy(tool_call);
    return NULL;
  }
  return tool_call;
}

turbo_runtime_data_bind_value_t *turbo_prompt_message_with_content_create_bind(
    const char *role, turbo_runtime_data_bind_value_t *content) {
  turbo_runtime_data_bind_value_t *message;

  if (!content) {
    return NULL;
  }
  message = turbo_runtime_data_bind_value_create_object();
  if (!message) {
    turbo_runtime_data_bind_value_destroy(content);
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(
          message, "role",
          turbo_runtime_data_bind_value_create_string(role ? role : "user")) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(message, "content", content) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(content);
    turbo_runtime_data_bind_value_destroy(message);
    return NULL;
  }
  return message;
}

turbo_runtime_data_bind_value_t *turbo_prompt_content_text_part_create_bind(const char *text) {
  turbo_runtime_data_bind_value_t *part = turbo_runtime_data_bind_value_create_object();

  if (!text || !part) {
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(part, "type",
                                         turbo_runtime_data_bind_value_create_string("text")) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(part, "text",
                                         turbo_runtime_data_bind_value_create_string(text)) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  return part;
}

turbo_runtime_data_bind_value_t *turbo_prompt_tool_use_part_create_bind(
    const char *id, const char *name, turbo_runtime_data_bind_value_t *input) {
  turbo_runtime_data_bind_value_t *part = turbo_runtime_data_bind_value_create_object();

  if (!id || !name || !input || !part) {
    turbo_runtime_data_bind_value_destroy(input);
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(part, "type",
                                         turbo_runtime_data_bind_value_create_string("tool_use")) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(part, "id",
                                         turbo_runtime_data_bind_value_create_string(id)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(part, "name",
                                         turbo_runtime_data_bind_value_create_string(name)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(part, "input", input) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(input);
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  return part;
}

turbo_runtime_data_bind_value_t *turbo_prompt_tool_result_part_create_bind(
    const char *tool_use_id, const char *content) {
  turbo_runtime_data_bind_value_t *part = turbo_runtime_data_bind_value_create_object();

  if (!tool_use_id || !content || !part) {
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  if (turbo_runtime_data_bind_object_set(
          part, "type",
          turbo_runtime_data_bind_value_create_string("tool_result")) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(
          part, "tool_use_id",
          turbo_runtime_data_bind_value_create_string(tool_use_id)) !=
          TURBO_RUNTIME_DATA_BIND_OK ||
      turbo_runtime_data_bind_object_set(part, "content",
                                         turbo_runtime_data_bind_value_create_string(content)) !=
          TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(part);
    return NULL;
  }
  return part;
}

json_value_t *turbo_prompt_chat_assistant_message_create(const char *content,
                                                         json_value_t *tool_calls) {
  json_value_t *message = turbo_prompt_message_create("assistant", content ? content : "");

  if (!message) {
    turbo_free_json(&tool_calls);
    return NULL;
  }

  if (!content || content[0] == '\0') {
    turbo_json_object_set_null(message, "content");
  }

  if (tool_calls) {
    turbo_json_object_add(message, "tool_calls", tool_calls);
  }

  return message;
}

json_value_t *turbo_prompt_chat_tool_message_create(const char *tool_call_id,
                                                    const char *content) {
  json_value_t *message;

  if (!tool_call_id) {
    return NULL;
  }

  message = turbo_prompt_message_create("tool", content ? content : "");
  if (!message) {
    return NULL;
  }

  turbo_json_object_set_string(message, "tool_call_id", tool_call_id);
  return message;
}

json_value_t *turbo_prompt_chat_tool_call_create(const char *id, const char *type,
                                                 const char *name,
                                                 const char *arguments) {
  json_value_t *tool_call;
  json_value_t *function_object;

  if (!id || !name || !arguments) {
    return NULL;
  }

  tool_call = turbo_json_create_object();
  function_object = turbo_json_create_object();
  if (!tool_call || !function_object) {
    turbo_free_json(&tool_call);
    turbo_free_json(&function_object);
    return NULL;
  }

  turbo_json_object_set_string(tool_call, "id", id);
  turbo_json_object_set_string(tool_call, "type", type ? type : "function");
  turbo_json_object_set_string(function_object, "name", name);
  turbo_json_object_set_string(function_object, "arguments", arguments);
  turbo_json_object_add(tool_call, "function", function_object);
  return tool_call;
}

json_value_t *turbo_prompt_message_with_content_create(const char *role, json_value_t *content) {
  json_value_t *message;

  if (!content) {
    turbo_free_json(&content);
    return NULL;
  }

  message = turbo_json_create_object();
  if (!message) {
    turbo_free_json(&content);
    return NULL;
  }

  turbo_json_object_set_string(message, "role", role ? role : "user");
  turbo_json_object_add(message, "content", content);
  return message;
}

json_value_t *turbo_prompt_content_text_part_create(const char *text) {
  json_value_t *part;

  if (!text) {
    return NULL;
  }

  part = turbo_json_create_object();
  if (!part) {
    return NULL;
  }

  turbo_json_object_set_string(part, "type", "text");
  turbo_json_object_set_string(part, "text", text);
  return part;
}

json_value_t *turbo_prompt_tool_use_part_create(const char *id, const char *name,
                                                json_value_t *input) {
  json_value_t *part;

  if (!id || !name || !input) {
    turbo_free_json(&input);
    return NULL;
  }

  part = turbo_json_create_object();
  if (!part) {
    turbo_free_json(&input);
    return NULL;
  }

  turbo_json_object_set_string(part, "type", "tool_use");
  turbo_json_object_set_string(part, "id", id);
  turbo_json_object_set_string(part, "name", name);
  turbo_json_object_add(part, "input", input);
  return part;
}

json_value_t *turbo_prompt_tool_result_part_create(const char *tool_use_id,
                                                   const char *content) {
  json_value_t *part;

  if (!tool_use_id || !content) {
    return NULL;
  }

  part = turbo_json_create_object();
  if (!part) {
    return NULL;
  }

  turbo_json_object_set_string(part, "type", "tool_result");
  turbo_json_object_set_string(part, "tool_use_id", tool_use_id);
  turbo_json_object_set_string(part, "content", content);
  return part;
}

turbo_prompt_status_t turbo_prompt_messages_append(json_value_t *messages, const char *role,
                                                   const char *content) {
  json_value_t *message;

  if (!messages || turbo_json_type(messages) != TURBO_JSON_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  message = turbo_prompt_message_create(role, content);
  if (!message) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  turbo_json_array_add(messages, message);
  return TURBO_PROMPT_OK;
}

turbo_prompt_status_t
turbo_prompt_messages_append_bind(turbo_runtime_data_bind_value_t *messages, const char *role,
                                  const char *content) {
  turbo_runtime_data_bind_value_t *message;

  if (!messages ||
      turbo_runtime_data_bind_value_kind(messages) != TURBO_RUNTIME_DATA_BIND_VALUE_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  message = turbo_prompt_message_create_bind(role, content);
  if (!message) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }
  if (turbo_prompt_message_validate_bind(message) != TURBO_PROMPT_OK) {
    turbo_runtime_data_bind_value_destroy(message);
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (turbo_runtime_data_bind_array_append(messages, message) != TURBO_RUNTIME_DATA_BIND_OK) {
    turbo_runtime_data_bind_value_destroy(message);
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  return TURBO_PROMPT_OK;
}
