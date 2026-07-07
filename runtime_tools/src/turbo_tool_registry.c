#include "turbo_tool_registry.h"
#include "turbo_tool_schema.h"

#include <stdlib.h>
#include <string.h>

static char *turbo_tool_strdup(const char *src) {
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

static int turbo_tool_openai_name_char_valid(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static int turbo_tool_openai_compatible_name_equals(const char *original,
                                                    const char *compatible) {
  size_t i;

  if (!original || !compatible) {
    return 0;
  }

  for (i = 0; original[i] != '\0' && compatible[i] != '\0'; ++i) {
    char ch = turbo_tool_openai_name_char_valid(original[i]) ? original[i] : '_';
    if (ch != compatible[i]) {
      return 0;
    }
  }

  return original[i] == '\0' && compatible[i] == '\0';
}

typedef struct {
  char *name;
  char *description;
  char *parameters_json;
  int parameters_json_serialized;
  turbo_runtime_data_bind_value_t *parameters_schema;
  int strict;
  turbo_tool_handler_fn handler;
  turbo_tool_bind_handler_fn bind_handler;
  void *user_data;
  turbo_tool_user_data_free_fn user_data_free;
} turbo_tool_entry_t;

struct turbo_tool_registry_s {
  turbo_tool_entry_t *entries;
  size_t count;
  size_t capacity;
};



static turbo_runtime_data_bind_value_t *
turbo_tool_registry_clone_bind_value(const turbo_runtime_data_bind_value_t *value) {
  json_value_t *json_value;
  turbo_runtime_data_bind_value_t *clone;

  if (!value) {
    return NULL;
  }

  json_value = turbo_runtime_data_bind_value_to_json(value);
  if (!json_value) {
    return NULL;
  }

  clone = turbo_runtime_data_bind_value_from_json(json_value);
  turbo_free_json(&json_value);
  return clone;
}

static char *
turbo_tool_registry_serialize_bind_value(const turbo_runtime_data_bind_value_t *value) {
  json_value_t *json_value;
  char *serialized;

  if (!value) {
    return NULL;
  }

  json_value = turbo_runtime_data_bind_value_to_json(value);
  if (!json_value) {
    return NULL;
  }

  serialized = turbo_json_serialize(json_value, NULL);
  turbo_free_json(&json_value);
  return serialized;
}

static const turbo_tool_entry_t *
turbo_tool_registry_find(const turbo_tool_registry_t *registry, const char *name) {
  size_t i;
  const turbo_tool_entry_t *compatible_match = NULL;
  size_t compatible_matches = 0;

  if (!registry || !name) {
    return NULL;
  }

  for (i = 0; i < registry->count; ++i) {
    if (strcmp(registry->entries[i].name, name) == 0) {
      return &registry->entries[i];
    }
  }

  for (i = 0; i < registry->count; ++i) {
    if (turbo_tool_openai_compatible_name_equals(registry->entries[i].name, name)) {
      compatible_match = &registry->entries[i];
      compatible_matches++;
    }
  }

  return compatible_matches == 1 ? compatible_match : NULL;
}

static turbo_tool_status_t turbo_tool_registry_reserve(turbo_tool_registry_t *registry) {
  turbo_tool_entry_t *entries;
  size_t new_capacity;

  if (registry->count < registry->capacity) {
    return TURBO_TOOL_OK;
  }

  new_capacity = registry->capacity == 0 ? 4 : registry->capacity * 2;
  entries =
      (turbo_tool_entry_t *)realloc(registry->entries, new_capacity * sizeof(*entries));
  if (!entries) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  registry->entries = entries;
  registry->capacity = new_capacity;
  return TURBO_TOOL_OK;
}

static void turbo_tool_registry_free_entry(turbo_tool_entry_t *entry) {
  if (!entry) {
    return;
  }

  free(entry->name);
  free(entry->description);
  if (entry->parameters_json_serialized) {
    turbo_json_serialize_free(entry->parameters_json);
  } else {
    free(entry->parameters_json);
  }
  turbo_runtime_data_bind_value_destroy(entry->parameters_schema);
  if (entry->user_data_free) {
    entry->user_data_free(entry->user_data);
  }
  memset(entry, 0, sizeof(*entry));
}

turbo_tool_registry_t *turbo_tool_registry_create(void) {
  return (turbo_tool_registry_t *)calloc(1, sizeof(turbo_tool_registry_t));
}

void turbo_tool_registry_destroy(turbo_tool_registry_t *registry) {
  size_t i;

  if (!registry) {
    return;
  }

  for (i = 0; i < registry->count; ++i) {
    turbo_tool_registry_free_entry(&registry->entries[i]);
  }

  free(registry->entries);
  free(registry);
}

turbo_tool_status_t
turbo_tool_registry_add(turbo_tool_registry_t *registry,
                        const turbo_tool_definition_t *definition) {
  turbo_tool_status_t status;
  turbo_tool_entry_t *entry;

  if (!registry || !definition || !definition->name || !definition->description ||
      (!definition->parameters_json && !definition->parameters_schema) ||
      (!definition->handler && !definition->bind_handler)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  if (turbo_tool_registry_find(registry, definition->name)) {
    return TURBO_TOOL_DUPLICATE;
  }

  status = turbo_tool_registry_reserve(registry);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

  entry = &registry->entries[registry->count];
  memset(entry, 0, sizeof(*entry));

  entry->name = turbo_tool_strdup(definition->name);
  entry->description = turbo_tool_strdup(definition->description);
  entry->parameters_json = definition->parameters_json
                               ? turbo_tool_strdup(definition->parameters_json)
                               : turbo_tool_registry_serialize_bind_value(
                                     definition->parameters_schema);
  entry->parameters_json_serialized = definition->parameters_json ? 0 : 1;
  entry->parameters_schema = definition->parameters_schema
                                 ? turbo_tool_registry_clone_bind_value(
                                       definition->parameters_schema)
                                 : NULL;
  if (!entry->name || !entry->description || !entry->parameters_json ||
      (definition->parameters_schema && !entry->parameters_schema)) {
    turbo_tool_registry_free_entry(entry);
    memset(entry, 0, sizeof(*entry));
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  entry->strict = definition->strict;
  entry->handler = definition->handler;
  entry->bind_handler = definition->bind_handler;
  entry->user_data = definition->user_data;
  entry->user_data_free = definition->user_data_free;
  registry->count++;
  return TURBO_TOOL_OK;
}

size_t turbo_tool_registry_count(const turbo_tool_registry_t *registry) {
  return registry ? registry->count : 0;
}

turbo_tool_status_t
turbo_tool_registry_get_definition(const turbo_tool_registry_t *registry, size_t index,
                                   turbo_tool_definition_t *out_definition) {
  const turbo_tool_entry_t *entry;

  if (!registry || !out_definition) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  if (index >= registry->count) {
    return TURBO_TOOL_NOT_FOUND;
  }

  entry = &registry->entries[index];
  memset(out_definition, 0, sizeof(*out_definition));
  out_definition->name = entry->name;
  out_definition->description = entry->description;
  out_definition->parameters_json = entry->parameters_json;
  out_definition->parameters_schema = entry->parameters_schema;
  out_definition->strict = entry->strict;
  out_definition->handler = entry->handler;
  out_definition->bind_handler = entry->bind_handler;
  out_definition->user_data = entry->user_data;
  out_definition->user_data_free = entry->user_data_free;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t
turbo_tool_registry_execute(const turbo_tool_registry_t *registry, const char *name,
                            const char *arguments_json, char **out_output) {
  const turbo_tool_entry_t *entry;
  int rc;

  if (!registry || !name || !out_output) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_output = NULL;
  entry = turbo_tool_registry_find(registry, name);
  if (!entry) {
    return TURBO_TOOL_NOT_FOUND;
  }

  rc = entry->handler(arguments_json ? arguments_json : "{}", out_output, entry->user_data);
  if (rc != 0) {
    if (*out_output) {
      free(*out_output);
      *out_output = NULL;
    }
    return TURBO_TOOL_ERROR;
  }

  return TURBO_TOOL_OK;
}

turbo_tool_status_t
turbo_tool_registry_execute_bind(const turbo_tool_registry_t *registry, const char *name,
                                 const turbo_runtime_data_bind_value_t *arguments,
                                 turbo_runtime_data_bind_value_t **out_result) {
  const turbo_tool_entry_t *entry;
  int rc;

  if (!registry || !name || !out_result) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  *out_result = NULL;
  entry = turbo_tool_registry_find(registry, name);
  if (!entry) {
    return TURBO_TOOL_NOT_FOUND;
  }
  if (!entry->bind_handler) {
    return TURBO_TOOL_ERROR;
  }

  rc = entry->bind_handler(arguments, out_result, entry->user_data);
  if (rc != 0) {
    if (*out_result) {
      turbo_runtime_data_bind_value_destroy(*out_result);
      *out_result = NULL;
    }
    return TURBO_TOOL_ERROR;
  }

  return TURBO_TOOL_OK;
}

json_value_t *
turbo_tool_registry_build_openai_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_tools(registry);
}

json_value_t *
turbo_tool_registry_build_openai_compatible_chat_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_compatible_chat_tools(registry);
}

json_value_t *
turbo_tool_registry_build_openai_chat_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_chat_tools(registry);
}

json_value_t *
turbo_tool_registry_build_anthropic_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_anthropic_tools(registry);
}
