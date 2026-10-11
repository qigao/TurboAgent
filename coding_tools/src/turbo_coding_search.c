#include "turbo_coding_tools_internal.h"
#include "workspace_native.h"
#include "workspace_tool_schemas.h"

#include <cmeta_fs.h>
#include <stdlib.h>
#include <string.h>
#include <vstr.h>

enum {
  SEARCH_CHUNK_BYTES = 4096,
  SEARCH_PATTERN_BYTES = 4096,
  SEARCH_ENTRY_LIMIT = 10000,
  SEARCH_MATCH_LIMIT = 1000
};
#define SEARCH_SCAN_BYTES (16u * 1024u * 1024u)
#define SEARCH_RESULT_BYTES (128u * 1024u)
#define SEARCH_PATH_BYTES ((size_t)SALTS_FS_ROOT_COMPONENT_MAX * SALTS_FS_ROOT_DEPTH_MAX)
#define SEARCH_WIRE_BYTES (6u * (SEARCH_PATH_BYTES + SEARCH_PATTERN_BYTES) + 512u)

typedef struct search_binding_s {
  cmeta_fs_root_t *root;
  size_t file_limit, entry_limit, match_limit, output_limit;
} search_binding_t;

typedef struct search_call_s {
  const search_binding_t *binding;
  const turbo_tool_execution_context_t *context;
  vstr pattern;
  json_value_t *matches;
  size_t entries, files, bytes, output_bytes;
  size_t binary, oversized, special;
  int truncated, stopped;
} search_call_t;

static void search_binding_free(void *user) {
  search_binding_t *binding = user;
  if (!binding) return;
  cmeta_fs_root_close(binding->root);
  free(binding);
}

static turbo_tool_status_t search_lines(search_call_t *call, const char *path, tstr content) {
  size_t offset = 0, line = 1, length = tstr_len(content), path_length = strlen(path);
  while (offset < length) {
    vstr remaining = vstr_from_buf(content + offset, length - offset);
    size_t newline = vstr_find_char(remaining, '\n');
    size_t count = newline == VSTR_NPOS ? remaining.len : newline;
    vstr text;
    turbo_tool_status_t status = turbo_coding_check_control(call->context);
    if (status != TURBO_TOOL_OK) return status;
    if (newline != VSTR_NPOS && count && remaining.data[count - 1] == '\r') --count;
    text = vstr_from_buf(remaining.data, count);
    if (vstr_contains(text, call->pattern)) {
      json_value_t *item, *value;
      size_t available = call->binding->output_limit - call->output_bytes;
      if (json_array_size(call->matches) >= call->binding->match_limit || path_length > available ||
          count > available - path_length) {
        call->truncated = call->stopped = 1;
        return TURBO_TOOL_OK;
      }
      item = json_create_object();
      value = json_create_string_n(text.data, text.len);
      if (!item || !value || turbo_runtime_json_object_set(item, "text", value) != 0) {
        json_free(value);
        json_free(item);
        return TURBO_TOOL_OUT_OF_MEMORY;
      }
      if (turbo_runtime_json_object_set_string(item, "path", path) != 0 ||
          turbo_runtime_json_object_set_int64(item, "line", (int64_t)line) != 0 ||
          turbo_runtime_json_array_append(call->matches, item) != 0) {
        json_free(item);
        return TURBO_TOOL_OUT_OF_MEMORY;
      }
      call->output_bytes += path_length + count;
    }
    if (newline == VSTR_NPOS) break;
    offset += newline + 1;
    ++line;
  }
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t search_file(search_call_t *call, const char *path) {
  cmeta_fs_root_file_t *file = NULL;
  cmeta_fs_stat_t metadata;
  tstr content = NULL;
  char chunk[SEARCH_CHUNK_BYTES];
  turbo_tool_status_t status = turbo_coding_check_control(call->context);
  if (status != TURBO_TOOL_OK) return status;
  status = TURBO_TOOL_ERROR;
  if (cmeta_fs_root_file_open(call->binding->root, path, SALTS_FS_O_RDONLY, 0, &file) != 0 ||
      cmeta_fs_root_file_stat(file, &metadata) != 0)
    goto done;
  status = TURBO_TOOL_OK;
  if (metadata.size > call->binding->file_limit) {
    ++call->oversized;
    call->truncated = 1;
    goto done;
  }
  if (metadata.size > SEARCH_SCAN_BYTES - call->bytes) {
    call->truncated = call->stopped = 1;
    goto done;
  }
  content = tstr_new();
  if (!content) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto done;
  }
  for (;;) {
    size_t amount = SEARCH_SCAN_BYTES - call->bytes;
    size_t file_remaining = call->binding->file_limit - tstr_len(content);
    int count;
    tstr next;
    status = turbo_coding_check_control(call->context);
    if (status != TURBO_TOOL_OK) goto done;
    if (!amount) {
      /* Do not spend an extra byte probing EOF after the total read budget.
       * The retained handle lets a complete file reach validation/matching;
       * concurrent writers still have the ordinary non-snapshot semantics. */
      if (cmeta_fs_root_file_stat(file, &metadata) != 0) {
        status = TURBO_TOOL_ERROR;
        goto done;
      }
      if (metadata.size <= tstr_len(content)) break;
      call->truncated = call->stopped = 1;
      goto done;
    }
    if (amount > sizeof(chunk)) amount = sizeof(chunk);
    /* One extra byte detects growth past the per-file limit; that probe also
     * counts against the global read budget. file_limit is capped at 16 MiB. */
    if (amount > file_remaining + 1) amount = file_remaining + 1;
    count = cmeta_fs_root_file_read(file, chunk, amount);
    if (count < 0) {
      status = TURBO_TOOL_ERROR;
      goto done;
    }
    if (!count) break;
    call->bytes += (size_t)count;
    if ((size_t)count > file_remaining) {
      ++call->oversized;
      call->truncated = 1;
      goto done;
    }
    next = tstr_cat_len(content, chunk, (size_t)count);
    if (!next) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto done;
    }
    content = next;
  }
  if (memchr(content, 0, tstr_len(content)) || !vstr_utf8_valid(tstr_to_v(content))) {
    ++call->binary;
    goto done;
  }
  ++call->files;
  status = search_lines(call, path, content);
