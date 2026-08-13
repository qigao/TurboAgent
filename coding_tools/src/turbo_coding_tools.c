#include "turbo_coding_tools.h"

#include <turbo_fs.h>
#include <turbo_str_view.h>

#include <stdlib.h>
#include <string.h>

#define TURBO_CODING_DEFAULT_MAX_READ_BYTES (1024u * 1024u)
#define TURBO_CODING_DEFAULT_MAX_LIST_ENTRIES 1000u
#define TURBO_CODING_DEFAULT_MAX_RESULT_BYTES (2u * 1024u * 1024u)

typedef struct turbo_coding_binding_s {
  char *workspace_root;
  size_t max_read_bytes;
  size_t max_list_entries;
  size_t max_result_bytes;
} turbo_coding_binding_t;

static char *turbo_coding_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text) + 1;
  copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

void turbo_coding_tools_config_init(turbo_coding_tools_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_CODING_TOOLS_CONFIG_ABI_VERSION;
  config->max_read_bytes = TURBO_CODING_DEFAULT_MAX_READ_BYTES;
  config->max_list_entries = TURBO_CODING_DEFAULT_MAX_LIST_ENTRIES;
  config->max_result_bytes = TURBO_CODING_DEFAULT_MAX_RESULT_BYTES;
}

static void turbo_coding_binding_destroy(void *user_data) {
  turbo_coding_binding_t *binding = (turbo_coding_binding_t *)user_data;
  if (!binding) return;
  free(binding->workspace_root);
  free(binding);
}

static turbo_coding_binding_t *
turbo_coding_binding_create(const turbo_coding_tools_config_t *config) {
  turbo_coding_binding_t *binding = (turbo_coding_binding_t *)calloc(1, sizeof(*binding));
  if (!binding) return NULL;
  binding->workspace_root = turbo_coding_strdup(config->workspace_root);
  binding->max_read_bytes = config->max_read_bytes;
  binding->max_list_entries = config->max_list_entries;
  binding->max_result_bytes = config->max_result_bytes;
  if (!binding->workspace_root) {
    turbo_coding_binding_destroy(binding);
    return NULL;
  }
  return binding;
}

static int turbo_coding_relative_path_valid(const char *path) {
  const char *segment;
  const char *cursor;
  if (!path || !path[0] || turbo_fs_path_is_absolute(path) || strchr(path, ':')) return 0;
  segment = path;
  for (cursor = path;; ++cursor) {
    if (*cursor == '/' || *cursor == '\\' || *cursor == '\0') {
      size_t length = (size_t)(cursor - segment);
      if (length == 0 || (length == 1 && segment[0] == '.') ||
          (length == 2 && segment[0] == '.' && segment[1] == '.'))
        return 0;
      if (*cursor == '\0') break;
      segment = cursor + 1;
    }
  }
  return 1;
}

static char *turbo_coding_resolve_path(const turbo_coding_binding_t *binding,
                                       const char *relative_path, int require_directory) {
  turbo_fs_stat_t metadata;
  char *resolved;
  char *cursor;
  size_t root_length;
  size_t path_length;
  if (!binding || !turbo_coding_relative_path_valid(relative_path) ||
      turbo_fs_lstat(binding->workspace_root, &metadata) != 0 || metadata.is_symlink ||
      !metadata.is_directory)
    return NULL;
  root_length = strlen(binding->workspace_root);
  path_length = strlen(relative_path);
  if (root_length > SIZE_MAX - path_length - 2) return NULL;
  resolved = (char *)malloc(root_length + path_length + 2);
  if (!resolved) return NULL;
  memcpy(resolved, binding->workspace_root, root_length);
  if (root_length > 0 && resolved[root_length - 1] != '/' && resolved[root_length - 1] != '\\')
    resolved[root_length++] = '/';
  memcpy(resolved + root_length, relative_path, path_length + 1);
  for (cursor = resolved + root_length;; ++cursor) {
    char saved;
    if (*cursor != '/' && *cursor != '\\' && *cursor != '\0') continue;
    saved = *cursor;
    *cursor = '\0';
    if (turbo_fs_lstat(resolved, &metadata) != 0 || metadata.is_symlink) {
      free(resolved);
      return NULL;
    }
    *cursor = saved;
    if (saved == '\0') break;
  }
  if ((require_directory && !metadata.is_directory) || (!require_directory && !metadata.is_file)) {
    free(resolved);
    return NULL;
  }
  return resolved;
}

