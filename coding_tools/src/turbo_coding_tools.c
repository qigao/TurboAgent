#include "turbo_coding_tools.h"
#include "turbo_coding_tools_internal.h"
#include "workspace_native.h"
#include "workspace_tool_schemas.h"

#include <cmeta_fs.h>
#include <salts/clock.h>
#include <vstr.h>

#include <stdlib.h>
#include <string.h>

#define TURBO_CODING_DEFAULT_MAX_READ_BYTES (1024u * 1024u)
#define TURBO_CODING_DEFAULT_MAX_LIST_ENTRIES 1000u
#define TURBO_CODING_DEFAULT_MAX_RESULT_BYTES (2u * 1024u * 1024u)
#define TURBO_CODING_MAX_PATH_BYTES \
  ((size_t)SALTS_FS_ROOT_COMPONENT_MAX * SALTS_FS_ROOT_DEPTH_MAX)

typedef struct turbo_coding_binding_s {
  cmeta_fs_root_t *root;
  size_t max_read_bytes;
  size_t max_list_entries;
  size_t max_result_bytes;
} turbo_coding_binding_t;

turbo_tool_status_t turbo_coding_check_control(const turbo_tool_execution_context_t *context) {
  int rc;
  if (!context) return TURBO_TOOL_OK;
  if (context->cancel_token) {
    rc = turbo_cancel_token_check(context->cancel_token);
    if (rc == SALTS_ETIMEDOUT) return TURBO_TOOL_DEADLINE_EXCEEDED;
    if (rc != SALTS_OK) return TURBO_TOOL_CANCELLED;
  }
  return context->deadline_mono_ms && cmeta_monotonic_ms() >= context->deadline_mono_ms
             ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_OK;
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
  if (binding->root) cmeta_fs_root_close(binding->root);
  free(binding);
}

static turbo_coding_binding_t *
turbo_coding_binding_create(const turbo_coding_tools_config_t *config, turbo_tool_status_t *status) {
  turbo_coding_binding_t *binding = calloc(1, sizeof(*binding));
  *status = TURBO_TOOL_OUT_OF_MEMORY;
  if (!binding) return NULL;
  binding->max_read_bytes = config->max_read_bytes;
  binding->max_list_entries = config->max_list_entries;
  binding->max_result_bytes = config->max_result_bytes;
  if (cmeta_fs_root_open(config->workspace_root, &binding->root) != 0) {
    *status = TURBO_TOOL_INVALID_ARGUMENT;
    turbo_coding_binding_destroy(binding);
    return NULL;
  }
  *status = TURBO_TOOL_OK;
  return binding;
}

json_value_t *turbo_coding_result(int ok, const char *status, const char *summary) {
  static const char base[] = "{\"ok\":false,\"exit_code\":-1,\"stdout\":\"\",\"stderr\":\"\","
                             "\"truncated\":false,\"retryable\":false,\"artifacts\":[],"
                             "\"metrics\":{\"duration_ms\":0,\"output_bytes\":0}}";
  json_value_t *result = json_parse(base, sizeof(base) - 1);
  json_value_t *ok_value = json_create_bool(ok != 0);
  if (!result || !ok_value || turbo_runtime_json_object_set(result, "ok", ok_value) != 0) {
    json_free(ok_value); json_free(result);
    return NULL;
  }
  if (turbo_runtime_json_object_set_string(result, "status", status) != 0 ||
      turbo_runtime_json_object_set_string(result, "summary", summary) != 0 ||
      turbo_runtime_json_object_set_int64(result, "exit_code", ok ? 0 : -1) != 0) {
    json_free(result);
    return NULL;
  }
  return result;
}

