#include "turbo_tool_schema.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static json_value_t *
turbo_tool_schema_clone_json_value(const json_value_t *value) {
  json_value_t *json_value;
  json_value_t *clone;

  if (!value) {
    return NULL;
  }

  json_value = json_clone(value);
  if (!json_value) {
    return NULL;
  }

  clone = json_clone(json_value);
  json_free(json_value);
  return clone;
}

static json_value_t *turbo_tool_schema_parse_parameters_json(const char *parameters_json,
                                                             int strict) {
  json_value_t *parameters = NULL;

  if (!parameters_json) {
    return NULL;
  }

  parameters = json_parse(parameters_json, strlen(parameters_json));
  if (!parameters) {
    return NULL;
  }

  if (strict && parameters && json_type(parameters) == JSON_OBJECT &&
      !json_object_get(parameters, "additionalProperties")) {
    json_object_set_bool(parameters, "additionalProperties", false);
  }

  return parameters;
}

json_value_t *
turbo_tool_schema_parse_parameters_json_value(const char *parameters_json, int strict) {
  json_value_t *parameters = turbo_tool_schema_parse_parameters_json(parameters_json, strict);
  json_value_t *bound;

  if (!parameters) {
    return NULL;
  }

  bound = json_clone(parameters);
  json_free(parameters);
  return bound;
}

static json_value_t *turbo_tool_schema_parse_parameters(const turbo_tool_definition_t *definition) {
  json_value_t *parameters;

  if (!definition) {
    return NULL;
  }

  if (definition->parameters_schema) {
    parameters = json_clone(definition->parameters_schema);
    if (parameters && definition->strict &&
        json_type(parameters) == JSON_OBJECT &&
        !json_object_get(parameters, "additionalProperties")) {
      json_object_set_bool(parameters, "additionalProperties", false);
    }
    return parameters;
  }
  if (!definition->parameters_json) {
    return NULL;
  }

  return turbo_tool_schema_parse_parameters_json(definition->parameters_json, definition->strict);
}


static void turbo_tool_schema_diagnostic(char *diagnostic, size_t capacity,
                                         const char *message,
                                         const char *detail) {
  if (!diagnostic || capacity == 0) return;
  if (detail && detail[0]) {
    snprintf(diagnostic, capacity, "%s: %s", message ? message : "schema validation failed",
             detail);
  } else {
    snprintf(diagnostic, capacity, "%s", message ? message : "schema validation failed");
  }
}

static int turbo_tool_schema_annotation_keyword(const char *key) {
  static const char *const annotations[] = {
      "$id", "$schema", "title", "description", "default", "examples",
      "deprecated", "readOnly", "writeOnly"};
  size_t i;
  if (!key) return 0;
  for (i = 0; i < sizeof(annotations) / sizeof(annotations[0]); ++i) {
    if (strcmp(key, annotations[i]) == 0) return 1;
  }
  return 0;
}

static int turbo_tool_schema_supported_keyword(const char *key) {
  static const char *const keywords[] = {
      "type", "properties", "required", "additionalProperties", "items",
      "enum", "const", "minimum", "maximum", "exclusiveMinimum",
      "exclusiveMaximum", "minLength", "maxLength", "minItems", "maxItems",
      "minProperties", "maxProperties"};
  size_t i;
  if (!key) return 0;
  if (turbo_tool_schema_annotation_keyword(key)) return 1;
  for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
    if (strcmp(key, keywords[i]) == 0) return 1;
  }
  return 0;
}

static turbo_tool_schema_validation_status_t
turbo_tool_schema_check_keywords(const json_value_t *schema,
                                 char *diagnostic,
                                 size_t diagnostic_capacity) {
  size_t i;
  if (!schema || json_type(schema) != JSON_OBJECT) {
    turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                 "parameter schema must be an object", NULL);
    return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
  }
  for (i = 0; i < json_object_size(schema); ++i) {
    const char *key = json_object_key(schema, i);
    if (!key || !turbo_tool_schema_supported_keyword(key)) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "unsupported parameter-schema keyword",
                                   key ? key : "<null>");
      return TURBO_TOOL_SCHEMA_UNSUPPORTED;
    }
  }
  return TURBO_TOOL_SCHEMA_VALID;
}

