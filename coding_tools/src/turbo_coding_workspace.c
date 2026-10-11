#include "turbo_coding_tools.h"
#include "turbo_coding_tools_internal.h"
#include "workspace_native.h"
#include "workspace_tool_schemas.h"

#include <cmeta_fs.h>
#include <cmeta_process.h>
#include <cmeta_uuid.h>
#include <salts/clock.h>
#include <vstr.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

enum { WORKSPACE_WRITE, WORKSPACE_EDIT, WORKSPACE_EXEC, WORKSPACE_ACTION_COUNT };
enum { WORKSPACE_IO_CHUNK = 4096, WORKSPACE_WAIT_MS = 25, WORKSPACE_DRAIN_MS = 5000 };
#define WORKSPACE_PATH_LIMIT ((size_t)SALTS_FS_ROOT_COMPONENT_MAX * SALTS_FS_ROOT_DEPTH_MAX)

typedef struct workspace_binding_s {
  turbo_coding_workspace_config_t config;
  cmeta_fs_root_t *root;
  tstr cwd;
  int action;
} workspace_binding_t;

void turbo_coding_workspace_config_init(turbo_coding_workspace_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_CODING_WORKSPACE_CONFIG_ABI_VERSION;
  turbo_coding_tools_config_init(&config->files);
  config->max_write_bytes = 1024u * 1024u;
  config->max_command_bytes = 64u * 1024u;
  config->max_process_output_bytes = 128u * 1024u;
  config->process_timeout_ms = 120000;
}

static void workspace_binding_free(void *data) {
  workspace_binding_t *binding = data;
  if (!binding) return;
  if (binding->root) cmeta_fs_root_close(binding->root);
  tstr_free(binding->cwd);
  free(binding);
}

