#include "turbo_prompt.h"
#include <json_parser.h>
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

static json_value_t *turbo_prompt_schema_string_node(const char *value);
static int turbo_prompt_schema_object_put(json_value_t *object, const char *key,
                                          json_value_t *value);
static json_value_t *turbo_prompt_schema_type_node(const char *value);

static json_value_t *turbo_prompt_schema_string_node(const char *value) {
  return json_create_string(value);
}

static int turbo_prompt_schema_object_put(json_value_t *object, const char *key,
                                          json_value_t *value) {
  if (!object || !key || !value) {
    turbo_runtime_json_destroy(value);
    return -1;
  }

  if (turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(value);
    return -1;
  }

  return 0;
}

static json_value_t *turbo_prompt_schema_type_node(const char *value) {
  json_value_t *schema = json_create_object();

  if (!schema) {
    return NULL;
  }
  if (turbo_prompt_schema_object_put(schema, "type", turbo_prompt_schema_string_node(value)) != 0) {
    turbo_runtime_json_destroy(schema);
    return NULL;
  }
  return schema;
}

static int turbo_prompt_schema_array_append(json_value_t *array,
                                            json_value_t *value) {
  if (!array || !value) {
    turbo_runtime_json_destroy(value);
    return -1;
  }
  if (turbo_runtime_json_array_append(array, value) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(value);
    return -1;
  }
  return 0;
}



static json_value_t *turbo_prompt_content_part_to_json(
    const json_value_t *part) {
  const char *type_name;
  json_value_t *json_part;

  if (!part || json_type(part) != JSON_OBJECT) {
    return NULL;
  }

  type_name = turbo_runtime_json_value_as_string(
      json_object_get(part, "type"));
  if (!type_name) {
    return NULL;
  }

  json_part = json_clone(part);
  if (!json_part) {
    return NULL;
  }

  if (strcmp(type_name, "tool_use") == 0) {
    const json_value_t *input =
        json_object_get(part, "input");
    json_value_t *input_json;

    if (!input) {
      json_free(json_part); json_part = NULL;
      return NULL;
    }
    input_json = json_clone(input);
    if (!input_json) {
      json_free(json_part); json_part = NULL;
      return NULL;
    }
    json_object_add(json_part, "input", input_json);
  }

  return json_part;
}

static json_value_t *turbo_prompt_content_json_value_to_json(
    const json_value_t *content) {
  size_t i;
  json_value_t *json_content;

  if (!content) {
    return NULL;
  }

  switch (json_type(content)) {
    case JSON_NULL:
      return json_create_null();
    case JSON_STRING:
      return json_create_string(turbo_runtime_json_value_as_string(content));
    case JSON_ARRAY:
      json_content = json_create_array();
      if (!json_content) {
        return NULL;
      }
      for (i = 0; i < turbo_runtime_json_value_size(content); ++i) {
        const json_value_t *part =
            json_array_get(content, i);
        json_value_t *json_part = turbo_prompt_content_part_to_json(part);

        if (!json_part) {
          json_free(json_content); json_content = NULL;
          return NULL;
        }
        json_array_add(json_content, json_part);
      }
      return json_content;
    default:
      return NULL;
  }
}

static json_value_t *turbo_prompt_tool_call_to_json(
    const json_value_t *tool_call) {
  return json_clone(tool_call);
}