static int turbo_tool_schema_json_equal(const json_value_t *lhs,
                                        const json_value_t *rhs) {
  size_t i;
  if (lhs == rhs) return 1;
  if (!lhs || !rhs || json_type(lhs) != json_type(rhs)) return 0;

  switch (json_type(lhs)) {
    case JSON_NULL:
      return 1;
    case JSON_BOOL:
      return json_bool(lhs) == json_bool(rhs);
    case JSON_NUMBER:
      return json_number(lhs) == json_number(rhs);
    case JSON_STRING:
      return strcmp(json_string(lhs), json_string(rhs)) == 0;
    case JSON_ARRAY:
      if (json_array_size(lhs) != json_array_size(rhs)) return 0;
      for (i = 0; i < json_array_size(lhs); ++i) {
        if (!turbo_tool_schema_json_equal(json_array_get(lhs, i),
                                          json_array_get(rhs, i))) {
          return 0;
        }
      }
      return 1;
    case JSON_OBJECT:
      if (json_object_size(lhs) != json_object_size(rhs)) return 0;
      for (i = 0; i < json_object_size(lhs); ++i) {
        const char *key = json_object_key(lhs, i);
        const json_value_t *right_value =
            key ? json_object_get(rhs, key) : NULL;
        if (!key || !right_value ||
            !turbo_tool_schema_json_equal(json_object_value(lhs, i),
                                          right_value)) {
          return 0;
        }
      }
      return 1;
    default:
      return 0;
  }
}

static size_t turbo_tool_schema_utf8_codepoint_count(const char *text) {
  const unsigned char *cursor = (const unsigned char *)text;
  size_t count = 0;
  if (!text) return 0;
  while (*cursor) {
    if ((*cursor & 0xc0u) != 0x80u) ++count;
    ++cursor;
  }
  return count;
}

static int turbo_tool_schema_nonnegative_size(const json_value_t *value,
                                              size_t *out) {
  double number;
  double integral;
  if (!value || json_type(value) != JSON_NUMBER || !out) return 0;
  number = json_number(value);
  if (!isfinite(number) || number < 0.0 || modf(number, &integral) != 0.0 ||
      integral > (double)SIZE_MAX) {
    return 0;
  }
  *out = (size_t)integral;
  return 1;
}

static turbo_tool_schema_validation_status_t
turbo_tool_schema_type_matches(const json_value_t *value,
                               const char *type_name,
                               int *out_matches) {
  double number;
  double integral;
  if (!value || !type_name || !out_matches) {
    return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
  }
  *out_matches = 0;
  if (strcmp(type_name, "object") == 0) {
    *out_matches = json_type(value) == JSON_OBJECT;
  } else if (strcmp(type_name, "array") == 0) {
    *out_matches = json_type(value) == JSON_ARRAY;
  } else if (strcmp(type_name, "string") == 0) {
    *out_matches = json_type(value) == JSON_STRING;
  } else if (strcmp(type_name, "number") == 0) {
    *out_matches = json_type(value) == JSON_NUMBER &&
                   isfinite(json_number(value));
  } else if (strcmp(type_name, "integer") == 0) {
    if (json_type(value) == JSON_NUMBER) {
      number = json_number(value);
      *out_matches = isfinite(number) && modf(number, &integral) == 0.0;
    }
  } else if (strcmp(type_name, "boolean") == 0) {
    *out_matches = json_type(value) == JSON_BOOL;
  } else if (strcmp(type_name, "null") == 0) {
    *out_matches = json_type(value) == JSON_NULL;
  } else {
    return TURBO_TOOL_SCHEMA_UNSUPPORTED;
  }
  return TURBO_TOOL_SCHEMA_VALID;
}

