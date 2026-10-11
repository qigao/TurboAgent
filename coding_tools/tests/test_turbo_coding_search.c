#include "tinytest.h"
#include "turbo_coding_tools.h"
#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdlib.h>
#include <string.h>

static tstr search_workspace(void) {
  char temp[SALTS_FS_MAX_PATH], path[SALTS_FS_MAX_PATH], uuid[SALTS_UUID_STRING_SIZE];
  cmeta_uuid_t id;
  if (cmeta_fs_get_tmpdir(temp, sizeof(temp)) != 0 || cmeta_uuid_v4_generate(&id) != 0 ||
      cmeta_uuid_format(&id, uuid, sizeof(uuid)) != 0 ||
      cmeta_fs_path_join(path, sizeof(path), temp, uuid) != 0 || cmeta_fs_mkdir(path, 0700) != 0)
    return NULL;
  return tstr_dup(path);
}

static void search_write(cmeta_fs_root_t *root, const char *path, const char *text, size_t size) {
  cmeta_fs_root_file_t *file = NULL;
  int written, closed;
  check_equal(cmeta_fs_root_file_open(
                  root, path, SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_O_TRUNC, 0600, &file),
              0);
  written = cmeta_fs_root_file_write(file, text, size);
  closed = cmeta_fs_root_file_close(file);
  check_equal(written, (int)size);
  check_equal(closed, 0);
}

static json_value_t *search_args(const char *path, const char *pattern) {
  json_value_t *args = json_create_object();
  if (!args || turbo_runtime_json_object_set_string(args, "path", path) != 0 ||
      turbo_runtime_json_object_set_string(args, "pattern", pattern) != 0) {
    json_free(args);
    return NULL;
  }
  return args;
}

typedef struct search_worker_s {
  turbo_tool_registry_t *registry;
  const char *pattern;
  json_value_t *result;
  turbo_tool_status_t status;
} search_worker_t;

static void search_worker(void *user) {
  search_worker_t *worker = user;
  json_value_t *args = search_args("root.txt", worker->pattern);
  worker->status = TURBO_TOOL_OUT_OF_MEMORY;
  if (args) {
    worker->status = turbo_tool_registry_execute_json_value(worker->registry, "fs.search", args,
                                                            &worker->result);
    json_free(args);
  }
}

static int search_existing(const char *arguments, char **out, void *user) {
  (void)arguments;
  (void)user;
  *out = malloc(3);
  if (!*out) return -1;
  memcpy(*out, "{}", 3);
  return 0;
}

static void search_existing_free(void *user) { ++*(int *)user; }