static json_value_t *turbo_prompt_message_to_openai_chat_json(
    const json_value_t *message) {
  const char *role;
  const json_value_t *content;
  const json_value_t *tool_calls;
  const json_value_t *tool_call_id;
  json_value_t *json_message;
  json_value_t *json_content;
  size_t i;

  if (turbo_prompt_message_validate_json_value(message) != TURBO_PROMPT_OK) {
    return NULL;
  }

  role = turbo_runtime_json_value_as_string(json_object_get(message, "role"));
  content = json_object_get(message, "content");
  tool_calls = json_object_get(message, "tool_calls");
  tool_call_id = json_object_get(message, "tool_call_id");
  if (!role || !content) {
    return NULL;
  }

  json_message = json_create_object();
  if (!json_message) {
    return NULL;
  }

  json_object_set_string(json_message, "role", role);
  json_content = turbo_prompt_content_json_value_to_json(content);
  if (!json_content) {
    json_free(json_message); json_message = NULL;
    return NULL;
  }
  json_object_add(json_message, "content", json_content);

  if (tool_call_id) {
    json_object_set_string(
        json_message, "tool_call_id", turbo_runtime_json_value_as_string(tool_call_id));
  }

  if (tool_calls) {
    json_value_t *json_tool_calls = json_create_array();
    if (!json_tool_calls) {
      json_free(json_message); json_message = NULL;
      return NULL;
    }
    for (i = 0; i < turbo_runtime_json_value_size(tool_calls); ++i) {
      json_value_t *json_tool_call = turbo_prompt_tool_call_to_json(
          json_array_get(tool_calls, i));
      if (!json_tool_call) {
        json_free(json_tool_calls); json_tool_calls = NULL;
        json_free(json_message); json_message = NULL;
        return NULL;
      }
      json_array_add(json_tool_calls, json_tool_call);
    }
    json_object_add(json_message, "tool_calls", json_tool_calls);
  }

  return json_message;
}

static json_value_t *turbo_prompt_tool_call_to_anthropic_part(
    const json_value_t *tool_call) {
  const json_value_t *function_value;
  const json_value_t *arguments_value;
  json_value_t *part;
  json_value_t *input_json;

  if (!tool_call) {
    return NULL;
  }

  function_value = json_object_get(tool_call, "function");
  arguments_value = function_value
                        ? json_object_get(function_value, "arguments")
                        : NULL;
  if (!function_value || !arguments_value) {
    return NULL;
  }

  part = json_create_object();
  if (!part) {
    return NULL;
  }

  json_object_set_string(part, "type", "tool_use");
  json_object_set_string(
      part, "id",
      turbo_runtime_json_value_as_string(json_object_get(tool_call, "id")));
  json_object_set_string(
      part, "name",
      turbo_runtime_json_value_as_string(json_object_get(function_value, "name")));

  if (json_type(arguments_value) == JSON_STRING) {
    input_json = json_parse(turbo_runtime_json_value_as_string(arguments_value),
                            strlen(turbo_runtime_json_value_as_string(arguments_value)));
    if (!input_json) {
      input_json = json_create_object();
    }
  } else {
    input_json = json_clone(arguments_value);
  }
  if (!input_json) {
    json_free(part); part = NULL;
    return NULL;
  }
  json_object_add(part, "input", input_json);
  return part;
}