static turbo_tool_status_t workspace_result(json_value_t **out, int ok,
                                           const char *status, const char *summary) {
  *out = turbo_coding_result(ok, status, summary);
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int workspace_text(tstr text) {
  return text && tstr_len(text) == strlen(text) && vstr_utf8_valid(vstr_from_buf(text, tstr_len(text)));
}

/* Bounded even if another process grows the file after stat. */
static turbo_tool_status_t workspace_read(workspace_binding_t *binding, const char *path,
                                         const turbo_tool_execution_context_t *context,
                                         tstr *out) {
  cmeta_fs_root_file_t *file = NULL;
  tstr text = NULL;
  char chunk[WORKSPACE_IO_CHUNK];
  int count;
  turbo_tool_status_t status = turbo_coding_check_control(context);
  *out = NULL;
  if (status != TURBO_TOOL_OK) return status;
  text = tstr_new();
  if (!text) return TURBO_TOOL_OUT_OF_MEMORY;
  status = TURBO_TOOL_ERROR;
  if (cmeta_fs_root_file_open(binding->root, path, SALTS_FS_O_RDONLY, 0, &file) != 0) goto done;
  for (;;) {
    tstr next;
    status = turbo_coding_check_control(context);
    if (status != TURBO_TOOL_OK) goto done;
    status = TURBO_TOOL_ERROR;
    count = cmeta_fs_root_file_read(file, chunk, sizeof(chunk));
    if (count <= 0) break;
    if ((size_t)count > binding->config.max_write_bytes - tstr_len(text)) goto done;
    next = tstr_cat_len(text, chunk, (size_t)count);
    if (!next) { status = TURBO_TOOL_OUT_OF_MEMORY; goto done; }
    text = next;
  }
  if (count < 0 || !workspace_text(text)) goto done;
  status = TURBO_TOOL_OK;
done:
  if (file && cmeta_fs_root_file_close(file) != 0 && status == TURBO_TOOL_OK)
    status = TURBO_TOOL_ERROR;
  if (status == TURBO_TOOL_OK) status = turbo_coding_check_control(context);
  if (status == TURBO_TOOL_OK) { *out = text; text = NULL; }
  tstr_free(text);
  return status;
}

static turbo_tool_status_t workspace_publish(workspace_binding_t *binding, const char *path,
                                            tstr content, const turbo_tool_execution_context_t *context,
                                            json_value_t **out) {
  cmeta_fs_stat_t metadata;
  cmeta_fs_root_file_t *file = NULL;
  cmeta_uuid_t id;
  char uuid[SALTS_UUID_STRING_SIZE];
  const char *separator = NULL, *cursor;
  tstr temporary = NULL, next;
  turbo_tool_status_t status;
  int rc, staged = 0, mode = 0600;
  size_t offset = 0;
  if (!path[0] || strlen(path) > WORKSPACE_PATH_LIMIT)
    return workspace_result(out, 0, "rejected", "invalid workspace path");
  rc = cmeta_fs_root_stat(binding->root, path, &metadata);
  if (rc == 0) {
    if (!metadata.is_file) return workspace_result(out, 0, "rejected", "target is not a regular file");
    mode = metadata.mode & 0777;
  } else if (rc != -ENOENT) {
    return workspace_result(out, 0, "rejected", "path cannot be admitted within the workspace");
  }
  if (cmeta_uuid_v4_generate(&id) != 0 || cmeta_uuid_format(&id, uuid, sizeof(uuid)) != 0)
    return TURBO_TOOL_ERROR;
  for (cursor = path; *cursor; ++cursor) if (*cursor == '/' || *cursor == '\\') separator = cursor;
  temporary = tstr_dup_len(path, separator ? (size_t)(separator - path) + 1 : 0);
  if (!temporary) return TURBO_TOOL_OUT_OF_MEMORY;
  next = tstr_cat(temporary, ".turbo-agent-");
  if (!next) { tstr_free(temporary); return TURBO_TOOL_OUT_OF_MEMORY; }
  temporary = next;
  next = tstr_cat(temporary, uuid);
  if (!next) { tstr_free(temporary); return TURBO_TOOL_OUT_OF_MEMORY; }
  temporary = next;
  /* Allocate the success response before the commit point. */
  status = workspace_result(out, 1, "completed", "file replacement committed");
  if (status != TURBO_TOOL_OK) goto done;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto done;
  rc = cmeta_fs_root_file_open(binding->root, temporary,
                             SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL,
                             mode, &file);
  if (rc != 0) goto io_error;
  staged = 1;
  while (offset < tstr_len(content)) {
    size_t amount = tstr_len(content) - offset;
    if (amount > WORKSPACE_IO_CHUNK) amount = WORKSPACE_IO_CHUNK;
    status = turbo_coding_check_control(context);
    if (status != TURBO_TOOL_OK) goto done;
    rc = cmeta_fs_root_file_write(file, content + offset, amount);
    if (rc <= 0) goto io_error;
    offset += (size_t)rc;
  }
  rc = cmeta_fs_root_file_close(file);
  file = NULL;
  if (rc != 0) goto io_error;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto done;
  if (cmeta_fs_root_rename(binding->root, temporary, binding->root, path) != 0) goto io_error;
  staged = 0;
  goto done;
io_error:
  json_free(*out); *out = NULL;
  status = workspace_result(out, 0, "failed", "file replacement was not committed");
done:
  if (file) cmeta_fs_root_file_close(file);
  if (staged && cmeta_fs_root_unlink(binding->root, temporary) != 0) {
    json_free(*out); *out = NULL;
    status = workspace_result(out, 0, "cleanup_failed", "target unchanged; staging file cleanup failed");
  }
  tstr_free(temporary);
  return status;
}

static turbo_tool_status_t workspace_edit(workspace_binding_t *binding, WorkspaceEdit_t *request,
                                         const turbo_tool_execution_context_t *context, json_value_t **out) {
  tstr original = NULL, replacement = NULL, next;
  const char *match;
  int conflict;
  size_t prefix, tail, total;
  turbo_tool_status_t status = workspace_read(binding, request->path, context, &original);
  if (status == TURBO_TOOL_ERROR)
    return workspace_result(out, 0, "failed", "file cannot be read within configured bounds");
  if (status != TURBO_TOOL_OK) return status;
  match = request->old_text[0] ? strstr(original, request->old_text) : NULL;
  conflict = !match || strstr(match + 1, request->old_text) != NULL;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) { tstr_free(original); return status; }
  if (conflict) {
    tstr_free(original);
    return workspace_result(out, 0, "conflict", "old_text must match exactly once");
  }
  prefix = (size_t)(match - original);
  tail = tstr_len(original) - prefix - tstr_len(request->old_text);
  total = tstr_len(original) - tstr_len(request->old_text);
  if (tstr_len(request->new_text) > binding->config.max_write_bytes - total) {
    tstr_free(original);
    return workspace_result(out, 0, "rejected", "replacement exceeds configured bounds");
  }
  replacement = tstr_dup_len(original, prefix);
  if (!replacement) goto oom;
  next = tstr_cat_len(replacement, request->new_text, tstr_len(request->new_text));
  if (!next) goto oom;
  replacement = next;
  next = tstr_cat_len(replacement, match + tstr_len(request->old_text), tail);
  if (!next) goto oom;
  replacement = next;
  status = workspace_publish(binding, request->path, replacement, context, out);
  tstr_free(replacement);
  tstr_free(original);
  return status;
oom:
  tstr_free(replacement);
  tstr_free(original);
  return TURBO_TOOL_OUT_OF_MEMORY;
}