static turbo_tool_schema_validation_status_t
turbo_tool_schema_validate_node(const json_value_t *value,
                                const json_value_t *schema,
                                char *diagnostic,
                                size_t diagnostic_capacity) {
  turbo_tool_schema_validation_status_t status;
  const json_value_t *type_schema;
  const json_value_t *required;
  const json_value_t *properties;
  const json_value_t *additional;
  const json_value_t *items;
  const json_value_t *enum_values;
  const json_value_t *const_value;
  const json_value_t *bound;
  size_t i;

  status = turbo_tool_schema_check_keywords(schema, diagnostic,
                                            diagnostic_capacity);
  if (status != TURBO_TOOL_SCHEMA_VALID) return status;

  type_schema = json_object_get(schema, "type");
  if (type_schema) {
    int matched = 0;
    if (json_type(type_schema) == JSON_STRING) {
      status = turbo_tool_schema_type_matches(value, json_string(type_schema),
                                              &matched);
      if (status != TURBO_TOOL_SCHEMA_VALID) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "unsupported parameter type",
                                     json_string(type_schema));
        return status;
      }
    } else if (json_type(type_schema) == JSON_ARRAY) {
      for (i = 0; i < json_array_size(type_schema); ++i) {
        const json_value_t *entry = json_array_get(type_schema, i);
        int entry_matches = 0;
        if (!entry || json_type(entry) != JSON_STRING) {
          turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                       "type array must contain strings", NULL);
          return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
        }
        status = turbo_tool_schema_type_matches(value, json_string(entry),
                                                &entry_matches);
        if (status != TURBO_TOOL_SCHEMA_VALID) {
          turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                       "unsupported parameter type",
                                       json_string(entry));
          return status;
        }
        if (entry_matches) {
          matched = 1;
          break;
        }
      }
    } else {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "type must be a string or string array", NULL);
      return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
    }
    if (!matched) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "parameter type mismatch", NULL);
      return TURBO_TOOL_SCHEMA_VALUE_INVALID;
    }
  }

  enum_values = json_object_get(schema, "enum");
  if (enum_values) {
    int matched = 0;
    if (json_type(enum_values) != JSON_ARRAY || json_array_size(enum_values) == 0) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "enum must be a non-empty array", NULL);
      return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
    }
    for (i = 0; i < json_array_size(enum_values); ++i) {
      if (turbo_tool_schema_json_equal(value, json_array_get(enum_values, i))) {
        matched = 1;
        break;
      }
    }
    if (!matched) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "parameter is not an admitted enum value", NULL);
      return TURBO_TOOL_SCHEMA_VALUE_INVALID;
    }
  }

  const_value = json_object_get(schema, "const");
  if (const_value && !turbo_tool_schema_json_equal(value, const_value)) {
    turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                 "parameter does not match const", NULL);
    return TURBO_TOOL_SCHEMA_VALUE_INVALID;
  }

  if (json_type(value) == JSON_NUMBER) {
    double number = json_number(value);
    struct {
      const char *key;
      int exclusive;
      int lower;
    } numeric_bounds[] = {
        {"minimum", 0, 1}, {"maximum", 0, 0},
        {"exclusiveMinimum", 1, 1}, {"exclusiveMaximum", 1, 0}};
    for (i = 0; i < sizeof(numeric_bounds) / sizeof(numeric_bounds[0]); ++i) {
      bound = json_object_get(schema, numeric_bounds[i].key);
      if (!bound) continue;
      if (json_type(bound) != JSON_NUMBER || !isfinite(json_number(bound))) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "numeric bound must be a finite number",
                                     numeric_bounds[i].key);
        return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
      }
      if ((numeric_bounds[i].lower &&
           (numeric_bounds[i].exclusive ? number <= json_number(bound)
                                        : number < json_number(bound))) ||
          (!numeric_bounds[i].lower &&
           (numeric_bounds[i].exclusive ? number >= json_number(bound)
                                        : number > json_number(bound)))) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "parameter violates numeric bound",
                                     numeric_bounds[i].key);
        return TURBO_TOOL_SCHEMA_VALUE_INVALID;
      }
    }
  }

  if (json_type(value) == JSON_STRING) {
    size_t length = turbo_tool_schema_utf8_codepoint_count(json_string(value));
    struct {
      const char *key;
      int minimum;
    } string_bounds[] = {{"minLength", 1}, {"maxLength", 0}};
    for (i = 0; i < sizeof(string_bounds) / sizeof(string_bounds[0]); ++i) {
      size_t limit;
      bound = json_object_get(schema, string_bounds[i].key);
      if (!bound) continue;
      if (!turbo_tool_schema_nonnegative_size(bound, &limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "string length bound must be a non-negative integer",
                                     string_bounds[i].key);
        return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
      }
      if ((string_bounds[i].minimum && length < limit) ||
          (!string_bounds[i].minimum && length > limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "parameter violates string length bound",
                                     string_bounds[i].key);
        return TURBO_TOOL_SCHEMA_VALUE_INVALID;
      }
    }
  }

  if (json_type(value) == JSON_ARRAY) {
    size_t count = json_array_size(value);
    struct {
      const char *key;
      int minimum;
    } array_bounds[] = {{"minItems", 1}, {"maxItems", 0}};
    for (i = 0; i < sizeof(array_bounds) / sizeof(array_bounds[0]); ++i) {
      size_t limit;
      bound = json_object_get(schema, array_bounds[i].key);
      if (!bound) continue;
      if (!turbo_tool_schema_nonnegative_size(bound, &limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "array bound must be a non-negative integer",
                                     array_bounds[i].key);
        return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
      }
      if ((array_bounds[i].minimum && count < limit) ||
          (!array_bounds[i].minimum && count > limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "parameter violates array bound",
                                     array_bounds[i].key);
        return TURBO_TOOL_SCHEMA_VALUE_INVALID;
      }
    }

    items = json_object_get(schema, "items");
    if (items) {
      if (json_type(items) != JSON_OBJECT) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "items must be an object schema", NULL);
        return TURBO_TOOL_SCHEMA_UNSUPPORTED;
      }
      for (i = 0; i < count; ++i) {
        status = turbo_tool_schema_validate_node(json_array_get(value, i), items,
                                                 diagnostic, diagnostic_capacity);
        if (status != TURBO_TOOL_SCHEMA_VALID) return status;
      }
    }
  }

  if (json_type(value) == JSON_OBJECT) {
    size_t count = json_object_size(value);
    struct {
      const char *key;
      int minimum;
    } object_bounds[] = {{"minProperties", 1}, {"maxProperties", 0}};
    for (i = 0; i < sizeof(object_bounds) / sizeof(object_bounds[0]); ++i) {
      size_t limit;
      bound = json_object_get(schema, object_bounds[i].key);
      if (!bound) continue;
      if (!turbo_tool_schema_nonnegative_size(bound, &limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "object bound must be a non-negative integer",
                                     object_bounds[i].key);
        return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
      }
      if ((object_bounds[i].minimum && count < limit) ||
          (!object_bounds[i].minimum && count > limit)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "parameter violates object bound",
                                     object_bounds[i].key);
        return TURBO_TOOL_SCHEMA_VALUE_INVALID;
      }
    }

    required = json_object_get(schema, "required");
    if (required) {
      if (json_type(required) != JSON_ARRAY) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "required must be an array", NULL);
        return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
      }
      for (i = 0; i < json_array_size(required); ++i) {
        const json_value_t *entry = json_array_get(required, i);
        const char *name;
        if (!entry || json_type(entry) != JSON_STRING) {
          turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                       "required entries must be strings", NULL);
          return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
        }
        name = json_string(entry);
        if (!json_object_get(value, name)) {
          turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                       "missing required property", name);
          return TURBO_TOOL_SCHEMA_VALUE_INVALID;
        }
      }
    }

    properties = json_object_get(schema, "properties");
    if (properties && json_type(properties) != JSON_OBJECT) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "properties must be an object", NULL);
      return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
    }

    if (properties) {
      for (i = 0; i < json_object_size(properties); ++i) {
        const char *key = json_object_key(properties, i);
        const json_value_t *property_schema = json_object_value(properties, i);
        const json_value_t *property_value =
            key ? json_object_get(value, key) : NULL;
        if (!key || !property_schema ||
            json_type(property_schema) != JSON_OBJECT) {
          turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                       "property schema must be an object",
                                       key ? key : "<null>");
          return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
        }
        if (!property_value) continue;
        status = turbo_tool_schema_validate_node(property_value, property_schema,
                                                 diagnostic, diagnostic_capacity);
        if (status != TURBO_TOOL_SCHEMA_VALID) return status;
      }
    }

    additional = json_object_get(schema, "additionalProperties");
    if (additional &&
        json_type(additional) != JSON_BOOL &&
        json_type(additional) != JSON_OBJECT) {
      turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                   "additionalProperties must be boolean or schema", NULL);
      return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
    }

    for (i = 0; i < count; ++i) {
      const char *key = json_object_key(value, i);
      const json_value_t *property_schema =
          key && properties ? json_object_get(properties, key) : NULL;
      if (!key || property_schema) continue;
      if (!additional || (json_type(additional) == JSON_BOOL && json_bool(additional))) {
        continue;
      }
      if (json_type(additional) == JSON_BOOL && !json_bool(additional)) {
        turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                     "unexpected property", key);
        return TURBO_TOOL_SCHEMA_VALUE_INVALID;
      }
      status = turbo_tool_schema_validate_node(json_object_value(value, i),
                                               additional, diagnostic,
                                               diagnostic_capacity);
      if (status != TURBO_TOOL_SCHEMA_VALID) return status;
    }
  }

  return TURBO_TOOL_SCHEMA_VALID;
}

