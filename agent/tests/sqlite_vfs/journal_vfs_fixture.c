#include "../../src/turbo_agent_tool_executor_internal.h"
#include "../../src/turbo_agent_tool_journal_internal.h"
#include "production_sqlite_store.h"
#include "sqlite_fault_vfs.h"
#include <fmt.h>
#include <tstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fault_case_s { const char *name; int kind, operation, error, mode; } fault_case_t;
static const fault_case_t cases[] = {
  {"none", -1, -1, 0, VFS_FAIL_BEFORE},
  {"journal_full", VFS_JOURNAL, VFS_WRITE, SQLITE_FULL, VFS_FAIL_BEFORE},
  {"journal_write", VFS_JOURNAL, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_FAIL_BEFORE},
  {"journal_sync", VFS_JOURNAL, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_FAIL_BEFORE},
  {"database_write", VFS_MAIN, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_FAIL_BEFORE},
  {"database_sync", VFS_MAIN, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_FAIL_BEFORE},
  {"journal_partial", VFS_JOURNAL, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_PARTIAL_ERROR},
  {"database_partial", VFS_MAIN, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_PARTIAL_ERROR},
  {"journal_write_after", VFS_JOURNAL, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_FAIL_AFTER},
  {"database_write_after", VFS_MAIN, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_FAIL_AFTER},
  {"journal_sync_after", VFS_JOURNAL, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_FAIL_AFTER},
  {"database_sync_after", VFS_MAIN, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_FAIL_AFTER},
  {"crash_journal_sync_before", VFS_JOURNAL, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_EXIT_BEFORE},
  {"crash_journal_sync_after", VFS_JOURNAL, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_EXIT_AFTER},
  {"crash_database_sync_before", VFS_MAIN, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_EXIT_BEFORE},
  {"crash_database_sync_after", VFS_MAIN, VFS_SYNC, SQLITE_IOERR_FSYNC, VFS_EXIT_AFTER},
  {"crash_journal_partial", VFS_JOURNAL, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_PARTIAL_EXIT},
  {"crash_database_partial", VFS_MAIN, VFS_WRITE, SQLITE_IOERR_WRITE, VFS_PARTIAL_EXIT}
};
static const char result[] = "{\"effect_count\":1}";

static int failure(const char *expression, int line) {
  fprintf(stderr, "SQLite VFS fixture line %d: %s\n", line, expression);
  return 2;
}
#define REQUIRE(x) do { if (!(x)) return failure(#x, __LINE__); } while (0)

static int text_is(const json_value_t *record, const char *key, const char *expected) {
  const json_value_t *value = json_object_get(record, key);
  return json_type(value) == JSON_STRING && json_string_len(value) == strlen(expected) &&
      !memcmp(json_string(value), expected, strlen(expected));
}

typedef struct fixture_s {
  const fault_case_t *cut;
  const char *phase;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime;
  turbo_agent_tool_executor_t *executor;
  turbo_tool_registry_t *registry;
  sqlite3 *oracle;
  int inject, phase_attempts, phase_status, calls, subject_calls, puts;
} fixture_t;

