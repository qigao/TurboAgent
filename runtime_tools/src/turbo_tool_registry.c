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
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '_' || ch == '-';
}

static int turbo_tool_openai_compatible_name_equals(const char *original, const char *compatible) {
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
  json_value_t *parameters_schema;
  int strict;
  turbo_tool_handler_fn handler;
  turbo_tool_json_value_handler_fn json_value_handler;
  void *user_data;
  turbo_tool_user_data_free_fn user_data_free;
  turbo_tool_execution_policy_t execution_policy;
  char **required_capabilities;
  size_t required_capability_count;
} turbo_tool_entry_t;

struct turbo_tool_registry_s {
  turbo_tool_entry_t *entries;
  size_t count;
  size_t capacity;
};

static json_value_t *turbo_tool_registry_clone_json_value(const json_value_t *value) {
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

static char *turbo_tool_registry_serialize_json_value(const json_value_t *value) {
  json_value_t *json_value;
  char *serialized;

  if (!value) {
    return NULL;
  }

  json_value = json_clone(value);
  if (!json_value) {
    return NULL;
  }

  serialized = json_serialize(json_value, NULL);
  json_free(json_value);
  return serialized;
}

static const turbo_tool_entry_t *turbo_tool_registry_find(const turbo_tool_registry_t *registry,
                                                          const char *name) {
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
  entries = (turbo_tool_entry_t *)realloc(registry->entries, new_capacity * sizeof(*entries));
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
    json_serialize_free(entry->parameters_json);
  } else {
    free(entry->parameters_json);
  }
  turbo_runtime_json_destroy(entry->parameters_schema);
  if (entry->user_data_free) {
    entry->user_data_free(entry->user_data);
  }
  if (entry->required_capabilities) {
    size_t index;
    for (index = 0; index < entry->required_capability_count; ++index) {
      free(entry->required_capabilities[index]);
    }
    free(entry->required_capabilities);
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

static int turbo_tool_execution_policy_valid(const turbo_tool_execution_policy_t *policy) {
  if (!policy) return 0;
  if (policy->mode < TURBO_TOOL_EXECUTION_SEQUENTIAL ||
      policy->mode > TURBO_TOOL_EXECUTION_EXCLUSIVE)
    return 0;
  return policy->idempotency >= TURBO_TOOL_IDEMPOTENCY_NONE &&
         policy->idempotency <= TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
}

static turbo_tool_status_t turbo_tool_registry_add_with_policy(
    turbo_tool_registry_t *registry, const turbo_tool_definition_t *definition,
    const turbo_tool_execution_policy_t *execution_policy, const char *const *required_capabilities,
    size_t required_capability_count) {
  turbo_tool_status_t status;
  turbo_tool_entry_t *entry;
  size_t capability_index;

  if (!registry || !definition || !definition->name || !definition->description ||
      (!definition->parameters_json && !definition->parameters_schema) ||
      (!definition->handler && !definition->json_value_handler) ||
      !turbo_tool_execution_policy_valid(execution_policy) ||
      (required_capability_count > 0 && !required_capabilities)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  for (capability_index = 0; capability_index < required_capability_count; ++capability_index) {
    size_t previous;
    if (!required_capabilities[capability_index] ||
        required_capabilities[capability_index][0] == '\0') {
      return TURBO_TOOL_INVALID_ARGUMENT;
    }
    for (previous = 0; previous < capability_index; ++previous) {
      if (strcmp(required_capabilities[previous], required_capabilities[capability_index]) == 0) {
        return TURBO_TOOL_INVALID_ARGUMENT;
      }
    }
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
  entry->parameters_json =
      definition->parameters_json
          ? turbo_tool_strdup(definition->parameters_json)
          : turbo_tool_registry_serialize_json_value(definition->parameters_schema);
  entry->parameters_json_serialized = definition->parameters_json ? 0 : 1;
  entry->parameters_schema =
      definition->parameters_schema
          ? turbo_tool_registry_clone_json_value(definition->parameters_schema)
          : NULL;
  if (required_capability_count > 0) {
    entry->required_capabilities =
        (char **)calloc(required_capability_count, sizeof(*entry->required_capabilities));
    if (entry->required_capabilities) {
      for (capability_index = 0; capability_index < required_capability_count; ++capability_index) {
        entry->required_capabilities[capability_index] =
            turbo_tool_strdup(required_capabilities[capability_index]);
        if (!entry->required_capabilities[capability_index]) break;
        ++entry->required_capability_count;
      }
    }
  }
  if (!entry->name || !entry->description || !entry->parameters_json ||
      (definition->parameters_schema && !entry->parameters_schema) ||
      entry->required_capability_count != required_capability_count) {
    turbo_tool_registry_free_entry(entry);
    memset(entry, 0, sizeof(*entry));
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  entry->strict = definition->strict;
  entry->handler = definition->handler;
  entry->json_value_handler = definition->json_value_handler;
  entry->user_data = definition->user_data;
  entry->user_data_free = definition->user_data_free;
  entry->execution_policy = *execution_policy;
  registry->count++;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_add(turbo_tool_registry_t *registry,
                                            const turbo_tool_definition_t *definition) {
  const turbo_tool_execution_policy_t legacy_policy = {TURBO_TOOL_EXECUTION_SEQUENTIAL,
                                                       TURBO_TOOL_IDEMPOTENCY_NONE};
  return turbo_tool_registry_add_with_policy(registry, definition, &legacy_policy, NULL, 0);
}

turbo_tool_status_t turbo_tool_registry_add_v2(turbo_tool_registry_t *registry,
                                               const turbo_tool_definition_v2_t *definition) {
  if (!definition || definition->struct_size < sizeof(*definition) ||
      definition->abi_version != TURBO_TOOL_DEFINITION_V2_ABI_VERSION) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  return turbo_tool_registry_add_with_policy(registry, &definition->definition,
                                             &definition->execution_policy, NULL, 0);
}

turbo_tool_status_t turbo_tool_registry_add_v3(turbo_tool_registry_t *registry,
                                               const turbo_tool_definition_v3_t *definition) {
  if (!definition || definition->struct_size < sizeof(*definition) ||
      definition->abi_version != TURBO_TOOL_DEFINITION_V3_ABI_VERSION) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  return turbo_tool_registry_add_with_policy(
      registry, &definition->definition, &definition->execution_policy,
      definition->required_capabilities, definition->required_capability_count);
}

turbo_tool_status_t turbo_tool_registry_remove(turbo_tool_registry_t *registry, const char *name) {
  size_t index;
  if (!registry || !name) return TURBO_TOOL_INVALID_ARGUMENT;
  for (index = 0; index < registry->count; ++index) {
    if (strcmp(registry->entries[index].name, name) != 0) continue;
    turbo_tool_registry_free_entry(&registry->entries[index]);
    if (index + 1 < registry->count) {
      memmove(&registry->entries[index], &registry->entries[index + 1],
              (registry->count - index - 1) * sizeof(*registry->entries));
    }
    --registry->count;
    memset(&registry->entries[registry->count], 0, sizeof(*registry->entries));
    return TURBO_TOOL_OK;
  }
  return TURBO_TOOL_NOT_FOUND;
}

size_t turbo_tool_registry_count(const turbo_tool_registry_t *registry) {
  return registry ? registry->count : 0;
}

turbo_tool_status_t turbo_tool_registry_get_definition(const turbo_tool_registry_t *registry,
                                                       size_t index,
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
  out_definition->json_value_handler = entry->json_value_handler;
  out_definition->user_data = entry->user_data;
  out_definition->user_data_free = entry->user_data_free;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t
turbo_tool_registry_get_execution_policy(const turbo_tool_registry_t *registry, const char *name,
                                         turbo_tool_execution_policy_t *out_policy) {
  const turbo_tool_entry_t *entry;
  if (!registry || !name || !out_policy) return TURBO_TOOL_INVALID_ARGUMENT;
  entry = turbo_tool_registry_find(registry, name);
  if (!entry) return TURBO_TOOL_NOT_FOUND;
  *out_policy = entry->execution_policy;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_get_required_capabilities(
    const turbo_tool_registry_t *registry, const char *name, const char *const **out_capabilities,
    size_t *out_count) {
  const turbo_tool_entry_t *entry;
  if (!registry || !name || !out_capabilities || !out_count) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  *out_capabilities = NULL;
  *out_count = 0;
  entry = turbo_tool_registry_find(registry, name);
  if (!entry) return TURBO_TOOL_NOT_FOUND;
  *out_capabilities = (const char *const *)entry->required_capabilities;
  *out_count = entry->required_capability_count;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_require_capability(turbo_tool_registry_t *registry,
                                                           const char *name,
                                                           const char *capability) {
  turbo_tool_entry_t *entry;
  char **resized;
  char *copy;
  size_t index;
  if (!registry || !name || !capability || capability[0] == '\0') {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  entry = (turbo_tool_entry_t *)turbo_tool_registry_find(registry, name);
  if (!entry) return TURBO_TOOL_NOT_FOUND;
  for (index = 0; index < entry->required_capability_count; ++index) {
    if (strcmp(entry->required_capabilities[index], capability) == 0) return TURBO_TOOL_OK;
  }
  copy = turbo_tool_strdup(capability);
  if (!copy) return TURBO_TOOL_OUT_OF_MEMORY;
  resized = (char **)realloc(entry->required_capabilities,
                             (entry->required_capability_count + 1) * sizeof(*resized));
  if (!resized) {
    free(copy);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  entry->required_capabilities = resized;
  entry->required_capabilities[entry->required_capability_count++] = copy;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_project(const turbo_tool_registry_t *source,
                                                const char *const *names, size_t name_count,
                                                turbo_tool_registry_t **out_projection) {
  turbo_tool_registry_t *projection;
  size_t name_index;

  if (!out_projection || !source || (name_count > 0 && !names)) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  *out_projection = NULL;

  projection = turbo_tool_registry_create();
  if (!projection) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  for (name_index = 0; name_index < name_count; ++name_index) {
    const turbo_tool_entry_t *entry;
    turbo_tool_definition_v3_t definition;
    turbo_tool_status_t status;
    size_t previous;

    if (!names[name_index] || names[name_index][0] == '\0') {
      turbo_tool_registry_destroy(projection);
      return TURBO_TOOL_INVALID_ARGUMENT;
    }
    for (previous = 0; previous < name_index; ++previous) {
      if (strcmp(names[previous], names[name_index]) == 0) {
        turbo_tool_registry_destroy(projection);
        return TURBO_TOOL_DUPLICATE;
      }
    }

    entry = NULL;
    for (previous = 0; previous < source->count; ++previous) {
      if (strcmp(source->entries[previous].name, names[name_index]) == 0) {
        entry = &source->entries[previous];
        break;
      }
    }
    if (!entry) {
      turbo_tool_registry_destroy(projection);
      return TURBO_TOOL_NOT_FOUND;
    }

    memset(&definition, 0, sizeof(definition));
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V3_ABI_VERSION;
    definition.definition.name = entry->name;
    definition.definition.description = entry->description;
    definition.definition.parameters_json = entry->parameters_json;
    definition.definition.parameters_schema = entry->parameters_schema;
    definition.definition.strict = entry->strict;
    definition.definition.handler = entry->handler;
    definition.definition.json_value_handler = entry->json_value_handler;
    definition.definition.user_data = entry->user_data;
    definition.definition.user_data_free = NULL;
    definition.execution_policy = entry->execution_policy;
    definition.required_capabilities = (const char *const *)entry->required_capabilities;
    definition.required_capability_count = entry->required_capability_count;

    status = turbo_tool_registry_add_v3(projection, &definition);
    if (status != TURBO_TOOL_OK) {
      turbo_tool_registry_destroy(projection);
      return status;
    }
  }

  *out_projection = projection;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_compose(const turbo_tool_registry_t *const *sources,
                                                size_t source_count,
                                                turbo_tool_registry_t **out_composite) {
  turbo_tool_registry_t *composite;
  size_t source_index;

  if (!out_composite || (source_count > 0 && !sources)) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_composite = NULL;
  composite = turbo_tool_registry_create();
  if (!composite) return TURBO_TOOL_OUT_OF_MEMORY;

  for (source_index = 0; source_index < source_count; ++source_index) {
    size_t tool_index;
    if (!sources[source_index]) {
      turbo_tool_registry_destroy(composite);
      return TURBO_TOOL_INVALID_ARGUMENT;
    }
    for (tool_index = 0; tool_index < turbo_tool_registry_count(sources[source_index]);
         ++tool_index) {
      turbo_tool_definition_v3_t definition;
      turbo_tool_status_t status;
      memset(&definition, 0, sizeof(definition));
      definition.struct_size = sizeof(definition);
      definition.abi_version = TURBO_TOOL_DEFINITION_V3_ABI_VERSION;
      status = turbo_tool_registry_get_definition(sources[source_index], tool_index,
                                                  &definition.definition);
      if (status != TURBO_TOOL_OK) {
        turbo_tool_registry_destroy(composite);
        return status;
      }
      definition.definition.user_data_free = NULL;
      status = turbo_tool_registry_get_execution_policy(
          sources[source_index], definition.definition.name, &definition.execution_policy);
      if (status == TURBO_TOOL_OK)
        status = turbo_tool_registry_get_required_capabilities(
            sources[source_index], definition.definition.name, &definition.required_capabilities,
            &definition.required_capability_count);
      if (status == TURBO_TOOL_OK) status = turbo_tool_registry_add_v3(composite, &definition);
      if (status != TURBO_TOOL_OK) {
        turbo_tool_registry_destroy(composite);
        return status;
      }
    }
  }
  *out_composite = composite;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_execute(const turbo_tool_registry_t *registry,
                                                const char *name, const char *arguments_json,
                                                char **out_output) {
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

turbo_tool_status_t turbo_tool_registry_execute_json_value(const turbo_tool_registry_t *registry,
                                                           const char *name,
                                                           const json_value_t *arguments,
                                                           json_value_t **out_result) {
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
  if (!entry->json_value_handler) {
    return TURBO_TOOL_ERROR;
  }

  rc = entry->json_value_handler(arguments, out_result, entry->user_data);
  if (rc != 0) {
    if (*out_result) {
      turbo_runtime_json_destroy(*out_result);
      *out_result = NULL;
    }
    return TURBO_TOOL_ERROR;
  }

  return TURBO_TOOL_OK;
}

json_value_t *turbo_tool_registry_build_openai_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_tools(registry);
}

json_value_t *
turbo_tool_registry_build_openai_compatible_chat_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_compatible_chat_tools(registry);
}

json_value_t *turbo_tool_registry_build_openai_chat_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_openai_chat_tools(registry);
}

json_value_t *turbo_tool_registry_build_anthropic_tools(const turbo_tool_registry_t *registry) {
  return turbo_tool_schema_build_anthropic_tools(registry);
}