turbo_tool_schema_validation_status_t
turbo_tool_schema_validate_arguments_json_value(
    const turbo_tool_definition_t *definition,
    const json_value_t *arguments,
    char *diagnostic,
    size_t diagnostic_capacity) {
  json_value_t *schema;
  turbo_tool_schema_validation_status_t status;

  if (diagnostic && diagnostic_capacity) diagnostic[0] = '\0';
  if (!definition || !arguments ||
      (!definition->parameters_json && !definition->parameters_schema)) {
    turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                 "invalid tool-schema validation arguments", NULL);
    return TURBO_TOOL_SCHEMA_INVALID_ARGUMENT;
  }

  schema = turbo_tool_schema_parse_parameters(definition);
  if (!schema) {
    turbo_tool_schema_diagnostic(diagnostic, diagnostic_capacity,
                                 "could not materialize parameter schema", NULL);
    return TURBO_TOOL_SCHEMA_INVALID_SCHEMA;
  }

  status = turbo_tool_schema_validate_node(arguments, schema, diagnostic,
                                           diagnostic_capacity);
  json_free(schema);
  return status;
}

static int turbo_tool_schema_openai_name_char_valid(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static char *turbo_tool_schema_openai_compatible_name(const char *name) {
  char *copy;
  size_t i;
  size_t len;

  if (!name || name[0] == '\0') {
    return NULL;
  }

  len = strlen(name);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }

  for (i = 0; i < len; ++i) {
    copy[i] = turbo_tool_schema_openai_name_char_valid(name[i]) ? name[i] : '_';
  }
  copy[len] = '\0';
  return copy;
}

