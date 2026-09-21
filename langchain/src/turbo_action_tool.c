#include "turbo_action_tool.h"
#include <json_parser.h>
#include "turbo_tool_schema.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  turbo_action_tool_definition_t definition;
  char *name;
  char *description;
  char *parameters_json;
} turbo_action_tool_entry_t;

struct turbo_action_tool_registry_s {
  turbo_action_tool_entry_t *entries;
  size_t count;
  size_t capacity;
};

typedef struct {
  char *tool_name;
  turbo_action_tool_handler_fn handler;
  void *handler_user_data;
} turbo_action_tool_bridge_entry_t;

static char *turbo_action_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }

  memcpy(copy, src, len);
  return copy;
}

static void turbo_action_tool_free_entry(turbo_action_tool_entry_t *entry) {
  if (!entry) {
    return;
  }

  free(entry->name);
  free(entry->description);
  free(entry->parameters_json);
  memset(entry, 0, sizeof(*entry));
}

static void turbo_action_tool_bridge_entry_free(void *user_data) {
  turbo_action_tool_bridge_entry_t *entry = (turbo_action_tool_bridge_entry_t *)user_data;
  if (!entry) {
    return;
  }

  free(entry->tool_name);
  free(entry);
}

static turbo_action_tool_status_t
turbo_action_tool_registry_reserve(turbo_action_tool_registry_t *registry) {
  turbo_action_tool_entry_t *entries;
  size_t new_capacity;

  if (registry->count < registry->capacity) {
    return TURBO_ACTION_TOOL_OK;
  }

  new_capacity = registry->capacity == 0 ? 4 : registry->capacity * 2;
  entries = (turbo_action_tool_entry_t *)realloc(registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_ACTION_TOOL_OUT_OF_MEMORY;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return TURBO_ACTION_TOOL_OK;
}

static int turbo_action_tool_bridge_handler(const char *arguments_json, char **out_output,
                                            void *user_data) {
  turbo_action_tool_bridge_entry_t *bridge = (turbo_action_tool_bridge_entry_t *)user_data;
  json_value_t *args = NULL;
  json_value_t *result = NULL;
  size_t output_len = 0;
  char *output = NULL;
  turbo_action_tool_status_t status;

  if (!bridge || !bridge->handler || !bridge->tool_name || !out_output) {
    return -1;
  }

  *out_output = NULL;
  if (!arguments_json || arguments_json[0] == '\0') {
    args = turbo_json_create_object();
  } else if (turbo_parse_json((const uint8_t *)arguments_json, strlen(arguments_json), &args) != 0) {
    return -1;
  }

  if (!args) {
    return -1;
  }

  status = bridge->handler(args, &result, bridge->handler_user_data) == 0
               ? TURBO_ACTION_TOOL_OK
               : TURBO_ACTION_TOOL_ERROR;
  turbo_free_json(&args);
  if (status != TURBO_ACTION_TOOL_OK || !result ||
      turbo_action_result_validate(result) != 0) {
    turbo_free_json(&result);
    return -1;
  }

  output = turbo_json_serialize(result, &output_len);
  turbo_free_json(&result);
  if (!output) {
    return -1;
  }

  *out_output = output;
  return 0;
}

static int turbo_action_tool_bridge_json_value_handler(
    const json_value_t *arguments,
    json_value_t **out_result, void *user_data) {
  turbo_action_tool_bridge_entry_t *bridge = (turbo_action_tool_bridge_entry_t *)user_data;
  json_value_t *args = NULL;
  json_value_t *result = NULL;
  turbo_action_tool_status_t status;

  if (!bridge || !bridge->handler || !bridge->tool_name || !out_result) {
    return -1;
  }

  *out_result = NULL;
  if (arguments) {
    args = turbo_json_clone(arguments);
    if (!args) {
      return -1;
    }
  } else {
    args = turbo_json_create_object();
    if (!args) {
      return -1;
    }
  }

  status = bridge->handler(args, &result, bridge->handler_user_data) == 0
               ? TURBO_ACTION_TOOL_OK
               : TURBO_ACTION_TOOL_ERROR;
  turbo_free_json(&args);
  if (status != TURBO_ACTION_TOOL_OK || !result ||
      turbo_action_result_validate(result) != 0) {
    turbo_free_json(&result);
    return -1;
  }

  *out_result = turbo_json_clone(result);
  turbo_free_json(&result);
  return *out_result ? 0 : -1;
}

static const turbo_action_tool_entry_t *
turbo_action_tool_registry_find_entry(const turbo_action_tool_registry_t *registry,
                                      const char *name) {
  size_t i;

  if (!registry || !name) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (strcmp(registry->entries[i].definition.name, name) == 0) {
      return &registry->entries[i];
    }
  }

  return NULL;
}

