#include "praktor.h"
#include "turbo_fs.h"
#include "json_parser.h"
#include "turbo_runtime_json.h"
#include "turbo_str.h"
#include "turbo_tool_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct mock_json_field_s {
  char *key;
  char *string_value;
  double number_value;
  int is_number;
} mock_json_field_t;

struct json_value_s {
  json_type_t type;
  mock_json_field_t fields[16];
  size_t field_count;
};

typedef struct mock_tool_entry_s {
  char *name;
  char *description;
  char *parameters_json;
  turbo_tool_handler_fn handler;
  turbo_tool_json_value_handler_fn json_handler;
  void *user_data;
  turbo_tool_user_data_free_fn user_data_free;
  turbo_tool_execution_policy_t policy;
  char **capabilities;
  size_t capability_count;
} mock_tool_entry_t;

struct turbo_tool_registry_s {
  mock_tool_entry_t entries[16];
  size_t count;
};

static char *mock_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text) + 1;
  copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

tstr_t tstr_dup(const char *text) { return mock_strdup(text); }
void tstr_free(tstr_t text) { free(text); }

int turbo_fs_path_is_absolute(const char *path) {
  return path && path[0] == '/';
}

int turbo_fs_lstat(const char *path, turbo_fs_stat_t *metadata) {
  struct stat st;
  if (!path || !metadata || lstat(path, &st) != 0) return -1;
  memset(metadata, 0, sizeof(*metadata));
  metadata->is_symlink = S_ISLNK(st.st_mode);
  metadata->is_file = S_ISREG(st.st_mode);
  metadata->is_directory = S_ISDIR(st.st_mode);
  metadata->size = (unsigned long long)st.st_size;
  return 0;
}

static void mock_json_set_string(json_value_t *object, const char *key, const char *value) {
  mock_json_field_t *field;
  if (!object || object->type != JSON_OBJECT || !key || !value ||
      object->field_count >= 16) return;
  field = &object->fields[object->field_count++];
  field->key = mock_strdup(key);
  field->string_value = mock_strdup(value);
}

json_value_t *json_create_object(void) {
  json_value_t *value = (json_value_t *)calloc(1, sizeof(*value));
  if (value) value->type = JSON_OBJECT;
  return value;
}

void json_object_set_string(json_value_t *object, const char *key, const char *value) {
  mock_json_set_string(object, key, value ? value : "");
}

void json_object_set_number(json_value_t *object, const char *key, double value) {
  mock_json_field_t *field;
  if (!object || object->type != JSON_OBJECT || !key ||
      object->field_count >= 16) return;
  field = &object->fields[object->field_count++];
  field->key = mock_strdup(key);
  field->number_value = value;
  field->is_number = 1;
}

const char *json_get_string(const json_value_t *object, const char *key) {
  size_t i;
  if (!object || object->type != JSON_OBJECT || !key) return NULL;
  for (i = 0; i < object->field_count; ++i)
    if (!object->fields[i].is_number && object->fields[i].key &&
        strcmp(object->fields[i].key, key) == 0)
      return object->fields[i].string_value;
  return NULL;
}

double json_get_double(const json_value_t *object, const char *key, double default_value) {
  size_t i;
  if (!object || object->type != JSON_OBJECT || !key) return default_value;
  for (i = 0; i < object->field_count; ++i)
    if (object->fields[i].is_number && object->fields[i].key &&
        strcmp(object->fields[i].key, key) == 0)
      return object->fields[i].number_value;
  return default_value;
}

json_type_t json_type(const json_value_t *value) {
  return value ? value->type : JSON_NULL;
}

void json_free(json_value_t *value) {
  size_t i;
  if (!value) return;
  for (i = 0; i < value->field_count; ++i) {
    free(value->fields[i].key);
    free(value->fields[i].string_value);
  }
  free(value);
}

void turbo_runtime_json_destroy(json_value_t *value) {
  json_free(value);
}