done:
  if (file && cmeta_fs_root_file_close(file) != 0 && status == TURBO_TOOL_OK)
    status = TURBO_TOOL_ERROR;
  tstr_free(content);
  return status;
}

static turbo_tool_status_t search_directory(search_call_t *call, const char *path, size_t depth) {
  cmeta_fs_root_dir_t *directory = NULL;
  cmeta_fs_root_dirent_t entry;
  char name[SALTS_FS_ROOT_DIRENT_NAME_MAX + 1];
  uint64_t cookie = 0;
  turbo_tool_status_t status = turbo_coding_check_control(call->context);
  if (status != TURBO_TOOL_OK) return status;
  if ((depth == 0 ? cmeta_fs_root_opendir_self(call->binding->root, &directory)
                  : cmeta_fs_root_opendir(call->binding->root, path, &directory)) != 0)
    return TURBO_TOOL_ERROR;
  while (!call->stopped) {
    int rc;
    tstr child, next;
    status = turbo_coding_check_control(call->context);
    if (status != TURBO_TOOL_OK) break;
    rc = cmeta_fs_root_readdir(directory, cookie, name, sizeof(name), &entry);
    if (rc < 0) {
      status = TURBO_TOOL_ERROR;
      break;
    }
    if (!rc) break;
    cookie = entry.next_cookie;
    if (call->entries >= call->binding->entry_limit || depth >= SALTS_FS_ROOT_DEPTH_MAX) {
      call->truncated = call->stopped = 1;
      break;
    }
    ++call->entries;
    if (entry.type != SALTS_FS_DIRENT_FILE && entry.type != SALTS_FS_DIRENT_DIRECTORY) {
      ++call->special;
      continue;
    }
    if (!entry.name_length || entry.name_length > SALTS_FS_ROOT_DIRENT_NAME_MAX ||
        strlen(name) != entry.name_length ||
        !vstr_utf8_valid(vstr_from_buf(name, entry.name_length))) {
      status = TURBO_TOOL_ERROR;
      break;
    }
    child = depth ? tstr_dup(path) : tstr_new();
    if (!child) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      break;
    }
    if (depth) {
      next = tstr_cat(child, "/");
      if (!next) {
        tstr_free(child);
        status = TURBO_TOOL_OUT_OF_MEMORY;
        break;
      }
      child = next;
    }
    next = tstr_cat_len(child, name, entry.name_length);
    if (!next) {
      tstr_free(child);
      status = TURBO_TOOL_OUT_OF_MEMORY;
      break;
    }
    child = next;
    if (tstr_len(child) > SEARCH_PATH_BYTES) {
      call->truncated = call->stopped = 1;
    } else {
      status = entry.type == SALTS_FS_DIRENT_DIRECTORY ? search_directory(call, child, depth + 1)
                                                       : search_file(call, child);
    }
    tstr_free(child);
    if (status != TURBO_TOOL_OK) break;
  }
  if (cmeta_fs_root_closedir(directory) != 0 && status == TURBO_TOOL_OK) status = TURBO_TOOL_ERROR;
  return status;
}