static json_value_t *turbo_coding_result(int ok, const char *status, const char *summary) {
  json_value_t *result = turbo_json_create_object();
  json_value_t *artifacts = turbo_json_create_array();
  json_value_t *metrics = turbo_json_create_object();
  if (!result || !artifacts || !metrics) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(artifacts);
    turbo_runtime_json_destroy(metrics);
    return NULL;
  }
  turbo_json_object_set_bool(result, "ok", ok);
  turbo_json_object_set_string(result, "status", status);
  turbo_json_object_set_number(result, "exit_code", ok ? 0 : -1);
  turbo_json_object_set_string(result, "summary", summary);
  turbo_json_object_set_string(result, "stdout", "");
  turbo_json_object_set_string(result, "stderr", "");
  turbo_json_object_set_bool(result, "truncated", 0);
  turbo_json_object_set_bool(result, "retryable", 0);
  turbo_json_object_add(result, "artifacts", artifacts);
  turbo_json_object_set_number(metrics, "duration_ms", 0);
  turbo_json_object_set_number(metrics, "output_bytes", 0);
  turbo_json_object_add(result, "metrics", metrics);
  return result;
}

static int turbo_coding_fs_read_json(const json_value_t *arguments, json_value_t **out_result,
                                     void *user_data) {
  turbo_coding_binding_t *binding = (turbo_coding_binding_t *)user_data;
  const char *path = turbo_json_get_string(arguments, "path");
  turbo_fs_stat_t metadata;
  turbo_fs_buf_t buffer = {0};
  json_value_t *result;
  char *content = NULL;
  char *resolved = turbo_coding_resolve_path(binding, path, 0);
  *out_result = NULL;
  if (!resolved) {
    *out_result = turbo_coding_result(0, "rejected", "path is outside the workspace policy");
    return *out_result ? 0 : -1;
  }
  if (turbo_fs_stat(resolved, &metadata) != 0 || metadata.size > binding->max_read_bytes ||
      metadata.size > binding->max_result_bytes || turbo_fs_read_file(resolved, &buffer) != 0 ||
      buffer.len > binding->max_read_bytes || buffer.len > binding->max_result_bytes ||
      (buffer.len > 0 && memchr(buffer.base, '\0', buffer.len) != NULL) ||
      !tstr_v_utf8_valid(tstr_v_from_buf(buffer.base, buffer.len))) {
    free(resolved);
    turbo_fs_buf_free(&buffer);
    *out_result = turbo_coding_result(0, "failed", "file cannot be read within configured bounds");
    return *out_result ? 0 : -1;
  }
  content = (char *)malloc(buffer.len + 1);
  if (!content) {
    free(resolved);
    turbo_fs_buf_free(&buffer);
    return -1;
  }
  memcpy(content, buffer.base, buffer.len);
  content[buffer.len] = '\0';
  result = turbo_coding_result(1, "completed", "file read completed");
  if (!result) {
    free(content);
    free(resolved);
    turbo_fs_buf_free(&buffer);
    return -1;
  }
  turbo_json_object_set_string(result, "path", path);
  turbo_json_object_set_string(result, "content", content);
  turbo_json_object_set_number(turbo_json_object_get(result, "metrics"), "output_bytes",
                               (double)buffer.len);
  free(resolved);
  free(content);
  turbo_fs_buf_free(&buffer);
  *out_result = result;
  return 0;
}

static const char *turbo_coding_dirent_type(turbo_fs_dirent_type_t type) {
  switch (type) {
  case TURBO_FS_DIRENT_FILE:
    return "file";
  case TURBO_FS_DIRENT_DIRECTORY:
    return "directory";
  case TURBO_FS_DIRENT_SYMLINK:
    return "symlink";
  default:
    return "other";
  }
}

static int turbo_coding_fs_list_json(const json_value_t *arguments, json_value_t **out_result,
                                     void *user_data) {
  turbo_coding_binding_t *binding = (turbo_coding_binding_t *)user_data;
  const char *path = turbo_json_get_string(arguments, "path");
  turbo_fs_dir_t *directory = NULL;
  turbo_fs_dirent_t entry;
  json_value_t *result = NULL;
  json_value_t *entries = NULL;
  char *resolved = turbo_coding_resolve_path(binding, path, 1);
  size_t count = 0;
  size_t output_bytes = 0;
  int read_status;
  *out_result = NULL;
  if (!resolved || turbo_fs_opendir(resolved, &directory) != 0) {
    free(resolved);
    *out_result = turbo_coding_result(0, "rejected", "directory is outside the workspace policy");
    return *out_result ? 0 : -1;
  }
  result = turbo_coding_result(1, "completed", "directory listing completed");
  entries = turbo_json_create_array();
  if (!result || !entries) goto fail;
  while ((read_status = turbo_fs_readdir(directory, &entry)) > 0) {
    json_value_t *item;
    size_t name_length = strlen(entry.name);
    if (count >= binding->max_list_entries ||
        name_length > binding->max_result_bytes - output_bytes) {
      turbo_json_object_set_bool(result, "truncated", 1);
      break;
    }
    item = turbo_json_create_object();
    if (!item) goto fail;
    turbo_json_object_set_string(item, "name", entry.name);
    turbo_json_object_set_string(item, "type", turbo_coding_dirent_type(entry.type));
    turbo_json_array_add(entries, item);
    output_bytes += name_length;
    ++count;
  }
  if (read_status < 0) goto fail;
  turbo_json_object_add(result, "entries", entries);
  entries = NULL;
  turbo_json_object_set_number(turbo_json_object_get(result, "metrics"), "output_bytes",
                               (double)output_bytes);
  turbo_fs_closedir(directory);
  free(resolved);
  *out_result = result;
  return 0;
fail:
  turbo_runtime_json_destroy(entries);
  turbo_runtime_json_destroy(result);
  turbo_fs_closedir(directory);
  free(resolved);
  return -1;
}

