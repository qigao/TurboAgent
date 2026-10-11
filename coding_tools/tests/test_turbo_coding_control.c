#include "tinytest.h"
#include "turbo_coding_tools.h"

#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <stdlib.h>
#include <string.h>

static uint64_t coding_control_clock(void *user) {
  return ++*(uint64_t *)user;
}

spec("read and list execution control") {
  static tstr directory, content, child;
  static cmeta_fs_root_t *root;
  static cmeta_fs_root_file_t *file;
  static turbo_tool_registry_t *registry;
  static turbo_cancel_source_t *source;
  static turbo_cancel_token_t *token;
  static json_value_t *arguments, *result;
  static char *output;
  static uint64_t ticks;
  static size_t extra_files;

  before_each() {
    char temp[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
    cmeta_uuid_t id;
    turbo_coding_tools_config_t config;
    check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
    check_equal(cmeta_uuid_v4_generate(&id), 0);
    check_equal(cmeta_uuid_format(&id, uuid, sizeof(uuid)), 0);
    directory = tstr_format("{}/turbo-control-{}", temp, uuid);
    check_not_null(directory);
    check_equal(cmeta_fs_mkdir(directory, 0700), 0);
    check_equal(cmeta_fs_root_open(directory, &root), 0);
    content = tstr_new_len(NULL, 1024u * 1024u);
    check_not_null(content);
    memset(content, 'a', tstr_len(content));
    check_equal(cmeta_fs_root_file_open(root, "note.txt",
        SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL, 0600, &file), 0);
    check_equal(cmeta_fs_root_file_write(file, content, tstr_len(content)), (int)tstr_len(content));
    check_equal(cmeta_fs_root_file_close(file), 0);
    file = NULL;
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    turbo_coding_tools_config_init(&config);
    config.workspace_root = directory;
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    ticks = 0;
  }

  after_each() {
    if (file) (void)cmeta_fs_root_file_close(file);
    file = NULL;
    json_free(arguments); json_free(result); free(output);
    arguments = result = NULL; output = NULL;
    turbo_cancel_token_release(token); token = NULL;
    turbo_cancel_source_destroy(source); source = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    tstr_free(child); child = NULL;
    if (root) {
      (void)cmeta_fs_root_unlink(root, "note.txt");
      for (size_t i = 0; i < extra_files; ++i) {
        child = tstr_format("entry-{}.txt", i);
        if (child) (void)cmeta_fs_root_unlink(root, child);
        tstr_free(child); child = NULL;
      }
      (void)cmeta_fs_root_close(root); root = NULL;
    }
    extra_files = 0;
    if (directory) check_equal(cmeta_fs_rmdir(directory), 0);
    tstr_free(directory); directory = NULL;
    tstr_free(content); content = NULL;
  }

  const char *tools[] = {"fs.read", "fs.list"};
  const char *requests[] = {"{\"path\":\"note.txt\"}", "{\"path\":\".\"}"};
  const turbo_cancel_reason_t reasons[] = {
      TURBO_CANCEL_USER, TURBO_CANCEL_SHUTDOWN, TURBO_CANCEL_PARENT, TURBO_CANCEL_DEADLINE};
  for (size_t tool = 0; tool < 2; ++tool) {
    for (size_t reason = 0; reason < sizeof(reasons) / sizeof(reasons[0]); ++reason) {
      it("honors cancellation reason %d in both %s entry points", (int)reasons[reason], tools[tool]) {
        turbo_tool_execution_context_t context = {0};
        turbo_tool_status_t expected = reasons[reason] == TURBO_CANCEL_DEADLINE
            ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED;
        check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
        check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
        check_equal(turbo_cancel_source_cancel(source, reasons[reason]), SALTS_OK);
        context.struct_size = sizeof(context);
        context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
        context.cancel_token = token;
        check_equal(turbo_tool_registry_execute_with_context(registry, tools[tool], requests[tool],
                                                              &context, &output), expected);
        check_null(output);
        arguments = json_parse(requests[tool], strlen(requests[tool]));
        check_not_null(arguments);
        check_equal(turbo_tool_registry_execute_json_value_with_context(
                        registry, tools[tool], arguments, &context, &result), expected);
        check_null(result);
      }
    }

    for (int legacy = 0; legacy < 2; ++legacy) {
      it("honors a token-free deadline for %s with context ABI %d", tools[tool], legacy ? 1 : 2) {
        turbo_tool_execution_context_t context = {0};
        context.struct_size = legacy ? TURBO_TOOL_EXECUTION_CONTEXT_V1_SIZE : sizeof(context);
        context.abi_version = legacy ? TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION_V1
                                    : TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
        context.deadline_mono_ms = 1;
        check_equal(turbo_tool_registry_execute_with_context(registry, tools[tool], requests[tool],
                                                              &context, &output),
                    TURBO_TOOL_DEADLINE_EXCEEDED);
        check_null(output);
        arguments = json_parse(requests[tool], strlen(requests[tool]));
        check_not_null(arguments);
        check_equal(turbo_tool_registry_execute_json_value_with_context(
                        registry, tools[tool], arguments, &context, &result),
                    TURBO_TOOL_DEADLINE_EXCEEDED);
        check_null(result);
      }
    }

    it("observes deadline progress within %s and leaves the registry reusable", tools[tool]) {
      turbo_cancel_source_config_t clock = {0};
      turbo_tool_execution_context_t context = {0};
      turbo_tool_definition_t definition;
      if (tool == 1) {
        for (size_t i = 0; i < 96; ++i) {
          child = tstr_format("entry-{}.txt", i);
          check_not_null(child);
          check_equal(cmeta_fs_root_file_open(root, child,
              SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL, 0600, &file), 0);
          ++extra_files;
          check_equal(cmeta_fs_root_file_close(file), 0); file = NULL;
          tstr_free(child); child = NULL;
        }
      }
      /* The public cancellation clock advances deterministically. The deadline
       * leaves room for admission but expires during multi-chunk/entry work. */
      clock.struct_size = sizeof(clock);
      clock.abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION;
      clock.deadline_mono_ms = 64;
      clock.monotonic_ms = coding_control_clock;
      clock.clock_user_data = &ticks;
      check_equal(turbo_cancel_source_create(&clock, &source), SALTS_OK);
      check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
      context.struct_size = sizeof(context);
      context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
      context.cancel_token = token;
      arguments = json_parse(requests[tool], strlen(requests[tool]));
      check_not_null(arguments);
      check_equal(turbo_tool_registry_execute_json_value_with_context(
                      registry, tools[tool], arguments, &context, &result),
                  TURBO_TOOL_DEADLINE_EXCEEDED);
      check_null(result);
      check_greater_equal(ticks, clock.deadline_mono_ms);
      check_equal(turbo_tool_registry_get_definition(registry, tool, &definition), TURBO_TOOL_OK);
      check_not_null(definition.handler);
      check_not_null(definition.json_value_handler);
      check_equal(definition.json_value_handler(arguments, &result, definition.user_data), 0);
      check_true(json_get_bool(result, "ok", false));
      if (tool == 0) check_equal(json_get_string(result, "content"), content);
      else check_equal(json_array_size(json_object_get(result, "entries")), 97);
    }
  }
}