static turbo_tool_status_t search_result(search_call_t *call, json_value_t **out) {
  json_value_t *result = turbo_coding_result(
      1, "completed",
      call->truncated ? "search limited; narrow path to continue" : "workspace search completed");
  json_value_t *truncated = json_create_bool(call->truncated != 0);
  json_value_t *metrics;
  if (!result || !truncated || turbo_runtime_json_object_set(result, "truncated", truncated) != 0) {
    json_free(truncated);
    json_free(result);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  metrics = json_object_get(result, "metrics");
  if (turbo_runtime_json_object_set_int64(metrics, "output_bytes", (int64_t)call->output_bytes) !=
          0 ||
      turbo_runtime_json_object_set_int64(metrics, "bytes_read", (int64_t)call->bytes) != 0 ||
      turbo_runtime_json_object_set_int64(metrics, "entries_visited", (int64_t)call->entries) !=
          0 ||
      turbo_runtime_json_object_set_int64(metrics, "files_searched", (int64_t)call->files) != 0 ||
      turbo_runtime_json_object_set_int64(metrics, "skipped_binary", (int64_t)call->binary) != 0 ||
      turbo_runtime_json_object_set_int64(metrics, "skipped_oversized", (int64_t)call->oversized) !=
          0 ||
      turbo_runtime_json_object_set_int64(metrics, "skipped_special", (int64_t)call->special) !=
          0 ||
      turbo_runtime_json_object_set(result, "matches", call->matches) != 0) {
    json_free(result);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }
  call->matches = NULL;
  *out = result;
  return TURBO_TOOL_OK;
}

static turbo_tool_status_t search_invoke(const json_value_t *arguments,
                                         const turbo_tool_execution_context_t *context,
                                         json_value_t **out, void *user) {
  WorkspaceSearch_t request;
  DataBind *codec = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindStatus parsed;
  cmeta_fs_stat_t metadata = {0};
  char *wire = NULL;
  size_t wire_length = 0, depth = 0;
  search_call_t call = {.binding = user, .context = context};
  turbo_tool_status_t status = turbo_coding_check_control(context);
  *out = NULL;
  if (status != TURBO_TOOL_OK) return status;
  WorkspaceSearch_init(&request);
  if (!WorkspaceSearch_tool_arguments_valid(arguments) ||
      json_string_len(json_object_get(arguments, "path")) > SEARCH_PATH_BYTES ||
      json_string_len(json_object_get(arguments, "pattern")) > SEARCH_PATTERN_BYTES)
    goto rejected;
  status = TURBO_TOOL_OUT_OF_MEMORY;
  wire = json_serialize(arguments, &wire_length);
  if (!wire || TurboWorkspace_codec_create(&codec, &error) != DATA_BIND_OK) goto done;
  parsed = WorkspaceSearch_from_json(codec, &request, wire, wire_length, &error);
  if (parsed == DATA_BIND_ERR_OOM) goto done;
  if (parsed != DATA_BIND_OK || !request.path || !request.pattern || !tstr_len(request.path) ||
      !tstr_len(request.pattern) || tstr_len(request.path) != strlen(request.path) ||
      tstr_len(request.pattern) != strlen(request.pattern) ||
      !vstr_utf8_valid(tstr_to_v(request.path)) || !vstr_utf8_valid(tstr_to_v(request.pattern)) ||
      strchr(request.pattern, '\r') || strchr(request.pattern, '\n'))
    goto rejected;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto done;
  call.pattern = tstr_to_v(request.pattern);
  if (strcmp(request.path, ".") != 0) {
    if (cmeta_fs_root_stat(call.binding->root, request.path, &metadata) != 0 ||
        (!metadata.is_file && !metadata.is_directory))
      goto rejected;
    depth = 1;
    for (const char *p = request.path; *p; ++p)
      if (*p == '/' || *p == '\\') ++depth;
  }
  call.matches = json_create_array();
  if (!call.matches) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto done;
  }
  if (depth && metadata.is_file) {
    call.entries = 1;
    status = search_file(&call, request.path);
  } else {
    status = search_directory(&call, request.path, depth);
  }
  if (status == TURBO_TOOL_ERROR) {
    *out = turbo_coding_result(0, "failed", "workspace search could not read an entry");
    status = *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
    goto done;
  }
  if (status != TURBO_TOOL_OK) goto done;
  status = turbo_coding_check_control(context);
  if (status == TURBO_TOOL_OK) status = search_result(&call, out);
  goto done;
rejected:
  *out = turbo_coding_result(0, "rejected", "invalid workspace search request or path");
  status = *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
done:
  json_free(call.matches);
  WorkspaceSearch_clear(&request);
  data_bind_free(codec);
  json_serialize_free(wire);
  return status;
}