static int oracle_count(sqlite3 *db, const char *id, int *out) {
  sqlite3_stmt *stmt = NULL;
  REQUIRE(sqlite3_prepare_v2(db, "SELECT count FROM effects WHERE id=?1", -1, &stmt, NULL) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  REQUIRE(sqlite3_column_type(stmt, 0) == SQLITE_INTEGER);
  *out = sqlite3_column_int(stmt, 0);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
  return 0;
}

static int phase_put(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  json_value_t *record = json_parse(text, strlen(text));
  int selected = f->inject && !strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION) &&
      text_is(record, "call_id", "subject") && text_is(record, "phase", f->phase);
  int rc;
  json_free(record);
  ++f->puts;
  if (selected) {
    f->inject = 0;
    ++f->phase_attempts;
    printf("ARM phase=%s fault=%s\n", f->phase, f->cut->name);
    if (f->cut->error)
      sqlite_fault_vfs_arm(f->cut->kind, f->cut->operation, f->cut->error, f->cut->mode);
  }
  /* Never fabricate a Store error. Only the real platform file operation can
   * fail, and this unchanged production Store performs SQLite's rollback. */
  rc = f->inner.put(f->inner.user_data, collection, id, text);
  if (selected) {
    f->phase_status = rc;
    sqlite_fault_vfs_disarm();
  }
  return rc;
}
static int phase_get(void *user, const char *collection, const char *id, char **out) {
  fixture_t *f = user;
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int phase_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  fixture_t *f = user;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static int phase_create(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  return f->inner.create_if_absent(f->inner.user_data, collection, id, text);
}
static void phase_free(void *user) {
  fixture_t *f = user;
  if (f->inner.user_data_free) f->inner.user_data_free(f->inner.user_data);
  f->inner = (turbo_agent_runtime_store_t){0};
}

static turbo_tool_status_t effect(const char *arguments, const turbo_tool_execution_context_t *context,
                                  char **out, void *user) {
  fixture_t *f = user;
  sqlite3_stmt *stmt = NULL;
  int count;
  ++f->calls;
  *out = NULL;
  if (!context || !context->thread_id || strcmp(context->thread_id, "vfs-thread") ||
      !context->run_id || strcmp(context->run_id, "vfs-run") || !context->turn_id ||
      strcmp(context->turn_id, "turn") || !context->tool_call_id ||
      (strcmp(context->tool_call_id, "subject") && strcmp(context->tool_call_id, "healthy")) ||
      strcmp(arguments, "{}")) return TURBO_TOOL_ERROR;
  if (!strcmp(context->tool_call_id, "subject")) ++f->subject_calls;
  if (sqlite3_prepare_v2(f->oracle, "UPDATE effects SET count=count+1 WHERE id=?1", -1,
                        &stmt, NULL) != SQLITE_OK) return TURBO_TOOL_ERROR;
  if (sqlite3_bind_text(stmt, 1, context->tool_call_id, -1, SQLITE_STATIC) != SQLITE_OK ||
      sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(f->oracle) != 1) {
    sqlite3_finalize(stmt); return TURBO_TOOL_ERROR;
  }
  if (sqlite3_finalize(stmt) != SQLITE_OK ||
      oracle_count(f->oracle, context->tool_call_id, &count) || count != 1) return TURBO_TOOL_ERROR;
  *out = malloc(sizeof(result));
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, result, sizeof(result));
  return TURBO_TOOL_OK;
}

static int open_fixture(fixture_t *f, const char *directory) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store;
  turbo_tool_definition_v4_t definition = {0};
  turbo_agent_tool_executor_config_t executor_config;
  tstr path = tstr_format("{}/journal.db", directory);
  tstr oracle = tstr_format("{}/external.db", directory);
  REQUIRE(path && oracle);
  vfs_test_sqlite_config_init(&config); config.path = path; config.create_if_missing = 0;
  REQUIRE(vfs_test_sqlite_store_open(&config, &f->inner) == SALTS_OK);
  tstr_free(path);
  store = (turbo_agent_runtime_store_t){phase_put, phase_get, phase_list, f, phase_free, phase_create};
  f->runtime = turbo_agent_runtime_create(&store);
  REQUIRE(f->runtime);
  REQUIRE(sqlite3_open_v2(oracle, &f->oracle, SQLITE_OPEN_READWRITE,
                        sqlite_fault_vfs_native_name()) == SQLITE_OK);
  tstr_free(oracle);
  REQUIRE(sqlite3_exec(f->oracle, "PRAGMA synchronous=FULL", NULL, NULL, NULL) == SQLITE_OK);
  f->registry = turbo_tool_registry_create();
  REQUIRE(f->registry);
  definition.struct_size = sizeof(definition); definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition.name = "probe"; definition.definition.description = "VFS durable effect probe";
  definition.definition.parameters_json = "{\"type\":\"object\",\"additionalProperties\":false}";
  definition.definition.user_data = f;
  definition.context_handler = effect;
  REQUIRE(turbo_tool_registry_add_v4(f->registry, &definition) == TURBO_TOOL_OK);
  turbo_agent_tool_executor_config_init(&executor_config);
  executor_config.max_workers = executor_config.queue_capacity = executor_config.max_batch_calls = 1;
  REQUIRE(turbo_agent_tool_executor_create(&executor_config, &f->executor) == SALTS_OK);
  return 0;
}
static int close_fixture(fixture_t *f) {
  turbo_agent_tool_executor_destroy(f->executor);
  turbo_tool_registry_destroy(f->registry);
  turbo_agent_runtime_destroy(f->runtime);
  if (!f->runtime) phase_free(f);
  return sqlite3_close(f->oracle);
}