static json_value_t *turbo_prompt_message_to_anthropic_json(
    const json_value_t *message) {
  const char *role;
  const json_value_t *content;
  const json_value_t *tool_calls;
  const json_value_t *tool_call_id;
  json_value_t *json_message;
  json_value_t *json_content;
  size_t i;

  if (turbo_prompt_message_validate_json_value(message) != TURBO_PROMPT_OK) {
    return NULL;
  }

  role = turbo_runtime_json_value_as_string(json_object_get(message, "role"));
  content = json_object_get(message, "content");
  tool_calls = json_object_get(message, "tool_calls");
  tool_call_id = json_object_get(message, "tool_call_id");
  if (!role || !content || strcmp(role, "system") == 0) {
    return NULL;
  }

  json_message = json_create_object();
  if (!json_message) {
    return NULL;
  }

  if (strcmp(role, "assistant") == 0) {
    json_object_set_string(json_message, "role", "assistant");
    if (tool_calls) {
      json_content = json_create_array();
      if (!json_content) {
        json_free(json_message); json_message = NULL;
        return NULL;
      }
      if (json_type(content) == JSON_STRING &&
          turbo_runtime_json_value_as_string(content) &&
          turbo_runtime_json_value_as_string(content)[0] != '\0') {
        json_value_t *text_part =
            turbo_prompt_content_text_part_create(turbo_runtime_json_value_as_string(content));
        if (!text_part) {
          json_free(json_content); json_content = NULL;
          json_free(json_message); json_message = NULL;
          return NULL;
        }
        json_array_add(json_content, text_part);
      } else if (json_type(content) ==
                 JSON_ARRAY) {
        size_t part_index;
        for (part_index = 0; part_index < turbo_runtime_json_value_size(content); ++part_index) {
          json_value_t *json_part = turbo_prompt_content_part_to_json(
              json_array_get(content, part_index));
          if (!json_part) {
            json_free(json_content); json_content = NULL;
            json_free(json_message); json_message = NULL;
            return NULL;
          }
          json_array_add(json_content, json_part);
        }
      }
      for (i = 0; i < turbo_runtime_json_value_size(tool_calls); ++i) {
        json_value_t *tool_use_part = turbo_prompt_tool_call_to_anthropic_part(
            json_array_get(tool_calls, i));
        if (!tool_use_part) {
          json_free(json_content); json_content = NULL;
          json_free(json_message); json_message = NULL;
          return NULL;
        }
        json_array_add(json_content, tool_use_part);
      }
    } else {
      json_content = turbo_prompt_content_json_value_to_json(content);
      if (!json_content) {
        json_free(json_message); json_message = NULL;
        return NULL;
      }
    }
  } else if (strcmp(role, "tool") == 0) {
    const char *tool_result_text = turbo_runtime_json_value_as_string(content);

    if (!tool_call_id || !tool_result_text) {
      json_free(json_message); json_message = NULL;
      return NULL;
    }
    json_object_set_string(json_message, "role", "user");
    json_content = json_create_array();
    if (!json_content) {
      json_free(json_message); json_message = NULL;
      return NULL;
    }
    json_array_add(
        json_content, turbo_prompt_tool_result_part_create(
                          turbo_runtime_json_value_as_string(tool_call_id), tool_result_text));
  } else {
    json_object_set_string(json_message, "role", "user");
    json_content = turbo_prompt_content_json_value_to_json(content);
    if (!json_content) {
      json_free(json_message); json_message = NULL;
      return NULL;
    }
  }

  json_object_add(json_message, "content", json_content);
  return json_message;
}

json_value_t *turbo_prompt_content_part_schema_json_value(void) {
  json_value_t *schema = json_create_object();
  json_value_t *properties = json_create_object();
  json_value_t *required = json_create_array();
  json_value_t *type_schema = turbo_prompt_schema_type_node("string");

  if (!schema || !properties || !required || !type_schema) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(type_schema);
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
                                     json_create_bool(0)) != 0) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(type_schema);
    return NULL;
  }

  return schema;
}

json_value_t *turbo_prompt_tool_call_schema_json_value(void) {
  json_value_t *schema = json_create_object();
  json_value_t *properties = json_create_object();
  json_value_t *required = json_create_array();
  json_value_t *function = json_create_object();
  json_value_t *function_properties =
      json_create_object();
  json_value_t *function_required =
      json_create_array();
  json_value_t *arguments_variants =
      json_create_array();
  json_value_t *arguments_schema = json_create_object();

  if (!schema || !properties || !required || !function || !function_properties ||
      !function_required || !arguments_variants || !arguments_schema) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(function);
    turbo_runtime_json_destroy(function_properties);
    turbo_runtime_json_destroy(function_required);
    turbo_runtime_json_destroy(arguments_variants);
    turbo_runtime_json_destroy(arguments_schema);
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
                                     json_create_bool(0)) != 0 ||
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
                                     json_create_bool(0)) != 0) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(function);
    turbo_runtime_json_destroy(function_properties);
    turbo_runtime_json_destroy(function_required);
    turbo_runtime_json_destroy(arguments_variants);
    turbo_runtime_json_destroy(arguments_schema);
    return NULL;
  }

  return schema;
}

