#include "tinytest.h"
#include "turbo_coding_tools.h"
#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <tstr.h>
#include <stdlib.h>
#include <string.h>

static tstr workspace_create(void) {
  char temp[SALTS_FS_MAX_PATH], path[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
  cmeta_uuid_t id;
  if (cmeta_fs_get_tmpdir(temp, sizeof(temp)) != 0 || cmeta_uuid_v4_generate(&id) != 0 ||
      cmeta_uuid_format(&id, uuid, sizeof(uuid)) != 0 ||
      cmeta_fs_path_join(path, sizeof(path), temp, uuid) != 0 || cmeta_fs_mkdir(path, 0700) != 0) return NULL;
  return tstr_dup(path);
}

static json_value_t *invoke(turbo_tool_registry_t *registry, const char *tool, const char *arguments) {
  char *text = NULL;
  json_value_t *result;
  check_equal(turbo_tool_registry_execute(registry, tool, arguments, &text), TURBO_TOOL_OK);
  check_not_null(text);
  result = text ? json_parse(text, strlen(text)) : NULL;
  free(text);
  return result;
}

static void remove_file(const char *root, const char *relative) {
  char path[SALTS_FS_MAX_PATH];
  check_equal(cmeta_fs_path_join(path, sizeof(path), root, relative), 0);
  check_equal(cmeta_fs_unlink(path), 0);
}

static json_value_t *command_arguments(const char *mode) {
  tstr command = tstr_dup("\"");
  json_value_t *arguments = json_create_object();
  command = tstr_cat(command, CODING_PROCESS_FIXTURE);
  command = tstr_cat(command, "\" ");
  command = tstr_cat(command, mode);
  check_equal(turbo_runtime_json_object_set_string(arguments, "command", command), TURBO_RUNTIME_JSON_OK);
  tstr_free(command);
  return arguments;
}

static int existing_tool(const char *arguments, char **out, void *data) {
  (void)arguments; (void)data;
  *out = malloc(3);
  if (!*out) return -1;
  memcpy(*out, "{}", 3);
  return 0;
}

static void existing_tool_free(void *data) { ++*(int *)data; }

static void cancel_later(void *data) {
  cmeta_sleep_ms(150);
  turbo_cancel_source_cancel(data, TURBO_CANCEL_USER);
}

static void capture_cancel_detail(const json_value_t *detail, void *data) {
  *(json_value_t **)data = json_clone(detail);
}

spec("workspace coding tools") {
  it("should write and edit exactly once while preserving the file on conflicts") {
    tstr root = workspace_create();
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_execution_policy_t policy;
    json_value_t *result;
    check_not_null(root);
    turbo_coding_workspace_config_init(&config);
    config.files.workspace_root = root;
    config.max_write_bytes = 64;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_count(registry), 6);
    check_equal(turbo_tool_registry_get_execution_policy(registry, "fs.edit", &policy), TURBO_TOOL_OK);
    check_equal(policy.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_equal(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    result = invoke(registry, "fs.write", "{\"path\":\"note.txt\",\"content\":\"hello world\"}");
    check_true(json_get_bool(result, "ok", 0)); json_free(result);
    result = invoke(registry, "fs.edit", "{\"path\":\"note.txt\",\"old_text\":\"world\",\"new_text\":\"agent\"}");
    check_true(json_get_bool(result, "ok", 0)); json_free(result);
    result = invoke(registry, "fs.edit", "{\"path\":\"note.txt\",\"old_text\":\"l\",\"new_text\":\"x\"}");
    check_equal(strcmp(json_get_string(result, "status"), "conflict"), 0); json_free(result);
    result = invoke(registry, "fs.read", "{\"path\":\"note.txt\"}");
    check_equal(strcmp(json_get_string(result, "content"), "hello agent"), 0); json_free(result);
    result = invoke(registry, "fs.list", "{\"path\":\".\"}");
    check_true(json_get_bool(result, "ok", 0));
    check_equal(json_array_size(json_object_get(result, "entries")), 1); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"../escape.txt\",\"content\":\"no\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"note.txt\",\"wrong\":\"x\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"absent/new.txt\",\"content\":\"no\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.edit", "{\"path\":\"note.txt\",\"old_text\":\"\",\"new_text\":\"x\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.edit", "{\"path\":\"note.txt\",\"old_text\":\"hello agent\",\"new_text\":\"\"}");
    check_true(json_get_bool(result, "ok", 0)); json_free(result);
    result = invoke(registry, "fs.read", "{\"path\":\"note.txt\"}");
    check_equal(strcmp(json_get_string(result, "content"), ""), 0); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"note.txt\",\"content\":\"\"}");
    check_true(json_get_bool(result, "ok", 0)); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"\",\"content\":\"x\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    turbo_tool_registry_destroy(registry);
    remove_file(root, "note.txt");
    check_equal(cmeta_fs_rmdir(root), 0);
    tstr_free(root);
  }

  it("should reject oversized edits and writes without changing the file") {
    tstr root = workspace_create();
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    json_value_t *result;
    turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
    config.max_write_bytes = 16;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
    result = invoke(registry, "fs.write", "{\"path\":\"note.txt\",\"content\":\"0123456789\"}");
    check_true(json_get_bool(result, "ok", 0)); json_free(result);
    result = invoke(registry, "fs.edit", "{\"path\":\"note.txt\",\"old_text\":\"0\",\"new_text\":\"1234567890123456\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"note.txt\",\"content\":\"12345678901234567\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.read", "{\"path\":\"note.txt\"}");
    check_equal(strcmp(json_get_string(result, "content"), "0123456789"), 0); json_free(result);
    turbo_tool_registry_destroy(registry); remove_file(root, "note.txt");
    check_equal(cmeta_fs_rmdir(root), 0); tstr_free(root);
  }

  it("should reject linked directories for read, list and file replacement") {
    tstr root = workspace_create(), outside = workspace_create();
    char link[SALTS_FS_MAX_PATH], secret[SALTS_FS_MAX_PATH];
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    cmeta_fs_buf_t contents = cmeta_fs_buf_init("outside", 7), verify = {0};
    json_value_t *result;
    check_equal(cmeta_fs_path_join(link, sizeof(link), root, "link"), 0);
    check_equal(cmeta_fs_path_join(secret, sizeof(secret), outside, "secret.txt"), 0);
    check_equal(cmeta_fs_write_file(secret, &contents), 0);
    turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
#ifdef _WIN32
    {
      /* Junction creation exercises reparse rejection without requiring the
       * optional Windows symbolic-link privilege on developer/CI machines. */
      tstr command = tstr_dup("mklink /J link \"");
      json_value_t *arguments = json_create_object();
      command = tstr_cat(command, outside);
      command = tstr_cat(command, "\"");
      check_equal(turbo_runtime_json_object_set_string(arguments, "command", command), TURBO_RUNTIME_JSON_OK);
      check_equal(turbo_tool_registry_execute_json_value(registry, "shell.exec", arguments, &result), TURBO_TOOL_OK);
      check_true(json_get_bool(result, "ok", 0));
      json_free(result); json_free(arguments); tstr_free(command);
    }
#else
    check_equal(cmeta_fs_symlink(outside, link, 1), 0);
#endif
    result = invoke(registry, "fs.read", "{\"path\":\"link/secret.txt\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.list", "{\"path\":\"link\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.search", "{\"path\":\"link/secret.txt\",\"pattern\":\"outside\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    result = invoke(registry, "fs.search", "{\"path\":\".\",\"pattern\":\"outside\"}");
    check_true(json_get_bool(result, "ok", 0));
    check_equal(json_array_size(json_object_get(result, "matches")), 0);
    check_equal(json_get_int(json_object_get(result, "metrics"), "skipped_special", 0), 1);
    json_free(result);
    result = invoke(registry, "fs.write", "{\"path\":\"link/secret.txt\",\"content\":\"changed\"}");
    check_false(json_get_bool(result, "ok", 1)); json_free(result);
    check_equal(cmeta_fs_read_file(secret, &verify), 0);
    check_equal(verify.len, 7); check_equal(memcmp(verify.base, "outside", 7), 0);
    cmeta_fs_buf_free(&verify);
    turbo_tool_registry_destroy(registry);
#ifdef _WIN32
    check_equal(cmeta_fs_rmdir(link), 0);
#else
    check_equal(cmeta_fs_unlink(link), 0);
#endif
    check_equal(cmeta_fs_unlink(secret), 0);
    check_equal(cmeta_fs_rmdir(root), 0); check_equal(cmeta_fs_rmdir(outside), 0);
    tstr_free(root); tstr_free(outside);
  }

  it("should reject cancellation before publishing and roll back duplicate registration") {
    tstr root = workspace_create();
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    char *output = NULL;
    turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_DUPLICATE);
    check_equal(turbo_tool_registry_count(registry), 6);
    check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
    context.struct_size = sizeof(context); context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;
    check_equal(turbo_tool_registry_execute_with_context(registry, "fs.write",
                  "{\"path\":\"cancelled.txt\",\"content\":\"no\"}", &context, &output), TURBO_TOOL_CANCELLED);
    check_null(output);
    turbo_cancel_token_release(token); turbo_cancel_source_destroy(source);
    turbo_tool_registry_destroy(registry);
    check_equal(cmeta_fs_rmdir(root), 0);
    tstr_free(root);
  }

  it("should remove only newly added tools when registration fails partway through") {
    tstr root = workspace_create();
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    int freed = 0;
    turbo_tool_definition_t original = {"fs.edit", "existing tool", "{}", NULL, 0,
                                         existing_tool, NULL, &freed, existing_tool_free};
    char *output = NULL;
    turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
    check_equal(turbo_tool_registry_add(registry, &original), TURBO_TOOL_OK);
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_DUPLICATE);
    check_equal(turbo_tool_registry_count(registry), 1); check_equal(freed, 0);
    check_equal(turbo_tool_registry_execute(registry, "fs.edit", "{}", &output), TURBO_TOOL_OK);
    check_equal(strcmp(output, "{}"), 0); free(output);
    turbo_tool_registry_destroy(registry); check_equal(freed, 1);
    check_equal(cmeta_fs_rmdir(root), 0); tstr_free(root);
  }

  for (int legacy = 0; legacy < 2; ++legacy) {
    it("should cancel and reap with %s context", legacy ? "legacy" : "full-detail") {
      tstr root = workspace_create();
      turbo_coding_workspace_config_t config;
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      cmeta_thread_t thread;
      turbo_tool_execution_context_t context = {0};
      json_value_t *arguments = command_arguments("wait"), *result = NULL, *detail = NULL;
      uint64_t started;
      turbo_coding_workspace_config_init(&config);
      config.files.workspace_root = root;
      check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
      check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
      check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
      context.struct_size = sizeof(context);
      context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
      if (legacy) {
        context.struct_size = TURBO_TOOL_EXECUTION_CONTEXT_V1_SIZE;
        context.abi_version = 1;
      }
      /* Populated tail fields must not be read when the declared ABI omits them. */
      context.detail_sink = capture_cancel_detail;
      context.detail_sink_user_data = &detail;
      context.cancel_token = token;
      check_equal(cmeta_thread_create(&thread, cancel_later, source), SALTS_OK);
      started = cmeta_monotonic_ms();
      check_equal(turbo_tool_registry_execute_json_value_with_context(registry, "shell.exec",
                                                                      arguments, &context, &result),
                  TURBO_TOOL_CANCELLED);
      check_true(cmeta_monotonic_ms() - started < 5000);
      check_null(result);
      check_equal(cmeta_thread_join(&thread), SALTS_OK);
      if (legacy) check_null(detail);
      else {
        check_not_null(detail);
        check_equal(json_get_string(detail, "status"), "cancelled");
        check_false(json_get_bool(detail, "ok", true));
      }
      json_free(detail);
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      json_free(arguments);
      turbo_tool_registry_destroy(registry);
      check_equal(cmeta_fs_rmdir(root), 0);
      tstr_free(root);
    }
  }

  it("should capture both streams and report a nonzero process exit") {
    tstr root = workspace_create();
    turbo_coding_workspace_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    json_value_t *arguments = command_arguments("output"), *result = NULL;
    turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_execute_json_value(registry, "shell.exec", arguments, &result), TURBO_TOOL_OK);
    info("process status=%s summary=%s stderr=%s", json_get_string(result, "status"), json_get_string(result, "summary"), json_get_string(result, "stderr"));
    check_false(json_get_bool(result, "ok", 1));
    check_equal(json_get_int(result, "exit_code", 0), 7);
    check_not_null(strstr(json_get_string(result, "stdout"), "fixture stdout"));
    check_not_null(strstr(json_get_string(result, "stderr"), "fixture stderr"));
    json_free(result); json_free(arguments);
    arguments = command_arguments("binary"); result = NULL;
    check_equal(turbo_tool_registry_execute_json_value(registry, "shell.exec", arguments, &result), TURBO_TOOL_OK);
    check_equal(strcmp(json_get_string(result, "status"), "binary_output"), 0);
    check_equal(json_get_int(result, "exit_code", 0), 3);
    check_equal(json_get_int(json_object_get(result, "metrics"), "output_bytes", 0), 1);
    json_free(result); json_free(arguments); turbo_tool_registry_destroy(registry);
    check_equal(cmeta_fs_rmdir(root), 0); tstr_free(root);
  }

  it("should stop processes at the output cap, configured timeout and caller deadline") {
    for (int scenario = 0; scenario < 3; ++scenario) {
      tstr root = workspace_create();
      turbo_coding_workspace_config_t config;
      turbo_tool_registry_t *registry = turbo_tool_registry_create();
      json_value_t *arguments = command_arguments(scenario == 0 ? "flood" : "wait"), *result = NULL;
      turbo_tool_execution_context_t context = {0};
      uint64_t started;
      turbo_coding_workspace_config_init(&config); config.files.workspace_root = root;
      config.max_process_output_bytes = 64;
      config.process_timeout_ms = scenario == 1 ? 100 : 10000;
      check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
      context.struct_size = sizeof(context); context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
      if (scenario == 2) context.deadline_mono_ms = cmeta_monotonic_ms() + 100;
      started = cmeta_monotonic_ms();
      check_equal(turbo_tool_registry_execute_json_value_with_context(registry, "shell.exec", arguments, &context, &result),
                  scenario == 2 ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_OK);
      check_true(cmeta_monotonic_ms() - started < 5000);
      if (scenario == 0) {
        check_true(json_get_bool(result, "truncated", 0));
        check_true(strlen(json_get_string(result, "stdout")) + strlen(json_get_string(result, "stderr")) <= 64);
      } else if (scenario == 1) {
        check_false(json_get_bool(result, "ok", 1));
        check_equal(strcmp(json_get_string(result, "status"), "timed_out"), 0);
      } else check_null(result);
      json_free(result); json_free(arguments); turbo_tool_registry_destroy(registry);
      check_equal(cmeta_fs_rmdir(root), 0); tstr_free(root);
    }
  }
}