static int execute(fixture_t *f, const char *id, int unknown, int replayed) {
  turbo_agent_tool_execution_t call = {0};
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  call.call_id = id; call.turn_key = "turn"; call.tool_name = "probe"; call.arguments_json = "{}";
  call.policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE};
  policy.allow_custom_tools = 1;
  REQUIRE(turbo_agent_tool_executor_execute(f->executor, f->runtime, NULL, "vfs-thread", "vfs-run",
          NULL, NULL, f->registry, &policy, &call, 1) == SALTS_OK);
  REQUIRE(call.status == (unknown ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK));
  REQUIRE(call.replayed == replayed);
  if (unknown) REQUIRE(!call.output);
  else REQUIRE(call.output && !strcmp(call.output, result));
  free(call.output); json_free(call.detail);
  return 0;
}

static int initialize(const char *directory) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store;
  sqlite3 *oracle = NULL;
  tstr path = tstr_format("{}/journal.db", directory);
  tstr oracle_path = tstr_format("{}/external.db", directory);
  REQUIRE(path && oracle_path);
  vfs_test_sqlite_config_init(&config); config.path = path;
  REQUIRE(vfs_test_sqlite_store_open(&config, &store) == SALTS_OK);
  REQUIRE(store.put(store.user_data, "vfs_control", "baseline", "{\"keep\":\"unchanged\"}") == SALTS_OK);
  store.user_data_free(store.user_data);
  REQUIRE(sqlite3_open(oracle_path, &oracle) == SQLITE_OK);
  REQUIRE(sqlite3_exec(oracle, "PRAGMA synchronous=FULL; CREATE TABLE effects(id TEXT PRIMARY KEY,count INTEGER);"
          "INSERT INTO effects VALUES('subject',0),('healthy',0)", NULL, NULL, NULL) == SQLITE_OK);
  REQUIRE(sqlite3_close(oracle) == SQLITE_OK);
  tstr_free(path); tstr_free(oracle_path);
  return 0;
}