static turbo_tool_status_t turbo_coding_fs_read_path(
    const char *path, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  turbo_coding_binding_t *binding = user_data;
  cmeta_fs_root_file_t *file = NULL;
  cmeta_fs_stat_t metadata;
  tstr content = NULL;
  char chunk[4096];
  int count;
  turbo_tool_status_t status = turbo_coding_check_control(context);
  size_t limit = binding->max_read_bytes < binding->max_result_bytes
                     ? binding->max_read_bytes : binding->max_result_bytes;
  *out_result = NULL;
  if (status != TURBO_TOOL_OK) return status;
  if (cmeta_fs_root_file_open(binding->root, path, SALTS_FS_O_RDONLY, 0, &file) != 0) {
    *out_result = turbo_coding_result(0, "rejected", "path is outside the workspace policy");
    return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_ERROR;
  }
  status = TURBO_TOOL_ERROR;
  if (cmeta_fs_root_file_stat(file, &metadata) != 0 || metadata.size > limit) goto rejected;
  content = tstr_new();
  if (!content) goto done;
  for (;;) {
    tstr next;
    status = turbo_coding_check_control(context);
    if (status != TURBO_TOOL_OK) goto done;
    count = cmeta_fs_root_file_read(file, chunk, sizeof(chunk));
    if (count <= 0) break;
    status = TURBO_TOOL_ERROR;
    if ((size_t)count > limit - tstr_len(content)) goto rejected;
    next = tstr_cat_len(content, chunk, (size_t)count);
    if (!next) goto done;
    content = next;
  }
  if (count < 0 || memchr(content, 0, tstr_len(content)) || !vstr_utf8_valid(tstr_to_v(content))) goto rejected;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto done;
  status = TURBO_TOOL_ERROR;
  *out_result = turbo_coding_result(1, "completed", "file read completed");
  if (!*out_result) goto done;
  if (turbo_runtime_json_object_set_string(*out_result, "path", path) != 0 ||
      turbo_runtime_json_object_set_string(*out_result, "content", content) != 0 ||
      turbo_runtime_json_object_set_int64(json_object_get(*out_result, "metrics"),
                                        "output_bytes", (int64_t)tstr_len(content)) != 0) {
    json_free(*out_result); *out_result = NULL; goto done;
  }
  status = TURBO_TOOL_OK;
  goto done;
rejected:
  *out_result = turbo_coding_result(0, "failed", "file cannot be read within configured bounds");
  status = *out_result ? TURBO_TOOL_OK : TURBO_TOOL_ERROR;
done:
  if (cmeta_fs_root_file_close(file) != 0 && status == TURBO_TOOL_OK)
    status = TURBO_TOOL_ERROR;
  tstr_free(content);
  if (status == TURBO_TOOL_OK) status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) { json_free(*out_result); *out_result = NULL; }
  return status;
}

static const char *turbo_coding_dirent_type(cmeta_fs_dirent_type_t type) {
  switch (type) {
  case SALTS_FS_DIRENT_FILE:
    return "file";
  case SALTS_FS_DIRENT_DIRECTORY:
    return "directory";
  case SALTS_FS_DIRENT_SYMLINK:
    return "symlink";
  default:
    return "other";
  }
}

static turbo_tool_status_t turbo_coding_fs_list_path(
    const char *path, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  turbo_coding_binding_t *binding = (turbo_coding_binding_t *)user_data;
  cmeta_fs_root_dir_t *directory = NULL;
  cmeta_fs_root_dirent_t entry;
  char name[SALTS_FS_ROOT_DIRENT_NAME_MAX + 1];
  uint64_t cookie = 0;
  json_value_t *result = NULL;
  json_value_t *entries = NULL;
  size_t count = 0;
  size_t output_bytes = 0;
  int read_status;
  turbo_tool_status_t status = turbo_coding_check_control(context);
  *out_result = NULL;
  if (status != TURBO_TOOL_OK) return status;
  if ((strcmp(path, ".") == 0 ? cmeta_fs_root_opendir_self(binding->root, &directory)
                              : cmeta_fs_root_opendir(binding->root, path, &directory)) != 0) {
    *out_result = turbo_coding_result(0, "rejected", "directory is outside the workspace policy");
    return *out_result ? TURBO_TOOL_OK : TURBO_TOOL_ERROR;
  }
  status = TURBO_TOOL_ERROR;
  result = turbo_coding_result(1, "completed", "directory listing completed");
  entries = json_create_array();
  if (!result || !entries) goto fail;
  for (;;) {
    json_value_t *item;
    size_t name_length;
    status = turbo_coding_check_control(context);
    if (status != TURBO_TOOL_OK) goto fail;
    read_status = cmeta_fs_root_readdir(directory, cookie, name, sizeof(name), &entry);
    if (read_status <= 0) break;
    status = TURBO_TOOL_ERROR;
    name_length = entry.name_length;
    cookie = entry.next_cookie;
    if (count >= binding->max_list_entries ||
        name_length > binding->max_result_bytes - output_bytes) {
      json_value_t *truncated = json_create_bool(1);
      if (!truncated || turbo_runtime_json_object_set(result, "truncated", truncated) != 0) {
        json_free(truncated); goto fail;
      }
      break;
    }
    item = json_create_object();
    if (!item) goto fail;
    if (turbo_runtime_json_object_set_string(item, "name", name) != 0 ||
        turbo_runtime_json_object_set_string(item, "type", turbo_coding_dirent_type(entry.type)) != 0 ||
        turbo_runtime_json_array_append(entries, item) != 0) {
      json_free(item); goto fail;
    }
    output_bytes += name_length;
    ++count;
  }
  status = TURBO_TOOL_ERROR;
  if (read_status < 0) goto fail;
  if (turbo_runtime_json_object_set(result, "entries", entries) != 0) goto fail;
  entries = NULL;
  if (turbo_runtime_json_object_set_int64(json_object_get(result, "metrics"), "output_bytes",
                                         (int64_t)output_bytes) != 0) goto fail;
  read_status = cmeta_fs_root_closedir(directory);
  directory = NULL;
  if (read_status != 0) goto fail;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto fail;
  *out_result = result;
  return TURBO_TOOL_OK;
fail:
  turbo_runtime_json_destroy(entries);
  turbo_runtime_json_destroy(result);
  if (directory) (void)cmeta_fs_root_closedir(directory);
  return status;
}