static int workspace_capture(cmeta_process_t *process, int stderr_stream, tstr *out) {
  char buffer[WORKSPACE_IO_CHUNK];
  size_t count;
  int rc;
  *out = tstr_new();
  if (!*out) return -1;
  do {
    tstr next;
    rc = stderr_stream ? cmeta_process_read_stderr(process, buffer, sizeof(buffer), &count)
                       : cmeta_process_read_stdout(process, buffer, sizeof(buffer), &count);
    if (rc != 0 && rc != SALTS_EOF) return -1;
    if (!count) break;
    next = tstr_cat_len(*out, buffer, count);
    if (!next) return -1;
    *out = next;
  } while (rc != SALTS_EOF);
  return 0;
}

#ifdef _WIN32
static tstr workspace_system_shell(void) {
  wchar_t directory[MAX_PATH + 1];
  UINT count = GetSystemDirectoryW(directory, MAX_PATH + 1);
  int bytes;
  tstr path, next;
  if (!count || count > MAX_PATH) return NULL;
  bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, NULL, 0, NULL, NULL);
  if (bytes <= 0) return NULL;
  path = tstr_new_len(NULL, (size_t)bytes - 1);
  if (!path) return NULL;
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, directory, -1, path, bytes, NULL, NULL)) {
    tstr_free(path); return NULL;
  }
  next = tstr_cat(path, "\\cmd.exe");
  if (!next) tstr_free(path);
  return next;
}

/* cmeta_process quotes argv for the Windows CRT. cmd /c has a different quote
 * grammar, so passing user text as one argv item corrupts quoted commands.
 * A private batch file preserves the command text and needs no quoted argv. */
static int workspace_command_script(workspace_binding_t *binding, const char *command, tstr *out) {
  cmeta_uuid_t id;
  char uuid[SALTS_UUID_STRING_SIZE];
  cmeta_fs_root_file_t *file = NULL;
  tstr name = NULL, next;
  const char *parts[] = {"@echo off\r\n@chcp 65001>nul\r\n", command, "\r\n"};
  int rc = -1;
  *out = NULL;
  if (cmeta_uuid_v4_generate(&id) != 0 || cmeta_uuid_format(&id, uuid, sizeof(uuid)) != 0) return -1;
  name = tstr_dup(uuid);
  if (!name) return -1;
  next = tstr_cat(name, ".cmd");
  if (!next) { tstr_free(name); return -1; }
  name = next;
  if (cmeta_fs_root_file_open(binding->root, name,
          SALTS_FS_O_CREAT | SALTS_FS_O_WRONLY | SALTS_FS_ROOT_O_EXCL, 0600, &file) != 0) goto done;
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
    size_t offset = 0, length = strlen(parts[i]);
    while (offset < length) {
      int written = cmeta_fs_root_file_write(file, parts[i] + offset, length - offset);
      if (written <= 0) goto failed;
      offset += (size_t)written;
    }
  }
  rc = cmeta_fs_root_file_close(file); file = NULL;
  if (rc != 0) goto failed;
  *out = name;
  return 0;
failed:
  if (file) cmeta_fs_root_file_close(file);
  cmeta_fs_root_unlink(binding->root, name);
done:
  tstr_free(name);
  return -1;
}
#endif

static turbo_tool_status_t workspace_exec(workspace_binding_t *binding, const char *command,
                                          const turbo_tool_execution_context_t *context,
                                          json_value_t **out) {
  cmeta_process_options_t options;
  cmeta_process_result_t result;
  cmeta_process_t *process = NULL;
  turbo_tool_status_t status;
  turbo_tool_status_t interrupted = TURBO_TOOL_OK;
  tstr stdout_text = NULL, stderr_text = NULL;
  uint64_t started = cmeta_monotonic_ms();
  int rc, text_output;
#ifdef _WIN32
  tstr script = NULL;
  tstr shell_path = NULL;
  const char *args[] = {"/d", "/s", "/c", NULL, NULL};
#else
  const char *args[] = {"-c", command, NULL};
  const char *shell = "/bin/sh";
#endif
  cmeta_process_options_init(&options);
#ifndef _WIN32
  options.program = shell;
#endif
  options.args = args;
  options.cwd = binding->cwd;
  options.flags |= SALTS_PROCESS_PIPE_STDIN;
  options.timeout_ms = binding->config.process_timeout_ms;
  options.max_output_bytes = binding->config.max_process_output_bytes;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) return status;