static int exercise(const char *directory, const char *phase, const fault_case_t *cut,
                     int inject, int healthy_replayed) {
  fixture_t f = {0};
  sqlite_fault_stats_t stats;
  tstr path = tstr_format("{}/journal.db", directory);
  int subject_count = cut->error && !strcmp(phase, "started") ? 0 : 1;
  int count;
  REQUIRE(path);
  REQUIRE(sqlite_fault_vfs_install(path) == SQLITE_OK);
  tstr_free(path);
  f.cut = cut; f.phase = phase; f.inject = inject;
  REQUIRE(open_fixture(&f, directory) == 0);
  stats = sqlite_fault_vfs_stats();
  if (!inject && !healthy_replayed && cut->mode >= VFS_EXIT_BEFORE && cut->kind == VFS_MAIN)
    REQUIRE(stats.main_writes > 0 && stats.main_syncs > 0);
  if (!inject && cut->mode >= VFS_EXIT_BEFORE)
    printf("OPEN_RECOVERY phase=%s fault=%s main_writes=%d main_syncs=%d\n",
           phase, cut->name, stats.main_writes, stats.main_syncs);
  REQUIRE(execute(&f, "subject", cut->error != 0,
                  inject ? (cut->error != 0 && !strcmp(phase, "started")) : 1) == 0);
  REQUIRE(!inject || cut->mode < VFS_EXIT_BEFORE); /* A missed crash is a failure. */
  REQUIRE(f.calls == (inject ? subject_count : 0));
  REQUIRE(f.subject_calls == (inject ? subject_count : 0));
  if (inject) {
    REQUIRE(f.phase_attempts == 1);
    REQUIRE(f.phase_status == (cut->error == SQLITE_FULL ? SALTS_ENOSPC :
                              cut->error ? SALTS_EIO : SALTS_OK));
    stats = sqlite_fault_vfs_stats();
    REQUIRE(stats.target_opens > 0);
    REQUIRE(stats.failures == (cut->error ? 1 : 0));
    if (cut->error) {
      REQUIRE(stats.error == cut->error);
      REQUIRE(stats.flags & (cut->kind == VFS_MAIN ? SQLITE_OPEN_MAIN_DB : SQLITE_OPEN_MAIN_JOURNAL));
      REQUIRE(stats.mode == cut->mode);
      REQUIRE(stats.native_completed == (cut->mode != VFS_FAIL_BEFORE));
      if (cut->mode == VFS_PARTIAL_ERROR)
        REQUIRE(stats.native_bytes > 0 && stats.native_bytes < stats.bytes && stats.changed_bytes > 0 &&
                stats.readback_bytes == stats.native_bytes);
      if (cut->mode == VFS_FAIL_AFTER && cut->operation == VFS_WRITE)
        REQUIRE(stats.native_bytes == stats.bytes && stats.readback_bytes == stats.bytes);
    }
    /* Same-handle retry never repeats the uncertain identity, while a fresh
     * independent identity can still commit after the one-shot I/O error. */
    REQUIRE(execute(&f, "subject", cut->error != 0, 1) == 0);
    REQUIRE(f.calls == subject_count);
  }
  REQUIRE(execute(&f, "healthy", 0, healthy_replayed) == 0);
  REQUIRE(f.calls == (inject ? subject_count : 0) + (healthy_replayed ? 0 : 1));
  if (!inject) REQUIRE(f.phase_attempts == 0);
  if (!inject && healthy_replayed) REQUIRE(f.puts == 0);
  REQUIRE(oracle_count(f.oracle, "subject", &count) == 0 && count == subject_count);
  REQUIRE(oracle_count(f.oracle, "healthy", &count) == 0 && count == 1);
  stats = sqlite_fault_vfs_stats();
  if (inject && cut->error) REQUIRE(stats.writes_after_fault > 0 && stats.syncs_after_fault > 0);
  printf("VFS stage=%s phase=%s fault=%s native=%s hits=%d error=%d file_flags=%d "
         "offset=%lld bytes=%d later_writes=%d later_syncs=%d callbacks_total=%d "
         "subject_callbacks=%d subject_effects=%d native_completed=%d native_bytes=%d "
         "changed_bytes=%d readback_bytes=%d\n",
         inject ? "inject" : "restart", phase, cut->name, sqlite_fault_vfs_native_name(),
         stats.failures, stats.error, stats.flags, (long long)stats.offset, stats.bytes,
         stats.writes_after_fault, stats.syncs_after_fault, f.calls, f.subject_calls, subject_count,
         stats.native_completed, stats.native_bytes, stats.changed_bytes, stats.readback_bytes);
  REQUIRE(close_fixture(&f) == SQLITE_OK);
  REQUIRE(sqlite_fault_vfs_stats().live_files == 0);
  REQUIRE(sqlite_fault_vfs_remove() == SQLITE_OK);
  return 0;
}

/* A separate native SQLite connection in a fresh process inspects actual files.
 * It does not use Runtime get/list, the Store wrapper or the injected VFS. */