static int turbo_tool_schema_openai_compatible_name_seen(
    const turbo_tool_registry_t *registry, size_t end_index, const char *name) {
  size_t i;

  if (!registry || !name) {
    return 0;
  }

  for (i = 0; i < end_index; ++i) {
    turbo_tool_definition_t previous = {0};
    char *previous_name;
    int matches;

    if (turbo_tool_registry_get_definition(registry, i, &previous) != TURBO_TOOL_OK) {
      return 1;
    }

    previous_name = turbo_tool_schema_openai_compatible_name(previous.name);
    if (!previous_name) {
      return 1;
    }

    matches = strcmp(previous_name, name) == 0;
    free(previous_name);
    if (matches) {
      return 1;
    }
  }

  return 0;
}

json_value_t *turbo_tool_schema_build_openai_chat_tool_definition(
    const char *name, const char *description, const char *parameters_json, int strict,
    int compatible_mode) {
  json_value_t *tool;
  json_value_t *function_object;
  json_value_t *parameters;

  if (!name || !description || !parameters_json) {
    return NULL;
  }

  parameters = turbo_tool_schema_parse_parameters_json(parameters_json, strict);
  if (!parameters) {
    return NULL;
  }

  tool = json_create_object();
  function_object = json_create_object();
  if (!tool || !function_object) {
    json_free(function_object);
    json_free(tool);
    json_free(parameters);
    return NULL;
  }

  json_object_set_string(tool, "type", "function");
  json_object_set_string(function_object, "name", name);
  json_object_set_string(function_object, "description", description);
  if (!compatible_mode) {
    json_object_set_bool(function_object, "strict", strict ? true : false);
  }
  json_object_add(function_object, "parameters", parameters);
  json_object_add(tool, "function", function_object);
  return tool;
}