static turbo_tool_status_t search_invoke_text(const char *arguments,
                                              const turbo_tool_execution_context_t *context,
                                              char **out, void *user) {
  json_value_t *input = NULL, *result = NULL;
  turbo_tool_status_t status;
  *out = NULL;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) return status;
  if (!arguments || strlen(arguments) > SEARCH_WIRE_BYTES) return TURBO_TOOL_INVALID_ARGUMENT;
  input = json_parse(arguments, strlen(arguments));
  if (!input) return TURBO_TOOL_INVALID_ARGUMENT;
  status = search_invoke(input, context, &result, user);
  json_free(input);
  if (status == TURBO_TOOL_OK) {
    *out = json_serialize(result, NULL);
    if (!*out) status = TURBO_TOOL_OUT_OF_MEMORY;
  }
  json_free(result);
  return status;
}

turbo_tool_status_t turbo_coding_search_add(turbo_tool_registry_t *registry,
                                            const turbo_coding_tools_config_t *config) {
  search_binding_t *binding = calloc(1, sizeof(*binding));
  json_value_t *schema = WorkspaceSearch_tool_schema();
  turbo_tool_definition_v4_t definition = {0};
  turbo_tool_status_t status = TURBO_TOOL_OUT_OF_MEMORY;
  if (!binding || !schema) goto fail;
  binding->file_limit =
      config->max_read_bytes < SEARCH_SCAN_BYTES ? config->max_read_bytes : SEARCH_SCAN_BYTES;
  binding->entry_limit =
      config->max_list_entries < SEARCH_ENTRY_LIMIT ? config->max_list_entries : SEARCH_ENTRY_LIMIT;
  binding->match_limit =
      config->max_list_entries < SEARCH_MATCH_LIMIT ? config->max_list_entries : SEARCH_MATCH_LIMIT;
  binding->output_limit = config->max_result_bytes < SEARCH_RESULT_BYTES ? config->max_result_bytes
                                                                         : SEARCH_RESULT_BYTES;
  if (cmeta_fs_root_open(config->workspace_root, &binding->root) != 0) {
    status = TURBO_TOOL_INVALID_ARGUMENT;
    goto fail;
  }
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition = (turbo_tool_definition_t){
      "fs.search",
      "Search UTF-8 workspace files for a case-sensitive literal, one result per matching line. "
      "path selects a file/directory; use . for the root. Check truncated and metrics for "
      "limits/skips.",
      NULL,
      schema,
      1,
      NULL,
      NULL,
      binding,
      search_binding_free};
  definition.execution_policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_PARALLEL_SAFE,
                                                                TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  definition.context_handler = search_invoke_text;
  definition.json_value_context_handler = search_invoke;
  status = turbo_tool_registry_add_v4(registry, &definition);
  if (status != TURBO_TOOL_OK) goto fail;
  json_free(schema);
  return TURBO_TOOL_OK;
fail:
  json_free(schema);
  search_binding_free(binding);
  return status;
}