spec("bounded workspace literal search") {
  static tstr directory, large;
  static cmeta_fs_root_t *root;
  static turbo_tool_registry_t *registry;
  static turbo_coding_tools_config_t config;
  static json_value_t *args, *result;
  static char *wire;
  static turbo_cancel_source_t *source;
  static turbo_cancel_token_t *token;
  static size_t bulk_files;
  static cmeta_thread_t threads[2];
  static search_worker_t workers[2];
  static int existing_freed;

  before_each() {
    directory = search_workspace();
    check_not_null(directory);
    check_equal(cmeta_fs_root_open(directory, &root), 0);
    check_equal(cmeta_fs_root_mkdir(root, "sub", 0700), 0);
    check_equal(cmeta_fs_root_mkdir(root, "sub/deep", 0700), 0);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    turbo_coding_tools_config_init(&config);
    config.workspace_root = directory;
    existing_freed = 0;
  }

  after_each() {
    for (size_t i = 0; i < 2; ++i) {
      if (threads[i]) cmeta_thread_join(&threads[i]);
      threads[i] = NULL;
      json_free(workers[i].result);
      workers[i] = (search_worker_t){0};
    }
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    turbo_cancel_token_release(token);
    token = NULL;
    turbo_cancel_source_destroy(source);
    source = NULL;
    json_free(args);
    json_free(result);
    args = result = NULL;
    free(wire);
    wire = NULL;
    tstr_free(large);
    large = NULL;
    if (root) {
      const char *files[] = {"root.txt", "sub/a.txt", "sub/deep/b.txt", "binary.bin",
                             "invalid.bin"};
      for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        (void)cmeta_fs_root_unlink(root, files[i]);
      for (size_t i = 0; i < bulk_files; ++i) {
        tstr path = tstr_format("bulk{}.txt", i);
        if (path) (void)cmeta_fs_root_unlink(root, path);
        tstr_free(path);
      }
      (void)cmeta_fs_root_rmdir(root, "sub/deep");
      (void)cmeta_fs_root_rmdir(root, "sub");
      cmeta_fs_root_close(root);
      root = NULL;
    }
    bulk_files = 0;
    if (directory) check_equal(cmeta_fs_rmdir(directory), 0);
    tstr_free(directory);
    directory = NULL;
  }

  it("exposes generated required fields and the read-only parallel policy") {
    turbo_tool_definition_t definition;
    turbo_tool_execution_policy_t policy;
    const json_value_t *schema, *properties, *required;
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_get_definition(registry, 2, &definition), TURBO_TOOL_OK);
    check_equal(definition.name, "fs.search");
    schema = definition.parameters_schema;
    properties = json_object_get(schema, "properties");
    required = json_object_get(schema, "required");
    check_equal(json_object_size(properties), 2);
    check_equal(json_get_string(json_object_get(properties, "path"), "type"), "string");
    check_equal(json_get_string(json_object_get(properties, "pattern"), "type"), "string");
    check_equal(json_array_size(required), 2);
    check_equal(json_string(json_array_get(required, 0)), "path");
    check_equal(json_string(json_array_get(required, 1)), "pattern");
    check_false(json_get_bool(schema, "additionalProperties", true));
    check_equal(turbo_tool_registry_get_execution_policy(registry, "fs.search", &policy),
                TURBO_TOOL_OK);
    check_equal(policy.mode, TURBO_TOOL_EXECUTION_PARALLEL_SAFE);
    check_equal(policy.idempotency, TURBO_TOOL_IDEMPOTENCY_READ_ONLY);
  }

  it("keeps an existing search tool when read-only pack registration rolls back") {
    turbo_tool_definition_t definition = {.name = "fs.search",
                                          .description = "existing",
                                          .parameters_json = "{}",
                                          .handler = search_existing,
                                          .user_data = &existing_freed,
                                          .user_data_free = search_existing_free};
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_DUPLICATE);
    check_equal(turbo_tool_registry_count(registry), 1);
    check_equal(existing_freed, 0);
    check_equal(turbo_tool_registry_execute(registry, "fs.search", "{}", &wire), TURBO_TOOL_OK);
    check_equal(wire, "{}");
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    check_equal(existing_freed, 1);
  }

  it("owns independent decoded requests and result trees on two reader threads") {
    search_write(root, "root.txt", "alpha\nbeta\n", 11);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    for (size_t i = 0; i < 2; ++i) {
      workers[i].registry = registry;
      workers[i].pattern = i == 0 ? "alpha" : "beta";
      check_equal(cmeta_thread_create(&threads[i], search_worker, &workers[i]), SALTS_OK);
    }
    for (size_t i = 0; i < 2; ++i) {
      const json_value_t *matches;
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      threads[i] = NULL;
      check_equal(workers[i].status, TURBO_TOOL_OK);
      matches = json_object_get(workers[i].result, "matches");
      check_equal(json_array_size(matches), 1);
      check_equal(json_get_string(json_array_get(matches, 0), "text"), workers[i].pattern);
      check_equal(json_get_int(json_array_get(matches, 0), "line", 0), (int)i + 1);
    }
  }

  it("finds each matching line once across nested files and preserves UTF-8 and line numbers") {
    static const char first[] = "hit hit\r\nMISS\n\xe4\xb8\xad\xe6\x96\x87 hit";
    const json_value_t *matches;
    size_t root_matches = 0, nested_matches = 0;
    search_write(root, "root.txt", first, sizeof(first) - 1);
    search_write(root, "sub/a.txt", "hit\n", 4);
    search_write(root, "sub/deep/b.txt", "HIT\n", 4);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args(".", "hit");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_false(json_get_bool(result, "truncated", true));
    matches = json_object_get(result, "matches");
    check_equal(json_array_size(matches), 3);
    for (size_t i = 0; i < 3; ++i) {
      const json_value_t *match = json_array_get(matches, i);
      if (strcmp(json_get_string(match, "path"), "root.txt") == 0) {
        check_equal(json_get_int(match, "line", 0), root_matches ? 3 : 1);
        check_equal(json_get_string(match, "text"),
                    root_matches ? "\xe4\xb8\xad\xe6\x96\x87 hit" : "hit hit");
        ++root_matches;
      } else {
        check_equal(json_get_string(match, "path"), "sub/a.txt");
        check_equal(json_get_int(match, "line", 0), 1);
        ++nested_matches;
      }
    }
    check_equal(root_matches, 2);
    check_equal(nested_matches, 1);
    check_equal(json_get_int(json_object_get(result, "metrics"), "files_searched", 0), 3);
  }

  it("treats regex metacharacters literally and permits an empty result") {
    search_write(root, "root.txt", "a.*b\naZZb\n", 10);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args("root.txt", "a.*b");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_equal(json_array_size(json_object_get(result, "matches")), 1);
    json_free(args);
    args = search_args("sub", "absent");
    json_free(result);
    result = NULL;
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_false(json_get_bool(result, "truncated", true));
    check_equal(json_array_size(json_object_get(result, "matches")), 0);
  }

  const char *invalid[] = {"{}",
                           "{\"path\":\".\"}",
                           "{\"path\":7,\"pattern\":\"x\"}",
                           "{\"path\":\".\",\"pattern\":null}",
                           "{\"path\":\".\",\"pattern\":\"\"}",
                           "{\"path\":\".\",\"pattern\":\"x\\u0000y\"}",
                           "{\"path\":\".\",\"pattern\":\"x\\ny\"}",
                           "{\"path\":\"../root.txt\",\"pattern\":\"x\"}",
                           "{\"path\":\"sub/../root.txt\",\"pattern\":\"x\"}",
                           "{\"path\":\".\",\"pattern\":\"x\",\"regex\":true}",
                           "{\"path\":\"root.txt:stream\",\"pattern\":\"x\"}"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    it("rejects malformed or disallowed search request %zu", i) {
      check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
      check_equal(turbo_tool_registry_execute(registry, "fs.search", invalid[i], &wire),
                  TURBO_TOOL_OK);
      check_not_null(wire);
      result = json_parse(wire, strlen(wire));
      check_equal(json_get_string(result, "status"), "rejected");
      check_null(json_object_get(result, "matches"));
    }
  }

  it("rejects an oversized pattern before file access") {
    large = tstr_new_len(NULL, 4097);
    check_not_null(large);
    memset(large, 'x', tstr_len(large));
    args = search_args(".", large);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_equal(json_get_string(result, "status"), "rejected");
  }

  it("counts skipped binary and invalid UTF-8 files without returning their bytes") {
    search_write(root, "root.txt", "hit", 3);
    search_write(root, "binary.bin", "hit\0secret", 10);
    search_write(root, "invalid.bin", "hit\xff", 4);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args(".", "hit");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_equal(json_array_size(json_object_get(result, "matches")), 1);
    check_equal(json_get_int(json_object_get(result, "metrics"), "skipped_binary", 0), 2);
    check_false(json_get_bool(result, "truncated", true));
  }

  const char *bounds[] = {"file", "matches", "output", "entries"};
  for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); ++i) {
    it("reports the %s bound without presenting a complete negative search", bounds[i]) {
      search_write(root, "root.txt", "hit\nhit\n", 8);
      if (i == 0) config.max_read_bytes = 4;
      if (i == 1 || i == 3) config.max_list_entries = 1;
      if (i == 2) config.max_result_bytes = 10; /* root.txt + hit needs 11 bytes. */
      check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
      args = search_args(i == 3 ? "." : "root.txt", "hit");
      check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                  TURBO_TOOL_OK);
      check_true(json_get_bool(result, "ok", false));
      check_true(json_get_bool(result, "truncated", false));
      check_less_equal(json_array_size(json_object_get(result, "matches")),
                       i == 1 || i == 3 ? 1 : 0);
      if (i == 0)
        check_equal(json_get_int(json_object_get(result, "metrics"), "skipped_oversized", 0), 1);
    }
  }

  it("accepts exact file and output budgets without losing a final line") {
    search_write(root, "root.txt", "hit", 3);
    config.max_read_bytes = 3;
    config.max_result_bytes = 11;
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args("root.txt", "hit");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_equal(json_array_size(json_object_get(result, "matches")), 1);
    check_false(json_get_bool(result, "truncated", true));
    check_equal(json_get_int(json_object_get(result, "metrics"), "output_bytes", 0), 11);
  }

  it("keeps escaped results within the default agent budget and reports remaining matches") {
    large = tstr_new_len(NULL, 140000);
    check_not_null(large);
    memset(large, 1, tstr_len(large));
    large[0] = large[70001] = 'x';
    large[70000] = '\n';
    search_write(root, "root.txt", large, tstr_len(large));
    config.max_result_bytes = SIZE_MAX;
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_execute(registry, "fs.search",
                                            "{\"path\":\"root.txt\",\"pattern\":\"x\"}", &wire),
                TURBO_TOOL_OK);
    check_not_null(wire);
    check_less(strlen(wire), 1024u * 1024u);
    result = json_parse(wire, strlen(wire));
    check_true(json_get_bool(result, "truncated", false));
    check_equal(json_array_size(json_object_get(result, "matches")), 1);
    check_equal(json_get_int(json_object_get(result, "metrics"), "output_bytes", 0), 70008);
  }

  it("searches the complete file at the exact aggregate read budget") {
    const json_value_t *matches, *match, *metrics;
    large = tstr_new_len(NULL, 16u * 1024u * 1024u);
    check_not_null(large);
    memset(large, 'a', tstr_len(large));
    memcpy(large + tstr_len(large) - 5, "\nhit!", 5);
    search_write(root, "root.txt", large, tstr_len(large));
    config.max_read_bytes = tstr_len(large);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args("root.txt", "hit!");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_false(json_get_bool(result, "truncated", true));
    matches = json_object_get(result, "matches");
    check_equal(json_array_size(matches), 1);
    match = json_array_get(matches, 0);
    check_equal(json_get_string(match, "path"), "root.txt");
    check_equal(json_get_string(match, "text"), "hit!");
    check_equal(json_get_int(match, "line", 0), 2);
    metrics = json_object_get(result, "metrics");
    check_equal(json_get_int(metrics, "bytes_read", 0), 16 * 1024 * 1024);
    check_equal(json_get_int(metrics, "files_searched", 0), 1);
  }

  it("keeps both files when their combined size exactly fills the read budget") {
    const json_value_t *matches, *metrics;
    large = tstr_new_len(NULL, 8u * 1024u * 1024u);
    check_not_null(large);
    memset(large, 'a', tstr_len(large));
    memcpy(large + tstr_len(large) - 5, "\nhit!", 5);
    search_write(root, "sub/a.txt", large, tstr_len(large));
    search_write(root, "sub/deep/b.txt", large, tstr_len(large));
    config.max_read_bytes = tstr_len(large);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args("sub", "hit!");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_false(json_get_bool(result, "truncated", true));
    matches = json_object_get(result, "matches");
    check_equal(json_array_size(matches), 2);
    for (size_t i = 0; i < 2; ++i) {
      check_equal(json_get_string(json_array_get(matches, i), "text"), "hit!");
      check_equal(json_get_int(json_array_get(matches, i), "line", 0), 2);
    }
    check_not_equal(json_get_string(json_array_get(matches, 0), "path"),
                    json_get_string(json_array_get(matches, 1), "path"));
    metrics = json_object_get(result, "metrics");
    check_equal(json_get_int(metrics, "bytes_read", 0), 16 * 1024 * 1024);
    check_equal(json_get_int(metrics, "files_searched", 0), 2);
  }

  it("validates binary contents even when the file fills the aggregate read budget") {
    const json_value_t *metrics;
    large = tstr_new_len(NULL, 16u * 1024u * 1024u);
    check_not_null(large);
    memset(large, 'a', tstr_len(large));
    memcpy(large, "hit\n", 4);
    large[tstr_len(large) - 1] = '\0';
    search_write(root, "root.txt", large, tstr_len(large));
    config.max_read_bytes = tstr_len(large);
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args("root.txt", "hit");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "ok", false));
    check_false(json_get_bool(result, "truncated", true));
    check_equal(json_array_size(json_object_get(result, "matches")), 0);
    metrics = json_object_get(result, "metrics");
    check_equal(json_get_int(metrics, "bytes_read", 0), 16 * 1024 * 1024);
    check_equal(json_get_int(metrics, "files_searched", -1), 0);
    check_equal(json_get_int(metrics, "skipped_binary", 0), 1);
  }

  it("caps aggregate reads even when many individually admissible files contain no match") {
    large = tstr_new_len(NULL, 1024u * 1024u);
    check_not_null(large);
    memset(large, 'a', tstr_len(large));
    for (size_t i = 0; i < 17; ++i) {
      tstr path = tstr_format("bulk{}.txt", i);
      check_not_null(path);
      ++bulk_files;
      search_write(root, path, large, tstr_len(large));
      tstr_free(path);
    }
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    args = search_args(".", "absent");
    check_equal(turbo_tool_registry_execute_json_value(registry, "fs.search", args, &result),
                TURBO_TOOL_OK);
    check_true(json_get_bool(result, "truncated", false));
    check_equal(json_get_int(json_object_get(result, "metrics"), "bytes_read", 0),
                16 * 1024 * 1024);
    check_equal(json_get_int(json_object_get(result, "metrics"), "files_searched", 0), 16);
    check_equal(json_array_size(json_object_get(result, "matches")), 0);
  }

  const turbo_cancel_reason_t reasons[] = {TURBO_CANCEL_USER, TURBO_CANCEL_SHUTDOWN,
                                           TURBO_CANCEL_DEADLINE};
  for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
    it("returns cancellation reason %d without a normal output", (int)reasons[i]) {
      turbo_tool_execution_context_t context = {0};
      check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
      check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
      check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
      check_equal(turbo_cancel_source_cancel(source, reasons[i]), SALTS_OK);
      context.struct_size = sizeof(context);
      context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
      context.cancel_token = token;
      check_equal(turbo_tool_registry_execute_with_context(registry, "fs.search",
                                                           "{\"path\":\".\",\"pattern\":\"hit\"}",
                                                           &context, &wire),
                  reasons[i] == TURBO_CANCEL_DEADLINE ? TURBO_TOOL_DEADLINE_EXCEEDED
                                                      : TURBO_TOOL_CANCELLED);
      check_null(wire);
    }
  }

  it("honors a caller deadline without a cancellation source") {
    turbo_tool_execution_context_t context = {0};
    check_equal(turbo_coding_tools_add_read_only(registry, &config), TURBO_TOOL_OK);
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.deadline_mono_ms = cmeta_monotonic_ms();
    args = search_args(".", "hit");
    check_equal(turbo_tool_registry_execute_json_value_with_context(registry, "fs.search", args,
                                                                    &context, &result),
                TURBO_TOOL_DEADLINE_EXCEEDED);
    check_null(result);
  }
}