static json_value_t *turbo_tool_schema_build_openai_chat_tool_with_parameters(
    const char *name, const char *description, json_value_t *parameters, int strict,
    int compatible_mode) {
  json_value_t *tool;
  json_value_t *function_object;

  if (!name || !description || !parameters) {
    json_free(parameters);
    return NULL;
  }

  tool = json_create_object();
  function_object = json_create_object();
  if (!tool || !function_object) {
    json_free(function_object);
    json_free(tool);
    json_free(parameters);
    return NULL;
  }

  json_object_set_string(tool, "type", "function");
  json_object_set_string(function_object, "name", name);
  json_object_set_string(function_object, "description", description);
  if (!compatible_mode) {
    json_object_set_bool(function_object, "strict", strict ? true : false);
  }
  json_object_add(function_object, "parameters", parameters);
  json_object_add(tool, "function", function_object);
  return tool;
}

json_value_t *turbo_tool_schema_build_openai_tools(const turbo_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;
  size_t count;

  count = turbo_tool_registry_count(registry);
  tools = json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition = {0};
    json_value_t *tool;
    json_value_t *parameters;

    if (turbo_tool_registry_get_definition(registry, i, &definition) != TURBO_TOOL_OK) {
      json_free(tools);
      return NULL;
    }

    parameters = turbo_tool_schema_parse_parameters(&definition);
    if (!parameters) {
      json_free(tools);
      return NULL;
    }

    tool = json_create_object();
    if (!tool) {
      json_free(parameters);
      json_free(tools);
      return NULL;
    }

    json_object_set_string(tool, "type", "function");
    json_object_set_string(tool, "name", definition.name);
    json_object_set_string(tool, "description", definition.description);
    json_object_set_bool(tool, "strict", definition.strict ? true : false);
    json_object_add(tool, "parameters", parameters);
    json_array_add(tools, tool);
  }

  return tools;
}

json_value_t *
turbo_tool_schema_build_registry_json_value(const turbo_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;
  size_t count = turbo_tool_registry_count(registry);

  tools = json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition = {0};
    json_value_t *tool = NULL;
    json_value_t *name = NULL;
    json_value_t *description = NULL;
    json_value_t *strict = NULL;
    json_value_t *parameters = NULL;

    if (turbo_tool_registry_get_definition(registry, i, &definition) != TURBO_TOOL_OK) {
      turbo_runtime_json_destroy(tools);
      return NULL;
    }

    tool = json_create_object();
    name = json_create_string(definition.name);
    description = json_create_string(definition.description);
    strict = json_create_bool(definition.strict ? 1 : 0);
    if (definition.parameters_schema) {
      parameters = turbo_tool_schema_clone_json_value(definition.parameters_schema);
    } else {
      parameters =
          turbo_tool_schema_parse_parameters_json_value(definition.parameters_json, definition.strict);
    }
    if (!tool || !name || !description || !strict || !parameters) {
      turbo_runtime_json_destroy(name);
      turbo_runtime_json_destroy(description);
      turbo_runtime_json_destroy(strict);
      turbo_runtime_json_destroy(parameters);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }

    if (turbo_runtime_json_object_set(tool, "name", name) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(name);
      turbo_runtime_json_destroy(description);
      turbo_runtime_json_destroy(strict);
      turbo_runtime_json_destroy(parameters);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }
    name = NULL;

    if (turbo_runtime_json_object_set(tool, "description", description) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(description);
      turbo_runtime_json_destroy(strict);
      turbo_runtime_json_destroy(parameters);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }
    description = NULL;

    if (turbo_runtime_json_object_set(tool, "strict", strict) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(strict);
      turbo_runtime_json_destroy(parameters);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }
    strict = NULL;

    if (turbo_runtime_json_object_set(tool, "parameters", parameters) !=
        TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(parameters);
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }
    parameters = NULL;

    if (turbo_runtime_json_array_append(tools, tool) != TURBO_RUNTIME_JSON_OK) {
      turbo_runtime_json_destroy(tool);
      turbo_runtime_json_destroy(tools);
      return NULL;
    }
    tool = NULL;
  }

  return tools;
}