/* Each parallel call owns its codec and decoded request. No mutable DataBind
 * instance or borrowed request string is shared between handlers. */
static turbo_tool_status_t turbo_coding_path_bridge(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data,
    turbo_tool_status_t (*handler)(const char *, const turbo_tool_execution_context_t *,
                                    json_value_t **, void *)) {
  WorkspacePath_t request;
  DataBind *codec = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindStatus status;
  char *wire = NULL;
  size_t wire_length = 0;
  turbo_tool_status_t rc = turbo_coding_check_control(context);
  *out_result = NULL;
  if (rc != TURBO_TOOL_OK) return rc;
  rc = TURBO_TOOL_ERROR;
  WorkspacePath_init(&request);
  /* The object-size rule is the additionalProperties=false contract. The
   * generated codec checks the required field and its native type. */
  if (!WorkspacePath_tool_arguments_valid(arguments) ||
      json_string_len(json_object_get(arguments, "path")) > TURBO_CODING_MAX_PATH_BYTES)
    goto rejected;
  wire = json_serialize(arguments, &wire_length);
  if (!wire || TurboWorkspace_codec_create(&codec, &error) != DATA_BIND_OK) goto done;
  status = WorkspacePath_from_json(codec, &request, wire, wire_length, &error);
  if (status == DATA_BIND_ERR_OOM) goto done;
  if (status != DATA_BIND_OK ||
      !request.path || tstr_len(request.path) != strlen(request.path)) goto rejected;
  rc = handler(request.path, context, out_result, user_data);
  goto done;
rejected:
  *out_result = turbo_coding_result(0, "rejected", "invalid workspace path request");
  rc = *out_result ? TURBO_TOOL_OK : TURBO_TOOL_ERROR;
done:
  WorkspacePath_clear(&request);
  data_bind_free(codec);
  json_serialize_free(wire);
  if (rc == TURBO_TOOL_OK) rc = turbo_coding_check_control(context);
  if (rc != TURBO_TOOL_OK) { json_free(*out_result); *out_result = NULL; }
  return rc;
}

static turbo_tool_status_t turbo_coding_fs_read_json_context(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  return turbo_coding_path_bridge(arguments, context, out_result, user_data,
                                  turbo_coding_fs_read_path);
}

static turbo_tool_status_t turbo_coding_fs_list_json_context(
    const json_value_t *arguments, const turbo_tool_execution_context_t *context,
    json_value_t **out_result, void *user_data) {
  return turbo_coding_path_bridge(arguments, context, out_result, user_data,
                                  turbo_coding_fs_list_path);
}

static int turbo_coding_fs_read_json(const json_value_t *arguments, json_value_t **out_result,
                                    void *user_data) {
  return turbo_coding_fs_read_json_context(arguments, NULL, out_result, user_data) == TURBO_TOOL_OK
             ? 0 : -1;
}

static int turbo_coding_fs_list_json(const json_value_t *arguments, json_value_t **out_result,
                                    void *user_data) {
  return turbo_coding_fs_list_json_context(arguments, NULL, out_result, user_data) == TURBO_TOOL_OK
             ? 0 : -1;
}

static turbo_tool_status_t turbo_coding_json_bridge(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data, turbo_tool_json_value_context_handler_fn handler) {
  json_value_t *arguments = NULL;
  json_value_t *result = NULL;
  turbo_tool_status_t status = turbo_coding_check_control(context);
  *out_output = NULL;
  if (status != TURBO_TOOL_OK) return status;
  if (!arguments_json) return TURBO_TOOL_ERROR;
  arguments = json_parse(arguments_json, strlen(arguments_json));
  if (!arguments) return TURBO_TOOL_ERROR;
  status = handler(arguments, context, &result, user_data);
  turbo_runtime_json_destroy(arguments);
  if (status != TURBO_TOOL_OK || !result) {
    turbo_runtime_json_destroy(result);
    return status == TURBO_TOOL_OK ? TURBO_TOOL_ERROR : status;
  }
  *out_output = json_serialize(result, NULL);
  turbo_runtime_json_destroy(result);
  status = *out_output ? turbo_coding_check_control(context) : TURBO_TOOL_ERROR;
  if (status != TURBO_TOOL_OK) { free(*out_output); *out_output = NULL; }
  return status;
}