json_value_t *turbo_prompt_message_schema_json_value(void) {
  json_value_t *schema = json_create_object();
  json_value_t *properties = json_create_object();
  json_value_t *required = json_create_array();
  json_value_t *role = turbo_prompt_schema_type_node("string");
  json_value_t *content = json_create_object();
  json_value_t *content_variants =
      json_create_array();
  json_value_t *content_parts = json_create_object();
  json_value_t *content_part_schema = turbo_prompt_content_part_schema_json_value();
  json_value_t *tool_calls = json_create_object();
  json_value_t *tool_calls_items = turbo_prompt_tool_call_schema_json_value();
  json_value_t *tool_call_id = turbo_prompt_schema_type_node("string");

  if (!schema || !properties || !required || !role || !content || !tool_calls ||
      !tool_calls_items || !tool_call_id || !content_variants || !content_parts ||
      !content_part_schema) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(role);
    turbo_runtime_json_destroy(content);
    turbo_runtime_json_destroy(content_variants);
    turbo_runtime_json_destroy(content_parts);
    turbo_runtime_json_destroy(content_part_schema);
    turbo_runtime_json_destroy(tool_calls);
    turbo_runtime_json_destroy(tool_calls_items);
    turbo_runtime_json_destroy(tool_call_id);
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
                                     json_create_bool(0)) != 0) {
    turbo_runtime_json_destroy(schema);
    turbo_runtime_json_destroy(properties);
    turbo_runtime_json_destroy(required);
    turbo_runtime_json_destroy(role);
    turbo_runtime_json_destroy(content);
    turbo_runtime_json_destroy(content_variants);
    turbo_runtime_json_destroy(content_parts);
    turbo_runtime_json_destroy(content_part_schema);
    turbo_runtime_json_destroy(tool_calls);
    turbo_runtime_json_destroy(tool_calls_items);
    turbo_runtime_json_destroy(tool_call_id);
    return NULL;
  }

  return schema;
}

json_value_t *turbo_prompt_messages_schema_json_value(void) {
  json_value_t *schema = NULL;
  json_value_t *items = NULL;

  schema = json_create_object();
  if (!schema) {
    return NULL;
  }

  items = turbo_prompt_message_schema_json_value();
  if (!items ||
      turbo_prompt_schema_object_put(schema, "type", turbo_prompt_schema_string_node("array")) != 0 ||
      turbo_prompt_schema_object_put(schema, "items", items) != 0) {
    turbo_runtime_json_destroy(items);
    turbo_runtime_json_destroy(schema);
    return NULL;
  }

  return schema;
}