json_value_t *turbo_tool_schema_build_openai_compatible_chat_tools(
    const turbo_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;
  size_t count;

  count = turbo_tool_registry_count(registry);
  tools = json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition = {0};
    json_value_t *tool;
    char *compatible_name;
    json_value_t *parameters;

    if (turbo_tool_registry_get_definition(registry, i, &definition) != TURBO_TOOL_OK) {
      json_free(tools);
      return NULL;
    }

    compatible_name = turbo_tool_schema_openai_compatible_name(definition.name);
    if (!compatible_name) {
      json_free(tools);
      return NULL;
    }
    if (turbo_tool_schema_openai_compatible_name_seen(registry, i, compatible_name)) {
      free(compatible_name);
      json_free(tools);
      return NULL;
    }

    parameters = turbo_tool_schema_parse_parameters(&definition);
    if (!parameters) {
      free(compatible_name);
      json_free(tools);
      return NULL;
    }

    tool = turbo_tool_schema_build_openai_chat_tool_with_parameters(
        compatible_name, definition.description, parameters, definition.strict, 1);
    free(compatible_name);
    if (!tool) {
      json_free(tools);
      return NULL;
    }
    json_array_add(tools, tool);
  }

  return tools;
}

json_value_t *turbo_tool_schema_build_openai_chat_tools(const turbo_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;
  size_t count = turbo_tool_registry_count(registry);

  tools = json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition = {0};
    json_value_t *tool;
    json_value_t *parameters;

    if (turbo_tool_registry_get_definition(registry, i, &definition) != TURBO_TOOL_OK) {
      json_free(tools);
      return NULL;
    }

    parameters = turbo_tool_schema_parse_parameters(&definition);
    if (!parameters) {
      json_free(tools);
      return NULL;
    }

    tool = turbo_tool_schema_build_openai_chat_tool_with_parameters(
        definition.name, definition.description, parameters, definition.strict, 0);
    if (!tool) {
      json_free(tools);
      return NULL;
    }
    json_array_add(tools, tool);
  }

  return tools;
}

json_value_t *turbo_tool_schema_build_anthropic_tools(const turbo_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;
  size_t count;

  count = turbo_tool_registry_count(registry);
  tools = json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < count; ++i) {
    turbo_tool_definition_t definition = {0};
    json_value_t *tool;
    json_value_t *input_schema;

    if (turbo_tool_registry_get_definition(registry, i, &definition) != TURBO_TOOL_OK) {
      json_free(tools);
      return NULL;
    }

    input_schema = turbo_tool_schema_parse_parameters(&definition);
    if (!input_schema) {
      json_free(tools);
      return NULL;
    }

    tool = json_create_object();
    if (!tool) {
      json_free(input_schema);
      json_free(tools);
      return NULL;
    }

    json_object_set_string(tool, "name", definition.name);
    json_object_set_string(tool, "description", definition.description);
    json_object_add(tool, "input_schema", input_schema);
    json_array_add(tools, tool);
  }

  return tools;
}