#ifdef _WIN32
  shell_path = workspace_system_shell();
  if (!shell_path) return TURBO_TOOL_ERROR;
  options.program = shell_path;
  if (workspace_command_script(binding, command, &script) != 0) {
    status = workspace_result(out, 0, "failed", "command script could not be prepared");
    goto done;
  }
  args[3] = script;
#endif
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) goto done;
  if (cmeta_process_spawn(&options, &process) != 0) {
    status = workspace_result(out, 0, "failed", "process could not be started");
    goto done;
  }
  if (cmeta_process_close_stdin(process) != 0) {
    status = TURBO_TOOL_ERROR;
    goto done;
  }
  for (;;) {
    rc = cmeta_process_wait_for(process, WORKSPACE_WAIT_MS, &result);
    if (rc == 0) break;
    if (rc != SALTS_ETIMEDOUT) {
      status = TURBO_TOOL_ERROR;
      goto done;
    }
    status = turbo_coding_check_control(context);
    if (status != TURBO_TOOL_OK) {
      interrupted = status;
      if (cmeta_process_terminate(process) != SALTS_OK ||
          cmeta_process_wait_for(process, WORKSPACE_DRAIN_MS, &result) != SALTS_OK ||
          result.state == SALTS_PROCESS_WAIT_FAILED) {
        status = TURBO_TOOL_ERROR;
        goto done;
      }
      break;
    }
  }
  if (workspace_capture(process, 0, &stdout_text) != 0 ||
      workspace_capture(process, 1, &stderr_text) != 0) {
    status = TURBO_TOOL_ERROR;
    goto done;
  }
  text_output = workspace_text(stdout_text) && workspace_text(stderr_text);
  status = workspace_result(out,
                            interrupted == TURBO_TOOL_OK && text_output &&
                                result.state == SALTS_PROCESS_EXITED && result.exit_code == 0,
                            interrupted == TURBO_TOOL_CANCELLED           ? "cancelled"
                            : interrupted == TURBO_TOOL_DEADLINE_EXCEEDED ? "deadline_exceeded"
                            : text_output ? cmeta_process_state_name(result.state)
                                          : "binary_output",
                            !text_output ? "process output is not UTF-8 text"
                            : interrupted != TURBO_TOOL_OK
                                ? "process interrupted; side effects may already have occurred"
                                : "process finished");
  if (status != TURBO_TOOL_OK) goto done;
  if (turbo_runtime_json_object_set_string(*out, "stdout", text_output ? stdout_text : "") != 0 ||
      turbo_runtime_json_object_set_string(*out, "stderr", text_output ? stderr_text : "") != 0 ||
      turbo_runtime_json_object_set_string(*out, "process_state",
                                           cmeta_process_state_name(result.state)) != 0 ||
      turbo_runtime_json_object_set_int64(*out, "exit_code", result.exit_code) != 0 ||
      turbo_runtime_json_object_set_int64(json_object_get(*out, "metrics"), "duration_ms",
                                          (int64_t)(cmeta_monotonic_ms() - started)) != 0 ||
      turbo_runtime_json_object_set_int64(
          json_object_get(*out, "metrics"), "output_bytes",
          (int64_t)(tstr_len(stdout_text) + tstr_len(stderr_text))) != 0) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto done;
  }
  if (result.state == SALTS_PROCESS_OUTPUT_LIMIT_EXCEEDED) {
    json_value_t *truncated = json_create_bool(1);
    if (!truncated || turbo_runtime_json_object_set(*out, "truncated", truncated) != 0) {
      json_free(truncated);
      status = TURBO_TOOL_OUT_OF_MEMORY;
    }
  }
done:
  cmeta_process_destroy(process);
#ifdef _WIN32
  if (script && cmeta_fs_root_unlink(binding->root, script) != 0) {
    json_free(*out);
    *out = NULL;
    status =
        interrupted != TURBO_TOOL_OK
            ? TURBO_TOOL_ERROR
            : workspace_result(out, 0, "cleanup_failed", "command ended but script cleanup failed");
  }
  tstr_free(script);
  tstr_free(shell_path);