json_value_t *json_parse(const char *data, size_t size) {
  char *text;
  json_value_t *object;
  if (!data || !size) return NULL;
  text = (char *)malloc(size + 1);
  if (!text) return NULL;
  memcpy(text, data, size);
  text[size] = '\0';
  if (!strchr(text, '{')) {
    free(text);
    return NULL;
  }
  object = json_create_object();
  if (!object) {
    free(text);
    return NULL;
  }
  if (strstr(text, "\"type\":\"object\"") ||
      strstr(text, "\"type\": \"object\""))
    mock_json_set_string(object, "type", "object");
  if (strstr(text, "\"workflow_status\":\"success\"") ||
      strstr(text, "\"workflow_status\": \"success\""))
    mock_json_set_string(object, "workflow_status", "success");
  if (strstr(text, "\"workflow_status\":\"failed\"") ||
      strstr(text, "\"workflow_status\": \"failed\""))
    mock_json_set_string(object, "workflow_status", "failed");
  free(text);
  return object;
}

int turbo_runtime_json_parse(const uint8_t *data, size_t size,
                             json_value_t **out_value) {
  if (!out_value) return -1;
  *out_value = json_parse((const char *)data, size);
  return *out_value ? 0 : -1;
}

char *json_serialize(const json_value_t *value, size_t *out_size) {
  char buffer[2048];
  size_t used = 0;
  size_t i;
  char *copy;
  if (!value || value->type != JSON_OBJECT) return NULL;
  buffer[used++] = '{';
  for (i = 0; i < value->field_count; ++i) {
    int written;
    const mock_json_field_t *field = &value->fields[i];
    if (i) buffer[used++] = ',';
    if (field->is_number)
      written = snprintf(buffer + used, sizeof(buffer) - used, "\"%s\":%.17g",
                         field->key, field->number_value);
    else
      written = snprintf(buffer + used, sizeof(buffer) - used, "\"%s\":\"%s\"",
                         field->key, field->string_value ? field->string_value : "");
    if (written < 0 || (size_t)written >= sizeof(buffer) - used) return NULL;
    used += (size_t)written;
  }
  if (used + 2 > sizeof(buffer)) return NULL;
  buffer[used++] = '}';
  buffer[used] = '\0';
  copy = (char *)malloc(used + 1);
  if (!copy) return NULL;
  memcpy(copy, buffer, used + 1);
  if (out_size) *out_size = used;
  return copy;
}

void json_serialize_free(char *text) { free(text); }

turbo_tool_registry_t *turbo_tool_registry_create(void) {
  return (turbo_tool_registry_t *)calloc(1, sizeof(turbo_tool_registry_t));
}

static void mock_entry_destroy(mock_tool_entry_t *entry) {
  size_t i;
  if (!entry) return;
  if (entry->user_data_free) entry->user_data_free(entry->user_data);
  free(entry->name);
  free(entry->description);
  free(entry->parameters_json);
  for (i = 0; i < entry->capability_count; ++i) free(entry->capabilities[i]);
  free(entry->capabilities);
  memset(entry, 0, sizeof(*entry));
}

void turbo_tool_registry_destroy(turbo_tool_registry_t *registry) {
  size_t i;
  if (!registry) return;
  for (i = 0; i < registry->count; ++i) mock_entry_destroy(&registry->entries[i]);
  free(registry);
}

static mock_tool_entry_t *mock_find(turbo_tool_registry_t *registry, const char *name) {
  size_t i;
  if (!registry || !name) return NULL;
  for (i = 0; i < registry->count; ++i)
    if (registry->entries[i].name && strcmp(registry->entries[i].name, name) == 0)
      return &registry->entries[i];
  return NULL;
}

