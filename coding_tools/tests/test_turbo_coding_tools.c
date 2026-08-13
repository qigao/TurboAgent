#include "tinytest.h"
#include "turbo_coding_tools.h"

#include <turbo_fs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
#endif

static char *coding_test_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static char *coding_test_workspace(void) {
  char temp[TURBO_FS_MAX_PATH];
  char path[TURBO_FS_MAX_PATH];
  if (turbo_fs_get_tmpdir(temp, sizeof(temp)) != 0) return NULL;
#ifdef _WIN32
  snprintf(path, sizeof(path), "%s/turbo_coding_%llu", temp, (unsigned long long)GetTickCount64());
#else
  snprintf(path, sizeof(path), "%s/turbo_coding_%lu", temp, (unsigned long)getpid());
#endif
  if (turbo_fs_mkdir(path, 0700) != 0) return NULL;
  return coding_test_strdup(path);
}

spec("turbo coding tools") {

  it("should register bounded read-only tools and reject workspace escape") {
    char *workspace = coding_test_workspace();
    char file_path[TURBO_FS_MAX_PATH];
    turbo_fs_buf_t write_buffer = turbo_fs_buf_init("hello", 5);
    turbo_coding_tools_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_execution_policy_t policy = {0};
    char *output = NULL;

    check_not_null(workspace);
    check_int_eq(turbo_fs_path_join(file_path, sizeof(file_path), workspace, "note.txt"), 0);
    check_int_eq(turbo_fs_write_file(file_path, &write_buffer), 0);
    turbo_coding_tools_config_init(&config);
    config.workspace_root = workspace;
    config.max_read_bytes = 64;
    config.max_list_entries = 8;
    config.max_result_bytes = 512;
    check_int_eq(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_size_eq(turbo_tool_registry_count(registry), 2);
    check_int_eq(turbo_tool_registry_get_execution_policy(registry, "fs.read", &policy),
                 TURBO_TOOL_OK);
    check_int_eq(policy.mode, TURBO_TOOL_EXECUTION_PARALLEL_SAFE);
    check_int_eq(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
    check_int_eq(
        turbo_tool_registry_execute(registry, "fs.read", "{\"path\":\"note.txt\"}", &output),
        TURBO_TOOL_OK);
    check_not_null(strstr(output, "hello"));
    free(output);
    output = NULL;
    check_int_eq(
        turbo_tool_registry_execute(registry, "fs.list", "{\"path\":\"sub/../\"}", &output),
        TURBO_TOOL_OK);
    check_not_null(strstr(output, "rejected"));

    free(output);
    turbo_tool_registry_destroy(registry);
    turbo_fs_unlink(file_path);
    turbo_fs_rmdir(workspace);
    free(workspace);
  }

  it("should enforce read byte limits") {
    char *workspace = coding_test_workspace();
    char file_path[TURBO_FS_MAX_PATH];
    turbo_fs_buf_t write_buffer = turbo_fs_buf_init("0123456789", 10);
    turbo_coding_tools_config_t config;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    char *output = NULL;

    check_int_eq(turbo_fs_path_join(file_path, sizeof(file_path), workspace, "large.txt"), 0);
    check_int_eq(turbo_fs_write_file(file_path, &write_buffer), 0);
    turbo_coding_tools_config_init(&config);
    config.workspace_root = workspace;
    config.max_read_bytes = 4;
    check_int_eq(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_int_eq(
        turbo_tool_registry_execute(registry, "fs.read", "{\"path\":\"large.txt\"}", &output),
        TURBO_TOOL_OK);
    check_not_null(strstr(output, "configured bounds"));

    free(output);
    turbo_tool_registry_destroy(registry);
    turbo_fs_unlink(file_path);
    turbo_fs_rmdir(workspace);
    free(workspace);
  }
}