#endif
  tstr_free(stdout_text);
  tstr_free(stderr_text);
  if (status == TURBO_TOOL_OK && interrupted != TURBO_TOOL_OK) {
    /* Registry failures keep their NULL output contract. Diagnostic detail is
     * borrowed only after teardown; the executor owns its journal copy. */
    if (context && context->struct_size >= sizeof(*context) && context->detail_sink)
      context->detail_sink(*out, context->detail_sink_user_data);
    status = interrupted;
  }
  return status;
}

static turbo_tool_status_t workspace_invoke(const json_value_t *arguments,
                                            const turbo_tool_execution_context_t *context,
                                            json_value_t **out, void *data) {
  workspace_binding_t *binding = data;
  WorkspaceWrite_t write;
  WorkspaceEdit_t edit;
  WorkspaceCommand_t command;
  DataBind *codec = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindStatus decoded;
  turbo_tool_status_t status;
  char *wire = NULL;
  size_t length, i;
  size_t fields = binding->action == WORKSPACE_WRITE ? WorkspaceWrite_tool_field_count :
                  binding->action == WORKSPACE_EDIT ? WorkspaceEdit_tool_field_count : WorkspaceCommand_tool_field_count;
  *out = NULL;
  status = turbo_coding_check_control(context);
  if (status != TURBO_TOOL_OK) return status;
  if (!(binding->action == WORKSPACE_WRITE ? WorkspaceWrite_tool_arguments_valid(arguments) :
        binding->action == WORKSPACE_EDIT ? WorkspaceEdit_tool_arguments_valid(arguments) :
        WorkspaceCommand_tool_arguments_valid(arguments)))
    return workspace_result(out, 0, "rejected", "invalid tool arguments");
  for (i = 0; i < fields; ++i) {
    const json_value_t *field = json_object_value(arguments, i);
    size_t limit = binding->action == WORKSPACE_EXEC ? binding->config.max_command_bytes : binding->config.max_write_bytes;
    if (binding->action != WORKSPACE_EXEC && strcmp(json_object_key(arguments, i), "path") == 0)
      limit = WORKSPACE_PATH_LIMIT;
    if (json_type(field) != JSON_STRING || json_string_len(field) > limit ||
        json_string_len(field) != strlen(json_string(field)) || !vstr_utf8_valid(json_string_v(field)))
      return workspace_result(out, 0, "rejected", "arguments exceed text or size constraints");
  }
  WorkspaceWrite_init(&write); WorkspaceEdit_init(&edit); WorkspaceCommand_init(&command);
  wire = json_serialize(arguments, &length);
  status = TURBO_TOOL_OUT_OF_MEMORY;
  if (!wire) goto done;
  decoded = TurboWorkspace_codec_create(&codec, &error);
  if (decoded != DATA_BIND_OK) {
    status = decoded == DATA_BIND_ERR_OOM ? TURBO_TOOL_OUT_OF_MEMORY : TURBO_TOOL_ERROR;
    goto done;
  }
  if (binding->action == WORKSPACE_WRITE) decoded = WorkspaceWrite_from_json(codec, &write, wire, length, &error);
  else if (binding->action == WORKSPACE_EDIT) decoded = WorkspaceEdit_from_json(codec, &edit, wire, length, &error);
  else decoded = WorkspaceCommand_from_json(codec, &command, wire, length, &error);
  if (decoded == DATA_BIND_ERR_OOM) goto done;
  if (decoded != DATA_BIND_OK) {
    status = workspace_result(out, 0, "rejected", "invalid tool arguments"); goto done;
  }
  if ((binding->action == WORKSPACE_WRITE ? WorkspaceWrite_tool_normalize_empty(&write) :
       binding->action == WORKSPACE_EDIT ? WorkspaceEdit_tool_normalize_empty(&edit) :
       WorkspaceCommand_tool_normalize_empty(&command)) != DATA_BIND_OK) goto done;
  if (binding->action == WORKSPACE_WRITE) status = workspace_publish(binding, write.path, write.content, context, out);
  else if (binding->action == WORKSPACE_EDIT) status = workspace_edit(binding, &edit, context, out);
  else if (!command.command[0]) status = workspace_result(out, 0, "rejected", "command must not be empty");
  else status = workspace_exec(binding, command.command, context, out);
done:
  WorkspaceWrite_clear(&write); WorkspaceEdit_clear(&edit); WorkspaceCommand_clear(&command);
  data_bind_free(codec);
  json_serialize_free(wire);
  if (status != TURBO_TOOL_OK) { json_free(*out); *out = NULL; }
  return status;
}