turbo_prompt_status_t
turbo_prompt_message_validate_json_value(const json_value_t *message) {
  const json_value_t *role;
  const json_value_t *content;
  const json_value_t *tool_call_id;
  const json_value_t *tool_calls;
  size_t i;

  if (!message ||
      json_type(message) != JSON_OBJECT) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  role = json_object_get(message, "role");
  content = json_object_get(message, "content");
  if (!role || !content ||
      json_type(role) != JSON_STRING) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (json_type(content) != JSON_STRING &&
      json_type(content) != JSON_NULL &&
      json_type(content) != JSON_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (json_type(content) == JSON_ARRAY) {
    for (i = 0; i < turbo_runtime_json_value_size(content); ++i) {
      const json_value_t *part =
          json_array_get(content, i);
      const json_value_t *part_type;
      const char *part_type_name;

      if (!part ||
          json_type(part) != JSON_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      part_type = json_object_get(part, "type");
      if (!part_type ||
          json_type(part_type) != JSON_STRING) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      part_type_name = turbo_runtime_json_value_as_string(part_type);
      if (!part_type_name) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      if (strcmp(part_type_name, "text") == 0) {
        const json_value_t *text =
            json_object_get(part, "text");
        if (!text ||
            json_type(text) != JSON_STRING) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else if (strcmp(part_type_name, "tool_use") == 0) {
        const json_value_t *id_value =
            json_object_get(part, "id");
        const json_value_t *name_value =
            json_object_get(part, "name");
        const json_value_t *input_value =
            json_object_get(part, "input");

        if (!id_value || !name_value || !input_value ||
            json_type(id_value) != JSON_STRING ||
            json_type(name_value) !=
                JSON_STRING ||
            json_type(input_value) !=
                JSON_OBJECT) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else if (strcmp(part_type_name, "tool_result") == 0) {
        const json_value_t *tool_use_id =
            json_object_get(part, "tool_use_id");
        const json_value_t *part_content =
            json_object_get(part, "content");

        if (!tool_use_id || !part_content ||
            json_type(tool_use_id) !=
                JSON_STRING ||
            json_type(part_content) !=
                JSON_STRING) {
          return TURBO_PROMPT_INVALID_ARGUMENT;
        }
      } else {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
    }
  }

  tool_call_id = json_object_get(message, "tool_call_id");
  if (tool_call_id &&
      json_type(tool_call_id) != JSON_STRING) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  tool_calls = json_object_get(message, "tool_calls");
  if (tool_calls) {
    if (json_type(tool_calls) != JSON_ARRAY) {
      return TURBO_PROMPT_INVALID_ARGUMENT;
    }
    for (i = 0; i < turbo_runtime_json_value_size(tool_calls); ++i) {
      const json_value_t *tool_call =
          json_array_get(tool_calls, i);
      const json_value_t *id_value;
      const json_value_t *type_value;
      const json_value_t *function_value;
      const json_value_t *name_value;
      const json_value_t *arguments_value;

      if (!tool_call ||
          json_type(tool_call) != JSON_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      id_value = json_object_get(tool_call, "id");
      type_value = json_object_get(tool_call, "type");
      function_value = json_object_get(tool_call, "function");
      if (!id_value || !type_value || !function_value ||
          json_type(id_value) != JSON_STRING ||
          json_type(type_value) != JSON_STRING ||
          json_type(function_value) != JSON_OBJECT) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      name_value = json_object_get(function_value, "name");
      arguments_value = json_object_get(function_value, "arguments");
      if (!name_value || !arguments_value ||
          json_type(name_value) !=
              JSON_STRING ||
          (json_type(arguments_value) !=
               JSON_STRING &&
           json_type(arguments_value) !=
               JSON_OBJECT)) {
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
    }
  }

  return TURBO_PROMPT_OK;
}

json_value_t *turbo_prompt_messages_to_openai_chat_json(
    const json_value_t *messages) {
  json_value_t *json_messages;
  size_t i;

  if (!messages ||
      json_type(messages) != JSON_ARRAY) {
    return NULL;
  }

  json_messages = json_create_array();
  if (!json_messages) {
    return NULL;
  }

  for (i = 0; i < turbo_runtime_json_value_size(messages); ++i) {
    json_value_t *json_message = turbo_prompt_message_to_openai_chat_json(
        json_array_get(messages, i));
    if (!json_message) {
      json_free(json_messages); json_messages = NULL;
      return NULL;
    }
    json_array_add(json_messages, json_message);
  }

  return json_messages;
}

json_value_t *turbo_prompt_messages_to_openai_responses_json(
    const json_value_t *messages) {
  return turbo_prompt_messages_to_openai_chat_json(messages);
}

turbo_prompt_status_t
turbo_prompt_messages_to_anthropic_json(
    const json_value_t *messages, json_value_t **out_messages,
    char **out_system) {
  json_value_t *json_messages;
  char *system_text = NULL;
  size_t i;

  if (!messages || !out_messages ||
      json_type(messages) != JSON_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  *out_messages = NULL;
  if (out_system) {
    *out_system = NULL;
  }

  json_messages = json_create_array();
  if (!json_messages) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  for (i = 0; i < turbo_runtime_json_value_size(messages); ++i) {
    const json_value_t *message =
        json_array_get(messages, i);
    const char *role;

    if (turbo_prompt_message_validate_json_value(message) != TURBO_PROMPT_OK) {
      json_free(json_messages); json_messages = NULL;
      tstr_free(system_text);
      return TURBO_PROMPT_INVALID_ARGUMENT;
    }

    role = turbo_runtime_json_value_as_string(
        json_object_get(message, "role"));
    if (role && strcmp(role, "system") == 0) {
      const char *content_text = turbo_runtime_json_value_as_string(
          json_object_get(message, "content"));
      char *joined;

      if (!content_text) {
        json_free(json_messages); json_messages = NULL;
        tstr_free(system_text);
        return TURBO_PROMPT_INVALID_ARGUMENT;
      }
      joined = system_text ? turbo_prompt_join_strings(system_text, "\n\n", content_text)
                           : turbo_prompt_strdup(content_text);
      if (!joined) {
        json_free(json_messages); json_messages = NULL;
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
        json_free(json_messages); json_messages = NULL;
        tstr_free(system_text);
        return TURBO_PROMPT_OUT_OF_MEMORY;
      }
      json_array_add(json_messages, json_message);
    }
  }

  *out_messages = json_messages;
  if (out_system) {
    if (system_text) {
      *out_system = tstr_to_cstr(system_text);
      tstr_free(system_text);
      if (!*out_system) {
        json_free(json_messages); json_messages = NULL;
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

  if (json_type(value) == JSON_STRING) {
    return turbo_prompt_strdup(json_string(value));
  }

  return json_serialize(value, NULL);
}

static char *turbo_prompt_render_json_value(const json_value_t *value) {
  return turbo_prompt_render_value(value);
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
  json_value_t *normalized_input = NULL;
  const json_value_t *render_input = input;
  char *normalized_text = NULL;
  char *result = NULL;

  if (!template_text) {
    return turbo_prompt_cstrdup("");
  }

  templ = mustache_compile(template_text, strlen(template_text), NULL, NULL, 0);
  if (!templ) {
    return NULL;
  }

  if (!render_input) {
    empty_input = json_create_object();
    if (!empty_input) {
      mustache_release(templ);
      return NULL;
    }
    render_input = empty_input;
  }

  /* Parsed number nodes retain the lexical form required by Mustache's JSON adapter. */
  normalized_text = json_serialize(render_input, NULL);
  if (normalized_text) {
    normalized_input = json_parse(normalized_text, strlen(normalized_text));
  }
  if (!normalized_text || !normalized_input) {
    free(normalized_text);
    json_free(empty_input); empty_input = NULL;
    mustache_release(templ);
    return NULL;
  }

  if (mustache_string_renderer_init(&renderer) == 0) {
    if (mustache_render_json(templ, normalized_input, &renderer.base, &renderer, NULL,
                             NULL) == 0) {
      result = mustache_string_renderer_get(&renderer);
    }
    mustache_string_renderer_free(&renderer);
  }

  json_free(normalized_input); normalized_input = NULL;
  free(normalized_text);
  json_free(empty_input); empty_input = NULL;
  mustache_release(templ);
  return result;
}

char *turbo_prompt_render_template_json_value(const char *template_text,
                                        const json_value_t *input) {
  json_value_t *json_input;
  char *result;

  if (!template_text) {
    return turbo_prompt_cstrdup("");
  }

  json_input = json_clone(input);
  result = turbo_prompt_render_template(template_text, json_input);
  json_free(json_input); json_input = NULL;

  return result;
}

json_value_t *turbo_prompt_messages_create(void) { return json_create_array(); }

json_value_t *turbo_prompt_messages_create_json_value(void) {
  return json_create_array();
}

json_value_t *turbo_prompt_message_create(const char *role, const char *content) {
  json_value_t *message = json_create_object();

  if (!message) {
    return NULL;
  }

  json_object_set_string(message, "role", role ? role : "user");
  json_object_set_string(message, "content", content ? content : "");
  return message;
}

json_value_t *turbo_prompt_message_create_json_value(const char *role,
                                                                  const char *content) {
  json_value_t *message = json_create_object();
  json_value_t *role_value = NULL;
  json_value_t *content_value = NULL;

  if (!message) {
    return NULL;
  }

  role_value = json_create_string(role ? role : "user");
  content_value = json_create_string(content ? content : "");
  if (!role_value || !content_value ||
      turbo_runtime_json_object_set(message, "role", role_value) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(message, "content", content_value) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(role_value);
    turbo_runtime_json_destroy(content_value);
    turbo_runtime_json_destroy(message);
    return NULL;
  }

  return message;
}

json_value_t *turbo_prompt_chat_assistant_message_create_json_value(
    const char *content, json_value_t *tool_calls) {
  json_value_t *message =
      turbo_prompt_message_create_json_value("assistant", content ? content : "");

  if (!message) {
    turbo_runtime_json_destroy(tool_calls);
    return NULL;
  }
  if (!content || content[0] == '\0') {
    if (turbo_runtime_json_object_set(message, "content",
                                           json_create_null()) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(tool_calls);
      turbo_runtime_json_destroy(message);
      return NULL;
    }
  }
  if (tool_calls &&
      turbo_runtime_json_object_set(message, "tool_calls", tool_calls) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(tool_calls);
    turbo_runtime_json_destroy(message);
    return NULL;
  }
  return message;
}

json_value_t *turbo_prompt_chat_tool_message_create_json_value(
    const char *tool_call_id, const char *content) {
  json_value_t *message;

  if (!tool_call_id) {
    return NULL;
  }
  message = turbo_prompt_message_create_json_value("tool", content ? content : "");
  if (!message) {
    return NULL;
  }
  if (turbo_runtime_json_object_set(message, "tool_call_id",
                                         json_create_string(tool_call_id)) !=
      TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(message);
    return NULL;
  }
  return message;
}

json_value_t *turbo_prompt_chat_tool_call_create_json_value(
    const char *id, const char *type, const char *name, const char *arguments) {
  json_value_t *tool_call = json_create_object();
  json_value_t *function_object = json_create_object();

  if (!id || !name || !arguments || !tool_call || !function_object) {
    turbo_runtime_json_destroy(tool_call);
    turbo_runtime_json_destroy(function_object);
    return NULL;
  }
  if (turbo_runtime_json_object_set(tool_call, "id",
                                         json_create_string(id)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          tool_call, "type",
          json_create_string(type ? type : "function")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(function_object, "name",
                                         json_create_string(name)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          function_object, "arguments",
          json_create_string(arguments)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(tool_call, "function", function_object) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(function_object);
    turbo_runtime_json_destroy(tool_call);
    return NULL;
  }
  return tool_call;
}

json_value_t *turbo_prompt_message_with_content_create_json_value(
    const char *role, json_value_t *content) {
  json_value_t *message;

  if (!content) {
    return NULL;
  }
  message = json_create_object();
  if (!message) {
    turbo_runtime_json_destroy(content);
    return NULL;
  }
  if (turbo_runtime_json_object_set(
          message, "role",
          json_create_string(role ? role : "user")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(message, "content", content) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(content);
    turbo_runtime_json_destroy(message);
    return NULL;
  }
  return message;
}

json_value_t *turbo_prompt_content_text_part_create_json_value(const char *text) {
  json_value_t *part = json_create_object();

  if (!text || !part) {
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  if (turbo_runtime_json_object_set(part, "type",
                                         json_create_string("text")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(part, "text",
                                         json_create_string(text)) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  return part;
}

json_value_t *turbo_prompt_tool_use_part_create_json_value(
    const char *id, const char *name, json_value_t *input) {
  json_value_t *part = json_create_object();

  if (!id || !name || !input || !part) {
    turbo_runtime_json_destroy(input);
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  if (turbo_runtime_json_object_set(part, "type",
                                         json_create_string("tool_use")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(part, "id",
                                         json_create_string(id)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(part, "name",
                                         json_create_string(name)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(part, "input", input) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(input);
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  return part;
}

json_value_t *turbo_prompt_tool_result_part_create_json_value(
    const char *tool_use_id, const char *content) {
  json_value_t *part = json_create_object();

  if (!tool_use_id || !content || !part) {
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  if (turbo_runtime_json_object_set(
          part, "type",
          json_create_string("tool_result")) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(
          part, "tool_use_id",
          json_create_string(tool_use_id)) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set(part, "content",
                                         json_create_string(content)) !=
          TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(part);
    return NULL;
  }
  return part;
}

json_value_t *turbo_prompt_chat_assistant_message_create(const char *content,
                                                         json_value_t *tool_calls) {
  json_value_t *message = turbo_prompt_message_create("assistant", content ? content : "");

  if (!message) {
    json_free(tool_calls); tool_calls = NULL;
    return NULL;
  }

  if (!content || content[0] == '\0') {
    json_object_set_null(message, "content");
  }

  if (tool_calls) {
    json_object_add(message, "tool_calls", tool_calls);
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

  json_object_set_string(message, "tool_call_id", tool_call_id);
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

  tool_call = json_create_object();
  function_object = json_create_object();
  if (!tool_call || !function_object) {
    json_free(tool_call); tool_call = NULL;
    json_free(function_object); function_object = NULL;
    return NULL;
  }

  json_object_set_string(tool_call, "id", id);
  json_object_set_string(tool_call, "type", type ? type : "function");
  json_object_set_string(function_object, "name", name);
  json_object_set_string(function_object, "arguments", arguments);
  json_object_add(tool_call, "function", function_object);
  return tool_call;
}

json_value_t *turbo_prompt_message_with_content_create(const char *role, json_value_t *content) {
  json_value_t *message;

  if (!content) {
    json_free(content); content = NULL;
    return NULL;
  }

  message = json_create_object();
  if (!message) {
    json_free(content); content = NULL;
    return NULL;
  }

  json_object_set_string(message, "role", role ? role : "user");
  json_object_add(message, "content", content);
  return message;
}

json_value_t *turbo_prompt_content_text_part_create(const char *text) {
  json_value_t *part;

  if (!text) {
    return NULL;
  }

  part = json_create_object();
  if (!part) {
    return NULL;
  }

  json_object_set_string(part, "type", "text");
  json_object_set_string(part, "text", text);
  return part;
}

json_value_t *turbo_prompt_tool_use_part_create(const char *id, const char *name,
                                                json_value_t *input) {
  json_value_t *part;

  if (!id || !name || !input) {
    json_free(input); input = NULL;
    return NULL;
  }

  part = json_create_object();
  if (!part) {
    json_free(input); input = NULL;
    return NULL;
  }

  json_object_set_string(part, "type", "tool_use");
  json_object_set_string(part, "id", id);
  json_object_set_string(part, "name", name);
  json_object_add(part, "input", input);
  return part;
}

json_value_t *turbo_prompt_tool_result_part_create(const char *tool_use_id,
                                                   const char *content) {
  json_value_t *part;

  if (!tool_use_id || !content) {
    return NULL;
  }

  part = json_create_object();
  if (!part) {
    return NULL;
  }

  json_object_set_string(part, "type", "tool_result");
  json_object_set_string(part, "tool_use_id", tool_use_id);
  json_object_set_string(part, "content", content);
  return part;
}

turbo_prompt_status_t turbo_prompt_messages_append(json_value_t *messages, const char *role,
                                                   const char *content) {
  json_value_t *message;

  if (!messages || json_type(messages) != JSON_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  message = turbo_prompt_message_create(role, content);
  if (!message) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  json_array_add(messages, message);
  return TURBO_PROMPT_OK;
}

turbo_prompt_status_t
turbo_prompt_messages_append_json_value(json_value_t *messages, const char *role,
                                  const char *content) {
  json_value_t *message;

  if (!messages ||
      json_type(messages) != JSON_ARRAY) {
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  message = turbo_prompt_message_create_json_value(role, content);
  if (!message) {
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }
  if (turbo_prompt_message_validate_json_value(message) != TURBO_PROMPT_OK) {
    turbo_runtime_json_destroy(message);
    return TURBO_PROMPT_INVALID_ARGUMENT;
  }

  if (turbo_runtime_json_array_append(messages, message) != TURBO_RUNTIME_JSON_OK) {
    turbo_runtime_json_destroy(message);
    return TURBO_PROMPT_OUT_OF_MEMORY;
  }

  return TURBO_PROMPT_OK;
}