static turbo_tool_status_t turbo_coding_fs_read_context(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data) {
  return turbo_coding_json_bridge(arguments_json, context, out_output, user_data,
                                  turbo_coding_fs_read_json_context);
}

static turbo_tool_status_t turbo_coding_fs_list_context(
    const char *arguments_json, const turbo_tool_execution_context_t *context,
    char **out_output, void *user_data) {
  return turbo_coding_json_bridge(arguments_json, context, out_output, user_data,
                                  turbo_coding_fs_list_json_context);
}

static int turbo_coding_fs_read(const char *arguments_json, char **out_output, void *user_data) {
  return turbo_coding_fs_read_context(arguments_json, NULL, out_output, user_data) == TURBO_TOOL_OK
             ? 0 : -1;
}

static int turbo_coding_fs_list(const char *arguments_json, char **out_output, void *user_data) {
  return turbo_coding_fs_list_context(arguments_json, NULL, out_output, user_data) == TURBO_TOOL_OK
             ? 0 : -1;
}

turbo_tool_status_t turbo_coding_tools_add_read_only(turbo_tool_registry_t *registry,
                                                     const turbo_coding_tools_config_t *config) {
  json_value_t *path_schema;
  turbo_coding_binding_t *read_binding;
  turbo_coding_binding_t *list_binding;
  turbo_tool_definition_v4_t read_definition;
  turbo_tool_definition_v4_t list_definition;
  turbo_tool_status_t status;
  if (!registry || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_CODING_TOOLS_CONFIG_ABI_VERSION || !config->workspace_root ||
      !cmeta_fs_path_is_absolute(config->workspace_root) || config->max_read_bytes == 0 ||
      config->max_list_entries == 0 || config->max_result_bytes == 0)
    return TURBO_TOOL_INVALID_ARGUMENT;
  read_binding = turbo_coding_binding_create(config, &status);
  if (!read_binding) return status;
  list_binding = turbo_coding_binding_create(config, &status);
  if (!list_binding) {
    turbo_coding_binding_destroy(read_binding);
    turbo_coding_binding_destroy(list_binding);
    return status;
  }
  path_schema = WorkspacePath_tool_schema();
  if (!path_schema) {
    turbo_coding_binding_destroy(read_binding);
    turbo_coding_binding_destroy(list_binding);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  memset(&read_definition, 0, sizeof(read_definition));
  read_definition.struct_size = sizeof(read_definition);
  read_definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  read_definition.definition = (turbo_tool_definition_t){"fs.read",
                                                         "Read one bounded UTF-8 workspace file.",
                                                         NULL,
                                                         path_schema,
                                                         1,
                                                         turbo_coding_fs_read,
                                                         turbo_coding_fs_read_json,
                                                         read_binding,
                                                         turbo_coding_binding_destroy};
  read_definition.execution_policy = (turbo_tool_execution_policy_t){
      TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  read_definition.context_handler = turbo_coding_fs_read_context;
  read_definition.json_value_context_handler = turbo_coding_fs_read_json_context;
  status = turbo_tool_registry_add_v4(registry, &read_definition);
  if (status != TURBO_TOOL_OK) {
    json_free(path_schema);
    turbo_coding_binding_destroy(read_binding);
    turbo_coding_binding_destroy(list_binding);
    return status;
  }
  memset(&list_definition, 0, sizeof(list_definition));
  list_definition.struct_size = sizeof(list_definition);
  list_definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  list_definition.definition = (turbo_tool_definition_t){"fs.list",
                                                         "List one bounded workspace directory.",
                                                         NULL,
                                                         path_schema,
                                                         1,
                                                         turbo_coding_fs_list,
                                                         turbo_coding_fs_list_json,
                                                         list_binding,
                                                         turbo_coding_binding_destroy};
  list_definition.execution_policy = (turbo_tool_execution_policy_t){
      TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  list_definition.context_handler = turbo_coding_fs_list_context;
  list_definition.json_value_context_handler = turbo_coding_fs_list_json_context;
  status = turbo_tool_registry_add_v4(registry, &list_definition);
  json_free(path_schema);
  if (status != TURBO_TOOL_OK) {
    turbo_coding_binding_destroy(list_binding);
    (void)turbo_tool_registry_remove(registry, "fs.read");
    return status;
  }
  status = turbo_coding_search_add(registry, config);
  if (status != TURBO_TOOL_OK) {
    (void)turbo_tool_registry_remove(registry, "fs.list");
    (void)turbo_tool_registry_remove(registry, "fs.read");
  }
  return status;
}