turbo_action_tool_registry_t *turbo_action_tool_registry_create(void) {
  return (turbo_action_tool_registry_t *)calloc(1, sizeof(turbo_action_tool_registry_t));
}

void turbo_action_tool_registry_destroy(turbo_action_tool_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->count; ++i) {
    turbo_action_tool_free_entry(&registry->entries[i]);
  }

  free(registry->entries);
  free(registry);
}

turbo_action_tool_status_t
turbo_action_tool_registry_add(turbo_action_tool_registry_t *registry,
                               const turbo_action_tool_definition_t *definition) {
  turbo_action_tool_status_t status;
  turbo_action_tool_entry_t *entry;

  if (!registry || !definition || !definition->name || !definition->description ||
      !definition->parameters_json || !definition->handler) {
    return TURBO_ACTION_TOOL_INVALID_ARGUMENT;
  }

  if (turbo_action_tool_registry_find_entry(registry, definition->name)) {
    return TURBO_ACTION_TOOL_DUPLICATE;
  }

  status = turbo_action_tool_registry_reserve(registry);
  if (status != TURBO_ACTION_TOOL_OK) {
    return status;
  }

  entry = &registry->entries[registry->count];
  memset(entry, 0, sizeof(*entry));
  entry->name = turbo_action_strdup(definition->name);
  entry->description = turbo_action_strdup(definition->description);
  entry->parameters_json = turbo_action_strdup(definition->parameters_json);
  if (!entry->name || !entry->description || !entry->parameters_json) {
    turbo_action_tool_free_entry(entry);
    return TURBO_ACTION_TOOL_OUT_OF_MEMORY;
  }

  entry->definition = *definition;
  entry->definition.name = entry->name;
  entry->definition.description = entry->description;
  entry->definition.parameters_json = entry->parameters_json;
  registry->count++;
  return TURBO_ACTION_TOOL_OK;
}

size_t turbo_action_tool_registry_count(const turbo_action_tool_registry_t *registry) {
  return registry ? registry->count : 0;
}

const turbo_action_tool_definition_t *
turbo_action_tool_registry_find(const turbo_action_tool_registry_t *registry, const char *name) {
  const turbo_action_tool_entry_t *entry = turbo_action_tool_registry_find_entry(registry, name);
  return entry ? &entry->definition : NULL;
}

turbo_action_tool_status_t
turbo_action_tool_registry_execute(const turbo_action_tool_registry_t *registry, const char *name,
                                   const json_value_t *args, json_value_t **out_result) {
  const turbo_action_tool_entry_t *entry;
  int rc;

  if (!registry || !name || !out_result) {
    return TURBO_ACTION_TOOL_INVALID_ARGUMENT;
  }

  *out_result = NULL;
  entry = turbo_action_tool_registry_find_entry(registry, name);
  if (!entry) {
    return TURBO_ACTION_TOOL_NOT_FOUND;
  }

  rc = entry->definition.handler(args, out_result, entry->definition.user_data);
  if (rc != 0) {
    turbo_free_json(out_result);
    return TURBO_ACTION_TOOL_ERROR;
  }

  if (!*out_result || turbo_action_result_validate(*out_result) != 0) {
    turbo_free_json(out_result);
    return TURBO_ACTION_TOOL_ERROR;
  }

  return TURBO_ACTION_TOOL_OK;
}

json_value_t *
turbo_action_tool_registry_build_openai_chat_tools(const turbo_action_tool_registry_t *registry) {
  json_value_t *tools;
  size_t i;

  if (!registry) {
    return turbo_json_create_array();
  }

  tools = turbo_json_create_array();
  if (!tools) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    json_value_t *tool = turbo_tool_schema_build_openai_chat_tool_definition(
        registry->entries[i].definition.name, registry->entries[i].definition.description,
        registry->entries[i].parameters_json, 1, 0);

    if (!tool) {
      turbo_free_json(&tools);
      return NULL;
    }
    turbo_json_array_add(tools, tool);
  }

  return tools;
}