static turbo_tool_status_t workspace_invoke_text(const char *arguments,
                                                const turbo_tool_execution_context_t *context,
                                                char **out, void *data) {
  json_value_t *input = NULL, *result = NULL;
  turbo_tool_status_t status;
  *out = NULL;
  if (!arguments) return TURBO_TOOL_INVALID_ARGUMENT;
  input = json_parse(arguments, strlen(arguments));
  if (!input) return TURBO_TOOL_INVALID_ARGUMENT;
  status = workspace_invoke(input, context, &result, data);
  if (status == TURBO_TOOL_OK) {
    *out = json_serialize(result, NULL);
    if (!*out) status = TURBO_TOOL_OUT_OF_MEMORY;
  }
  json_free(result); json_free(input);
  return status;
}

turbo_tool_status_t turbo_coding_tools_add_workspace(turbo_tool_registry_t *registry,
                                                    const turbo_coding_workspace_config_t *config) {
  static const char *names[] = {"fs.write", "fs.edit", "shell.exec"};
  static const char *descriptions[] = {
    "Atomically replace/create a bounded UTF-8 workspace file. Parent directory must exist.",
    "Replace exactly one occurrence of old_text with new_text in a bounded workspace file.",
    "Run a platform shell command in the workspace with host privileges, bounded time and output."};
  static const char *patch[] = {"patch"}, *shell[] = {"shell"};
  json_value_t *(*schemas[])(void) = {WorkspaceWrite_tool_schema, WorkspaceEdit_tool_schema, WorkspaceCommand_tool_schema};
  size_t added = 0;
  turbo_tool_status_t status;
  if (!registry || !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_CODING_WORKSPACE_CONFIG_ABI_VERSION ||
      !config->max_write_bytes || config->max_write_bytes > INT_MAX ||
      !config->max_command_bytes || config->max_command_bytes > INT_MAX ||
      !config->max_process_output_bytes || config->max_process_output_bytes > INT_MAX || !config->process_timeout_ms)
    return TURBO_TOOL_INVALID_ARGUMENT;
  status = turbo_coding_tools_add_read_only(registry, &config->files);
  if (status != TURBO_TOOL_OK) return status;
  for (added = 0; added < WORKSPACE_ACTION_COUNT; ++added) {
    workspace_binding_t *binding = calloc(1, sizeof(*binding));
    turbo_tool_definition_v6_t definition = {0};
    json_value_t *schema = schemas[added]();
    status = TURBO_TOOL_OUT_OF_MEMORY;
    if (!binding || !schema) { workspace_binding_free(binding); json_free(schema); goto rollback; }
    binding->config = *config;
    binding->cwd = tstr_dup(config->files.workspace_root);
    binding->config.files.workspace_root = binding->cwd;
    binding->action = (int)added;
    if (!binding->cwd) { workspace_binding_free(binding); json_free(schema); goto rollback; }
    if (cmeta_fs_root_open(binding->cwd, &binding->root) != 0) {
      workspace_binding_free(binding); json_free(schema); status = TURBO_TOOL_INVALID_ARGUMENT; goto rollback;
    }
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    definition.definition = (turbo_tool_definition_t){names[added], descriptions[added], NULL, schema, 1,
                                                    NULL, NULL, binding, workspace_binding_free};
    definition.execution_policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_EXCLUSIVE, TURBO_TOOL_IDEMPOTENCY_NONE};
    definition.required_capabilities = added == WORKSPACE_EXEC ? shell : patch;
    definition.required_capability_count = 1;
    definition.effect_flags = added == WORKSPACE_EXEC ? TURBO_TOOL_EFFECT_PROCESS | TURBO_TOOL_EFFECT_EXTERNAL_MUTATION : TURBO_TOOL_EFFECT_WRITE;
    definition.context_handler = workspace_invoke_text;
    definition.json_value_context_handler = workspace_invoke;
    status = turbo_tool_registry_add_v6(registry, &definition);
    json_free(schema);
    if (status != TURBO_TOOL_OK) { workspace_binding_free(binding); goto rollback; }
  }
  return TURBO_TOOL_OK;
rollback:
  while (added) turbo_tool_registry_remove(registry, names[--added]);
  turbo_tool_registry_remove(registry, "fs.search");
  turbo_tool_registry_remove(registry, "fs.list");
  turbo_tool_registry_remove(registry, "fs.read");
  return status;
}