turbo_tool_status_t turbo_tool_registry_add_v3(
    turbo_tool_registry_t *registry, const turbo_tool_definition_v3_t *definition) {
  mock_tool_entry_t *entry;
  size_t i;
  if (!registry || !definition || !definition->definition.name ||
      registry->count >= 16)
    return TURBO_TOOL_INVALID_ARGUMENT;
  if (mock_find(registry, definition->definition.name)) return TURBO_TOOL_DUPLICATE;
  entry = &registry->entries[registry->count];
  entry->name = mock_strdup(definition->definition.name);
  entry->description = mock_strdup(definition->definition.description);
  entry->parameters_json = mock_strdup(definition->definition.parameters_json);
  entry->handler = definition->definition.handler;
  entry->json_handler = definition->definition.json_value_handler;
  entry->user_data = definition->definition.user_data;
  entry->user_data_free = definition->definition.user_data_free;
  entry->policy = definition->execution_policy;
  entry->capability_count = definition->required_capability_count;
  if (entry->capability_count) {
    entry->capabilities = (char **)calloc(entry->capability_count, sizeof(char *));
    if (!entry->capabilities) return TURBO_TOOL_OUT_OF_MEMORY;
    for (i = 0; i < entry->capability_count; ++i) {
      entry->capabilities[i] = mock_strdup(definition->required_capabilities[i]);
      if (!entry->capabilities[i]) return TURBO_TOOL_OUT_OF_MEMORY;
    }
  }
  ++registry->count;
  return TURBO_TOOL_OK;
}

size_t turbo_tool_registry_count(const turbo_tool_registry_t *registry) {
  return registry ? registry->count : 0;
}

turbo_tool_status_t turbo_tool_registry_get_execution_policy(
    const turbo_tool_registry_t *registry, const char *name,
    turbo_tool_execution_policy_t *out_policy) {
  mock_tool_entry_t *entry = mock_find((turbo_tool_registry_t *)registry, name);
  if (!entry || !out_policy) return TURBO_TOOL_NOT_FOUND;
  *out_policy = entry->policy;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_get_required_capabilities(
    const turbo_tool_registry_t *registry, const char *name,
    const char *const **out_capabilities, size_t *out_count) {
  mock_tool_entry_t *entry = mock_find((turbo_tool_registry_t *)registry, name);
  if (!entry || !out_capabilities || !out_count) return TURBO_TOOL_NOT_FOUND;
  *out_capabilities = (const char *const *)entry->capabilities;
  *out_count = entry->capability_count;
  return TURBO_TOOL_OK;
}

turbo_tool_status_t turbo_tool_registry_execute(
    const turbo_tool_registry_t *registry, const char *name,
    const char *arguments_json, char **out_output) {
  mock_tool_entry_t *entry = mock_find((turbo_tool_registry_t *)registry, name);
  if (!entry || !entry->handler || !out_output) return TURBO_TOOL_NOT_FOUND;
  *out_output = NULL;
  return entry->handler(arguments_json, out_output, entry->user_data) == 0
             ? TURBO_TOOL_OK
             : TURBO_TOOL_ERROR;
}

turbo_tool_status_t turbo_tool_registry_execute_json_value(
    const turbo_tool_registry_t *registry, const char *name,
    const json_value_t *arguments, json_value_t **out_result) {
  mock_tool_entry_t *entry = mock_find((turbo_tool_registry_t *)registry, name);
  if (!entry || !entry->json_handler || !out_result) return TURBO_TOOL_NOT_FOUND;
  *out_result = NULL;
  return entry->json_handler(arguments, out_result, entry->user_data) == 0
             ? TURBO_TOOL_OK
             : TURBO_TOOL_ERROR;
}

#ifndef TURBO_PRAKTOR_CONTRACT_USE_REAL_PRAKTOR
static int32_t mock_praktor_execute(
    const praktor_execute_request *request, praktor_owned_json *output,
    praktor_error *error) {
  static const char result[] = "{\"workflow_status\":\"success\"}";
  (void)error;
  if (!request || !request->workflow_path || !request->input_json || !output)
    return PRAKTOR_RESULT_INVALID_ARGUMENT;
  output->data = mock_strdup(result);
  if (!output->data) return PRAKTOR_RESULT_OUT_OF_MEMORY;
  output->size = sizeof(result) - 1;
  return PRAKTOR_RESULT_SUCCESS;
}

static void mock_praktor_release(praktor_owned_json *output) {
  if (!output) return;
  free(output->data);
  output->data = NULL;
  output->size = 0;
}

const praktor_api *praktor_get_api(void) {
  static const praktor_api api = {
      sizeof(praktor_api), PRAKTOR_ABI_MAJOR, PRAKTOR_ABI_MINOR,
      PRAKTOR_CAPABILITY_JSON_WORKFLOW, mock_praktor_execute, mock_praktor_release};
  return &api;
}

#endif