static int inspect(const char *directory, const char *phase, const fault_case_t *cut) {
  sqlite3 *db = NULL, *oracle = NULL;
  sqlite3_stmt *stmt = NULL;
  tstr path = tstr_format("{}/journal.db", directory);
  tstr oracle_path = tstr_format("{}/external.db", directory);
  const char *subject_phase = !cut->error ? "committed" : !strcmp(phase, "started") ? "planned" : "started";
  char ids[2][65] = {{0}}, hashes[2][65] = {{0}};
  int seen[2][2] = {{0}}, baseline = 0, rc, count;
  REQUIRE(path && oracle_path);
  REQUIRE(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, NULL) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(stmt, 0), "ok"));
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
  REQUIRE(sqlite3_prepare_v2(db, "SELECT collection,id,payload FROM runtime_records ORDER BY collection,id",
                            -1, &stmt, NULL) == SQLITE_OK);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *collection = (const char *)sqlite3_column_text(stmt, 0);
    const char *id = (const char *)sqlite3_column_text(stmt, 1);
    const char *text = (const char *)sqlite3_column_text(stmt, 2);
    json_value_t *record;
    int subject, claim;
    REQUIRE(collection && id && text);
    if (!strcmp(collection, "vfs_control")) {
      REQUIRE(!baseline++ && !strcmp(id, "baseline") && !strcmp(text, "{\"keep\":\"unchanged\"}"));
      continue;
    }
    claim = !strcmp(collection, TURBO_AGENT_TOOL_CLAIM_COLLECTION);
    REQUIRE(claim || !strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION));
    record = json_parse(text, (size_t)sqlite3_column_bytes(stmt, 2));
    REQUIRE(record);
    subject = text_is(record, "call_id", "subject");
    REQUIRE(subject || text_is(record, "call_id", "healthy"));
    REQUIRE(!seen[subject][claim]++);
    REQUIRE(strlen(id) == 64 && text_is(record, "journal_id", id));
    REQUIRE(text_is(record, "thread_id", "vfs-thread") && text_is(record, "run_id", "vfs-run"));
    REQUIRE(text_is(record, "turn_key", "turn") && text_is(record, "tool_name", "probe"));
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(record, "schema_version"), -1) == 1);
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(record, "status"), -1) == TURBO_TOOL_OK);
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(record, "execution_mode"), -1) == TURBO_TOOL_EXECUTION_SEQUENTIAL);
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(record, "idempotency"), -1) == TURBO_TOOL_IDEMPOTENCY_NONE);
    REQUIRE(text_is(record, "phase", claim ? "admitted" : subject ? subject_phase : "committed"));
    {
      const char *hash = json_get_string(record, "arguments_hash");
      REQUIRE(hash && strlen(hash) == 64 && json_string_len(json_object_get(record, "arguments_hash")) == 64);
      if (ids[subject][0]) REQUIRE(!strcmp(ids[subject], id) && !strcmp(hashes[subject], hash));
      else { memcpy(ids[subject], id, 65); memcpy(hashes[subject], hash, 65); }
    }
    if (!claim && (!subject || !cut->error)) REQUIRE(text_is(record, "output", result));
    else REQUIRE(!json_object_get(record, "output"));
    json_free(record);
  }
  REQUIRE(rc == SQLITE_DONE && baseline == 1);
  REQUIRE(seen[0][0] == 1 && seen[0][1] == 1 && seen[1][0] == 1 && seen[1][1] == 1);
  REQUIRE(strcmp(ids[0], ids[1]) && !strcmp(hashes[0], hashes[1]));
  REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
  REQUIRE(sqlite3_close(db) == SQLITE_OK);
  REQUIRE(sqlite3_open_v2(oracle_path, &oracle, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
  REQUIRE(oracle_count(oracle, "subject", &count) == 0 && count == (!cut->error || strcmp(phase, "started") ? 1 : 0));
  REQUIRE(oracle_count(oracle, "healthy", &count) == 0 && count == 1);
  REQUIRE(sqlite3_close(oracle) == SQLITE_OK);
  printf("DISK phase=%s fault=%s actual=%s claim=admitted integrity=ok healthy=committed\n",
         phase, cut->name, subject_phase);
  tstr_free(path); tstr_free(oracle_path);
  return 0;
}

int main(int argc, char **argv) {
  const fault_case_t *cut = NULL;
  if (argc != 5 || (strcmp(argv[2], "started") && strcmp(argv[2], "committed"))) return 2;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    if (!strcmp(argv[3], cases[i].name)) cut = &cases[i];
  if (!cut) return 2;
  if (!strcmp(argv[1], "init")) return initialize(argv[4]);
  if (!strcmp(argv[1], "inspect")) return inspect(argv[4], argv[2], cut);
  if (!strcmp(argv[1], "inject") || !strcmp(argv[1], "recover") || !strcmp(argv[1], "replay")) {
    int injecting = !strcmp(argv[1], "inject");
    int first_crash_reopen = !strcmp(argv[1], "recover") && cut->mode >= VFS_EXIT_BEFORE;
    return exercise(argv[4], argv[2], cut, injecting, !injecting && !first_crash_reopen);
  }
  return 2;
}