static int turbo_coding_json_bridge(const char *arguments_json, char **out_output, void *user_data,
                                    int (*handler)(const json_value_t *, json_value_t **, void *)) {
  json_value_t *arguments = NULL;
  json_value_t *result = NULL;
  int rc;
  *out_output = NULL;
  if (!arguments_json ||
      turbo_parse_json((const uint8_t *)arguments_json, strlen(arguments_json), &arguments) != 0)
    return -1;
  rc = handler(arguments, &result, user_data);
  turbo_runtime_json_destroy(arguments);
  if (rc != 0 || !result) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_output = turbo_json_serialize(result, NULL);
  turbo_runtime_json_destroy(result);
  return *out_output ? 0 : -1;
}

static int turbo_coding_fs_read(const char *arguments_json, char **out_output, void *user_data) {
  return turbo_coding_json_bridge(arguments_json, out_output, user_data, turbo_coding_fs_read_json);
}

static int turbo_coding_fs_list(const char *arguments_json, char **out_output, void *user_data) {
  return turbo_coding_json_bridge(arguments_json, out_output, user_data, turbo_coding_fs_list_json);
}

turbo_tool_status_t turbo_coding_tools_add_read_only(turbo_tool_registry_t *registry,
                                                     const turbo_coding_tools_config_t *config) {
  static const char *read_schema =
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}";
  static const char *list_schema =
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}";
  turbo_coding_binding_t *read_binding;
  turbo_coding_binding_t *list_binding;
  turbo_tool_definition_v2_t read_definition;
  turbo_tool_definition_v2_t list_definition;
  turbo_tool_status_t status;
  if (!registry || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_CODING_TOOLS_CONFIG_ABI_VERSION || !config->workspace_root ||
      !turbo_fs_path_is_absolute(config->workspace_root) || config->max_read_bytes == 0 ||
      config->max_list_entries == 0 || config->max_result_bytes == 0)
    return TURBO_TOOL_INVALID_ARGUMENT;
  read_binding = turbo_coding_binding_create(config);
  list_binding = turbo_coding_binding_create(config);
  if (!read_binding || !list_binding) {
    turbo_coding_binding_destroy(read_binding);
    turbo_coding_binding_destroy(list_binding);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  memset(&read_definition, 0, sizeof(read_definition));
  read_definition.struct_size = sizeof(read_definition);
  read_definition.abi_version = TURBO_TOOL_DEFINITION_V2_ABI_VERSION;
  read_definition.definition = (turbo_tool_definition_t){"fs.read",
                                                         "Read one bounded UTF-8 workspace file.",
                                                         read_schema,
                                                         NULL,
                                                         1,
                                                         turbo_coding_fs_read,
                                                         turbo_coding_fs_read_json,
                                                         read_binding,
                                                         turbo_coding_binding_destroy};
  read_definition.execution_policy = (turbo_tool_execution_policy_t){
      TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  status = turbo_tool_registry_add_v2(registry, &read_definition);
  if (status != TURBO_TOOL_OK) {
    turbo_coding_binding_destroy(read_binding);
    turbo_coding_binding_destroy(list_binding);
    return status;
  }
  memset(&list_definition, 0, sizeof(list_definition));
  list_definition.struct_size = sizeof(list_definition);
  list_definition.abi_version = TURBO_TOOL_DEFINITION_V2_ABI_VERSION;
  list_definition.definition = (turbo_tool_definition_t){"fs.list",
                                                         "List one bounded workspace directory.",
                                                         list_schema,
                                                         NULL,
                                                         1,
                                                         turbo_coding_fs_list,
                                                         turbo_coding_fs_list_json,
                                                         list_binding,
                                                         turbo_coding_binding_destroy};
  list_definition.execution_policy = (turbo_tool_execution_policy_t){
      TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  status = turbo_tool_registry_add_v2(registry, &list_definition);
  if (status != TURBO_TOOL_OK) {
    turbo_coding_binding_destroy(list_binding);
    (void)turbo_tool_registry_remove(registry, "fs.read");
  }
  return status;
}
