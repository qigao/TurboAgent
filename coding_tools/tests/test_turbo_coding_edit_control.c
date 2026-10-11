#include "tinytest.h"
#include "turbo_coding_tools.h"

#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <stdlib.h>
#include <string.h>

static uint64_t edit_control_clock(void *user) {
  return ++*(uint64_t *)user;
}

spec("workspace mutation execution control") {
  static tstr directory, path, content;
  static cmeta_fs_root_t *root;
  static cmeta_fs_root_file_t *file;
  static cmeta_fs_buf_t actual;
  static turbo_tool_registry_t *registry;
  static turbo_cancel_source_t *source;
  static turbo_cancel_token_t *token;
  static json_value_t *arguments, *result;
  static char *wire, *output;
  static uint64_t ticks;

  before_each() {
    char temp[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
    cmeta_uuid_t id;
    turbo_coding_workspace_config_t config;
    check_equal(cmeta_fs_get_tmpdir(temp, sizeof(temp)), 0);
    check_equal(cmeta_uuid_v4_generate(&id), 0);
    check_equal(cmeta_uuid_format(&id, uuid, sizeof(uuid)), 0);
    directory = tstr_format("{}/turbo-edit-control-{}", temp, uuid);
    check_not_null(directory);
    check_equal(cmeta_fs_mkdir(directory, 0700), 0);
    path = tstr_format("{}/note.txt", directory);
    check_not_null(path);
    check_equal(cmeta_fs_root_open(directory, &root), 0);
    content = tstr_new_len(NULL, 1024u * 1024u);
    check_not_null(content);
    memset(content, 'a', tstr_len(content));
    content[tstr_len(content) - 1] = '!';
    check_equal(cmeta_fs_root_file_open(root, "note.txt",
        SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL, 0600, &file), 0);
    check_equal(cmeta_fs_root_file_write(file, content, tstr_len(content)), (int)tstr_len(content));
    check_equal(cmeta_fs_root_file_close(file), 0); file = NULL;
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    turbo_coding_workspace_config_init(&config);
    config.files.workspace_root = directory;
    check_equal(turbo_coding_tools_add_workspace(registry, &config), TURBO_TOOL_OK);
    ticks = 0;
  }

  after_each() {
    if (file) (void)cmeta_fs_root_file_close(file);
    file = NULL;
    cmeta_fs_buf_free(&actual); memset(&actual, 0, sizeof(actual));
    json_free(arguments); json_free(result);
    arguments = result = NULL;
    json_serialize_free(wire); wire = NULL;
    free(output); output = NULL;
    turbo_cancel_token_release(token); token = NULL;
    turbo_cancel_source_destroy(source); source = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    if (root) {
      (void)cmeta_fs_root_unlink(root, "note.txt");
      (void)cmeta_fs_root_close(root); root = NULL;
    }
    if (directory) check_equal(cmeta_fs_rmdir(directory), 0);
    tstr_free(directory); directory = NULL;
    tstr_free(path); path = NULL;
    tstr_free(content); content = NULL;
  }

  const struct {
    const char *name, *tool, *old_text;
    uint64_t deadline;
  } scenarios[] = {
      {"edit read with no match", "fs.edit", "missing", 64},
      {"edit read with ambiguous matches", "fs.edit", "aa", 64},
      {"edit read with a unique match", "fs.edit", "!", 64},
      {"edit staging writes", "fs.edit", "!", 320},
      {"write staging writes", "fs.write", NULL, 64}};
  for (size_t scenario = 0; scenario < sizeof(scenarios) / sizeof(scenarios[0]); ++scenario) {
    for (int text = 0; text < 2; ++text) {
      it("preserves the target and cleans up after a deadline during %s via %s",
         scenarios[scenario].name, text ? "text" : "DOM") {
        turbo_cancel_source_config_t clock = {0};
        turbo_tool_execution_context_t context = {0};
        arguments = json_create_object();
        check_not_null(arguments);
        check_equal(turbo_runtime_json_object_set_string(arguments, "path", "note.txt"), 0);
        if (scenarios[scenario].old_text) {
          check_equal(turbo_runtime_json_object_set_string(arguments, "old_text",
                                                          scenarios[scenario].old_text), 0);
          check_equal(turbo_runtime_json_object_set_string(arguments, "new_text", "?"), 0);
        } else {
          check_equal(turbo_runtime_json_object_set_string(arguments, "content", content + 1), 0);
        }
        /* The clock leaves admission time, then expires inside repeated I/O.
         * 320 ticks allow the 1 MiB original to be read before staged writes. */
        clock.struct_size = sizeof(clock);
        clock.abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION;
        clock.deadline_mono_ms = scenarios[scenario].deadline;
        clock.monotonic_ms = edit_control_clock;
        clock.clock_user_data = &ticks;
        check_equal(turbo_cancel_source_create(&clock, &source), SALTS_OK);
        check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
        context.struct_size = sizeof(context);
        context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
        context.cancel_token = token;
        if (text) {
          wire = json_serialize(arguments, NULL);
          check_not_null(wire);
          check_equal(turbo_tool_registry_execute_with_context(registry, scenarios[scenario].tool,
                                                                wire, &context, &output),
                      TURBO_TOOL_DEADLINE_EXCEEDED);
          check_null(output);
        } else {
          check_equal(turbo_tool_registry_execute_json_value_with_context(
                          registry, scenarios[scenario].tool, arguments, &context, &result),
                      TURBO_TOOL_DEADLINE_EXCEEDED);
          check_null(result);
        }
        check_greater_equal(ticks, clock.deadline_mono_ms);
        check_equal(cmeta_fs_read_file(path, &actual), 0);
        check_equal(actual.len, tstr_len(content));
        check_equal(actual.base, content, actual.len);
        cmeta_fs_buf_free(&actual); memset(&actual, 0, sizeof(actual));
        check_equal(turbo_tool_registry_execute(registry, "fs.list", "{\"path\":\".\"}", &output),
                    TURBO_TOOL_OK);
        result = json_parse(output, strlen(output));
        check_not_null(result);
        check_equal(json_array_size(json_object_get(result, "entries")), 1);
        check_equal(json_get_string(json_array_get(json_object_get(result, "entries"), 0), "name"),
                    "note.txt");
        json_free(result); result = NULL;
        free(output); output = NULL;

        /* Explicit subsequent calls use the same registry with a fresh context. */
        check_equal(turbo_tool_registry_execute(registry, "fs.edit",
            "{\"path\":\"note.txt\",\"old_text\":\"!\",\"new_text\":\"?\"}", &output), TURBO_TOOL_OK);
        result = json_parse(output, strlen(output));
        check_not_null(result);
        check_true(json_get_bool(result, "ok", false));
        check_equal(cmeta_fs_read_file(path, &actual), 0);
        check_equal(actual.len, tstr_len(content));
        content[tstr_len(content) - 1] = '?';
        check_equal(actual.base, content, actual.len);
      }
    }
  }
}
