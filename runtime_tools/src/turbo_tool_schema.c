#include "turbo_tool_schema.h"

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
  if (!definition) {
    return NULL;
  }

  if (definition->parameters_schema) {
    return json_clone(definition->parameters_schema);
  }
  if (!definition->parameters_json) {
    return NULL;
  }

  return turbo_tool_schema_parse_parameters_json(definition->parameters_json, definition->strict);
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