turbo_tool_registry_t *
turbo_action_tool_registry_build_tool_registry_bridge(
    const turbo_action_tool_registry_t *registry) {
  turbo_tool_registry_t *tool_registry;
  size_t i;

  if (!registry) {
    return NULL;
  }

  tool_registry = turbo_tool_registry_create();
  if (!tool_registry) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    turbo_action_tool_bridge_entry_t *bridge =
        (turbo_action_tool_bridge_entry_t *)calloc(1, sizeof(*bridge));
    turbo_tool_definition_t definition;

    if (!bridge) {
      turbo_tool_registry_destroy(tool_registry);
      return NULL;
    }

    bridge->tool_name = turbo_action_strdup(registry->entries[i].definition.name);
    bridge->handler = registry->entries[i].definition.handler;
    bridge->handler_user_data = registry->entries[i].definition.user_data;
    if (!bridge->tool_name) {
      turbo_action_tool_bridge_entry_free(bridge);
      turbo_tool_registry_destroy(tool_registry);
      return NULL;
    }

    memset(&definition, 0, sizeof(definition));
    definition.name = registry->entries[i].definition.name;
    definition.description = registry->entries[i].definition.description;
    definition.parameters_json = registry->entries[i].definition.parameters_json;
    definition.strict = 1;
    definition.handler = turbo_action_tool_bridge_handler;
    definition.json_value_handler = turbo_action_tool_bridge_json_value_handler;
    definition.user_data = bridge;
    definition.user_data_free = turbo_action_tool_bridge_entry_free;
    if (turbo_tool_registry_add(tool_registry, &definition) != TURBO_TOOL_OK) {
      turbo_action_tool_bridge_entry_free(bridge);
      turbo_tool_registry_destroy(tool_registry);
      return NULL;
    }
  }

  return tool_registry;
}

json_value_t *turbo_action_result_create(int ok, const char *summary) {
  json_value_t *result = turbo_json_create_object();
  json_value_t *changed_files = turbo_json_create_array();
  json_value_t *artifacts = turbo_json_create_array();

  if (!result || !changed_files || !artifacts) {
    turbo_free_json(&result);
    turbo_free_json(&changed_files);
    turbo_free_json(&artifacts);
    return NULL;
  }

  turbo_json_object_set_bool(result, "ok", ok ? true : false);
  turbo_json_object_set_string(result, "summary", summary ? summary : "");
  turbo_json_object_set_string(result, "stdout", "");
  turbo_json_object_set_string(result, "stderr", "");
  turbo_json_object_set_number(result, "exit_code", 0);
  turbo_json_object_add(result, "changed_files", changed_files);
  turbo_json_object_add(result, "artifacts", artifacts);
  turbo_json_object_set_bool(result, "retryable", false);
  return result;
}

int turbo_action_result_set_command_fields(json_value_t *result, int exit_code,
                                           const char *stdout_text, const char *stderr_text) {
  if (!result || turbo_json_type(result) != TURBO_JSON_OBJECT) {
    return -1;
  }

  turbo_json_object_set_number(result, "exit_code", exit_code);
  turbo_json_object_set_string(result, "stdout", stdout_text ? stdout_text : "");
  turbo_json_object_set_string(result, "stderr", stderr_text ? stderr_text : "");
  return 0;
}

int turbo_action_result_add_changed_file(json_value_t *result, const char *path) {
  json_value_t *changed_files;

  if (!result || turbo_json_type(result) != TURBO_JSON_OBJECT || !path) {
    return -1;
  }

  changed_files = turbo_json_object_get(result, "changed_files");
  if (!changed_files || turbo_json_type(changed_files) != TURBO_JSON_ARRAY) {
    return -1;
  }

  turbo_json_array_add(changed_files, turbo_json_create_string(path));
  return 0;
}

int turbo_action_result_set_retryable(json_value_t *result, int retryable) {
  if (!result || turbo_json_type(result) != TURBO_JSON_OBJECT) {
    return -1;
  }

  turbo_json_object_set_bool(result, "retryable", retryable ? true : false);
  return 0;
}

int turbo_action_result_validate(const json_value_t *result) {
  const json_value_t *changed_files;
  const json_value_t *artifacts;

  if (!result || turbo_json_type(result) != TURBO_JSON_OBJECT) {
    return -1;
  }

  if (!turbo_json_object_get(result, "ok") || !turbo_json_object_get(result, "summary") ||
      !turbo_json_object_get(result, "stdout") || !turbo_json_object_get(result, "stderr") ||
      !turbo_json_object_get(result, "exit_code") || !turbo_json_object_get(result, "retryable")) {
    return -1;
  }

  changed_files = turbo_json_object_get(result, "changed_files");
  artifacts = turbo_json_object_get(result, "artifacts");
  if (!changed_files || turbo_json_type(changed_files) != TURBO_JSON_ARRAY || !artifacts ||
      turbo_json_type(artifacts) != TURBO_JSON_ARRAY) {
    return -1;
  }

  return 0;
}
