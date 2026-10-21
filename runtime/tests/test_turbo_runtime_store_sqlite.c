#include "tinytest.h"
#include "turbo_runtime_store_sqlite.h"
#include "turbo_runtime_segment.h"
#include "turbo_runtime_history.h"
#include <sqlite3.h>
#include <fmt.h>
#include <tstr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <salts/thread.h>

static void close_store(turbo_agent_runtime_store_t *store) {
  if (store->user_data_free) store->user_data_free(store->user_data);
  *store = (turbo_agent_runtime_store_t){0};
}

static int increment(turbo_graph_exec_ctx_t *ctx, void *data) {
  int64_t count = turbo_runtime_json_value_as_int64(json_object_get(ctx->json_value_state, "count"), 0);
  (void)data;
  return turbo_runtime_json_object_set_int64(ctx->json_value_state, "count", count + 1);
}

spec("checkpoint publication on SQLite") {
  static char *directory, *serialized, *id, *selected_id;
  static tstr path, checkpoint_id;
  static turbo_agent_runtime_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_graph_t *graph;
  static turbo_graph_checkpoint_t *checkpoint;
  static json_value_t *input, *summary, *state, *record;

  after_each() {
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    close_store(&store);
    turbo_graph_destroy(graph); graph = NULL;
    turbo_graph_checkpoint_destroy(checkpoint); checkpoint = NULL;
    json_free(input); json_free(summary); json_free(state); json_free(record);
    input = summary = state = record = NULL;
    json_serialize_free(serialized); serialized = NULL;
    free(id); id = NULL;
    free(selected_id); selected_id = NULL;
    tstr_free(path); tstr_free(checkpoint_id); path = checkpoint_id = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  it("loads an interrupted checkpoint after owner destruction and resumes only the remaining node") {
    const char *pause[] = {"second"};
    turbo_runtime_sqlite_config_t config;
    turbo_runtime_segment_t segment;
    turbo_graph_run_options_t options = {.interrupt_before_nodes = pause, .interrupt_before_count = 1};
    directory = tt_make_temp_dir("turbo-checkpoint");
    check_not_null(directory);
    path = tstr_format("{}/checkpoint.db", directory);
    check_not_null(path);
    turbo_runtime_sqlite_config_init(&config); config.path = path;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    graph = turbo_graph_create("durable-graph");
    check_not_null(graph);
    check_equal(turbo_graph_add_json_value_node(graph, "first", increment, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_json_value_node(graph, "second", increment, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_json_value_edge(graph, "first", "second", NULL, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_set_entry(graph, "first"), TURBO_GRAPH_EXEC_OK);
    input = json_create_object(); check_not_null(input);
    id = turbo_runtime_record_uuid("run"); check_not_null(id);
    turbo_runtime_segment_init(&segment);
    segment.runtime = runtime; segment.graph = graph; segment.thread_id = "thread";
    segment.run_id = id; segment.start_state = input; segment.options = &options;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "interrupted");
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(state, "count"), 0), 1);
    checkpoint_id = tstr_dup(json_get_string(summary, "checkpoint_id"));
    check_not_null(checkpoint_id);
    json_free(summary); json_free(state); summary = state = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store); check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_runtime_get_published_thread_head(runtime, "thread", &record), SALTS_OK);
    check_equal(json_get_string(record, "id"), id);
    check_equal(json_get_string(record, "latest_checkpoint_id"), checkpoint_id);
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_get_latest_checkpoint(runtime, id, &record), 0);
    check_equal(json_get_string(record, "id"), checkpoint_id);
    serialized = json_serialize(json_object_get(record, "checkpoint_json"), NULL);
    check_not_null(serialized);
    check_equal(turbo_graph_checkpoint_deserialize(serialized, strlen(serialized), &checkpoint), TURBO_GRAPH_EXEC_OK);
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_get_run(runtime, id, &record), 0);
    segment.runtime = runtime; segment.start_state = NULL; segment.start_checkpoint = checkpoint;
    segment.parent_checkpoint_id = checkpoint_id; segment.next_checkpoint_seq = 2;
    segment.run_created_at = json_get_string(record, "created_at");
    options.interrupt_before_count = 0;
    check_equal(turbo_runtime_segment_execute(&segment, &summary, &state), 0);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(state, "count"), 0), 2);
    json_free(summary); json_free(state); summary = state = NULL;
    /* An older interrupted run must not outrank the newly published completion,
     * even when the old run sorts first and has a later wall-clock timestamp. */
    check_equal(turbo_runtime_json_object_set_string(record, "id", "aaa-old"), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "updated_at", "9999-01-01T00:00:00Z"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "runs", "aaa-old", record), 0);
    json_free(record); record = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store); check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_runtime_get_published_thread_head(runtime, "thread", &record), SALTS_OK);
    check_equal(json_get_string(record, "id"), id);
    check_equal(json_get_string(record, "status"), "completed");
    check_equal(turbo_runtime_json_object_set_string(record, "updated_at", "9999-01-01T00:00:00Z"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "runs", id, record), 0);
    /* Equal timestamps retain backend order on the legacy query; head selection
     * must independently choose the explicitly published run. */
    check_equal(turbo_agent_runtime_latest_run_id_for_thread(runtime, "thread", &selected_id), 0);
    check_equal(selected_id, "aaa-old");
    free(selected_id); selected_id = NULL;
    check_equal(turbo_agent_runtime_current_run_id_for_thread(runtime, "thread", &selected_id), 0);
    check_equal(selected_id, id);
    check_equal(turbo_agent_runtime_get_thread_head_state_json_value(runtime, "thread", &state), 0);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(state, "count"), 0), 2);
  }
}

spec("bounded SQLite runtime records") {
  static char *directory, *text;
  static tstr path, large;
  static turbo_runtime_sqlite_config_t config;
  static turbo_agent_runtime_store_t store, second;
  static sqlite3 *raw;
  static json_value_t *records;

  before_each() {
    directory = tt_make_temp_dir("turbo-sqlite");
    check_not_null(directory);
    path = tstr_format("{}/session.db", directory);
    check_not_null(path);
    turbo_runtime_sqlite_config_init(&config);
    config.path = path;
    config.busy_timeout_ms = 1;
  }
  after_each() {
    close_store(&second); close_store(&store);
    if (raw) sqlite3_close(raw);
    raw = NULL;
    free(text); text = NULL;
    json_free(records); records = NULL;
    tstr_free(large); large = NULL;
    tstr_free(path); path = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  it("persists exact records, updates, collection isolation and filtered lists across reopen") {
    const char *payload = "{\"thread\":\"one\",\"integer\":9223372036854775807}";
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "runs", "../';--", payload), SALTS_OK);
    check_equal(store.put(store.user_data, "other", "../';--", "{}"), SALTS_OK);
    check_equal(store.put(store.user_data, "runs", "next", "{\"thread\":\"two\"}"), SALTS_OK);
    close_store(&store);
    config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "runs", "../';--", &text), SALTS_OK);
    check_equal(strcmp(text, payload), 0);
    free(text); text = NULL;
    check_equal(store.list(store.user_data, "runs", "thread", "one", &text), SALTS_OK);
    records = json_parse(text, strlen(text));
    check_not_null(records);
    check_equal(json_array_size(records), 1);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(json_array_get(records, 0), "integer"), 0), INT64_MAX);
    free(text); text = NULL;
    check_equal(store.put(store.user_data, "runs", "next", "{\"updated\":true}"), SALTS_OK);
    check_equal(store.put(store.user_data, "runs", "next", "invalid"), SALTS_EPROTO);
    check_equal(store.get(store.user_data, "runs", "next", &text), SALTS_OK);
    check_equal(strcmp(text, "{\"updated\":true}"), 0);
  }

  it("retains exclusive ownership across commits and releases it on close") {
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "c", "id", "{}"), SALTS_OK);
    check_equal(turbo_runtime_store_sqlite_open(&config, &second), SALTS_EBUSY);
    check_null(second.user_data);
    close_store(&store);
    check_equal(turbo_runtime_store_sqlite_open(&config, &second), SALTS_OK);
  }

  it("fails without creating a missing database when creation is disabled") {
    config.create_if_missing = 0;
    check_not_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_null(store.put);
    check_not_equal(sqlite3_open_v2(path, &raw, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
  }

  it("rejects foreign databases and unsupported versions without overwriting their data") {
    check_equal(sqlite3_open(path, &raw), SQLITE_OK);
    check_equal(sqlite3_exec(raw, "CREATE TABLE foreign_data(value); INSERT INTO foreign_data VALUES(7)", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw); raw = NULL;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_ENOTSUP);
    check_equal(sqlite3_open(path, &raw), SQLITE_OK);
    check_equal(sqlite3_exec(raw, "CREATE TABLE preserved AS SELECT value FROM foreign_data; DROP TABLE preserved;"
                                   "PRAGMA application_id=1413567060; PRAGMA user_version=99", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw); raw = NULL;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_ENOTSUP);
  }

  it("rejects corrupt JSON even when a list filter would exclude the row") {
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    close_store(&store);
    check_equal(sqlite3_open(path, &raw), SQLITE_OK);
    check_equal(sqlite3_exec(raw, "INSERT INTO runtime_records VALUES('c','bad','{')", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw); raw = NULL;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "c", "bad", &text), SALTS_EPROTO);
    check_null(text);
    check_equal(store.list(store.user_data, "c", "thread", "missing", &text), SALTS_EPROTO);
    check_null(text);
  }

  it("rejects a versioned database whose schema no longer enforces record identity") {
    check_equal(sqlite3_open(path, &raw), SQLITE_OK);
    check_equal(sqlite3_exec(raw, "CREATE TABLE runtime_records(collection TEXT,id TEXT,payload TEXT);"
                                   "PRAGMA application_id=1413567060; PRAGMA user_version=1", NULL, NULL, NULL), SQLITE_OK);
    sqlite3_close(raw); raw = NULL;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_EPROTO);
    check_null(store.user_data);
  }

  it("enforces record, scan and list limits without returning partial results") {
    config.max_record_bytes = 2; config.max_list_bytes = 4; config.max_list_records = 1;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "c", "one", "{}"), SALTS_OK);
    check_equal(store.put(store.user_data, "c", "one", "[1]"), SALTS_ENOSPC);
    check_equal(store.list(store.user_data, "c", NULL, NULL, &text), SALTS_OK);
    check_equal(strcmp(text, "[{}]"), 0);
    free(text); text = NULL;
    check_equal(store.put(store.user_data, "c", "two", "{}"), SALTS_OK);
    check_equal(store.list(store.user_data, "c", "x", "excluded", &text), SALTS_ENOSPC);
    check_null(text);
    close_store(&store);
    config.max_list_records = 2;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.list(store.user_data, "c", NULL, NULL, &text), SALTS_ENOSPC);
    check_null(text);
    check_equal(store.get(store.user_data, "c", "absent", &text), SALTS_ENOENT);
    check_null(text);
  }

  it("rolls back an oversized database update and preserves the previous committed value") {
    config.max_database_bytes = 65536;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "c", "one", "{}"), SALTS_OK);
    large = tstr_new_len(NULL, 128 * 1024);
    check_not_null(large);
    memset(large, 'x', tstr_len(large));
    large[0] = '"'; large[tstr_len(large) - 1] = '"';
    check_equal(store.put(store.user_data, "c", "one", large), SALTS_ENOSPC);
    close_store(&store);
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "c", "one", &text), SALTS_OK);
    check_equal(strcmp(text, "{}"), 0);
  }

  it("rejects memory paths, incompatible configuration and invalid identifiers") {
    config.path = ":memory:";
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_EINVAL);
    check_null(store.put);
    config.path = path; config.abi_version++;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_EINVAL);
    config.abi_version--;
    check_equal(turbo_runtime_store_sqlite_open(&config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "", "one", "{}"), SALTS_EINVAL);
    check_equal(store.put(store.user_data, "c", "bad-utf8", "{\"x\":\"\xff\"}"), SALTS_EPROTO);
    check_equal(store.get(store.user_data, "c", "", &text), SALTS_EINVAL);
    check_null(text);
  }
}

typedef struct sqlite_claim_worker_s {
  turbo_agent_runtime_t *runtime;
  int writer_id;
  const char *key;
  atomic_int *ready;
  atomic_int *go;
  atomic_int *created;
  atomic_int *exists;
  atomic_int *errors;
} sqlite_claim_worker_t;

static void sqlite_claim_worker(void *data) {
  sqlite_claim_worker_t *worker = (sqlite_claim_worker_t *)data;
  json_value_t *record;
  int rc;
  atomic_fetch_add(worker->ready, 1);
  record = json_create_object();
  if (!record ||
      turbo_runtime_json_object_set_int64(record, "writer", worker->writer_id) !=
          TURBO_RUNTIME_JSON_OK) {
    atomic_fetch_add(worker->errors, 1);
    json_free(record);
    return;
  }
  while (!atomic_load(worker->go)) cmeta_thread_yield();
  rc = turbo_agent_runtime_store_create_if_absent_json(
      worker->runtime, "sqlite_claims", worker->key, record);
  if (rc == TURBO_AGENT_RUNTIME_STORE_CREATE_CREATED) atomic_fetch_add(worker->created, 1);
  else if (rc == TURBO_AGENT_RUNTIME_STORE_CREATE_EXISTS) atomic_fetch_add(worker->exists, 1);
  else atomic_fetch_add(worker->errors, 1);
  json_free(record);
}

spec("SQLite Runtime Store atomic create-if-absent") {
  static char *directory;
  static tstr path;
  static turbo_runtime_sqlite_config_t config;
  static turbo_agent_runtime_store_ex_t extended;
  static turbo_agent_runtime_t *runtime;
  static json_value_t *stored;

  before_each() {
    directory = tt_make_temp_dir("turbo-sqlite-claim");
    check_not_null(directory);
    path = tstr_format("{}/claims.db", directory);
    check_not_null(path);
    turbo_runtime_sqlite_config_init(&config);
    config.path = path;
    turbo_agent_runtime_store_ex_init(&extended);
    runtime = NULL;
    stored = NULL;
  }

  after_each() {
    if (runtime) {
      turbo_agent_runtime_destroy(runtime);
      runtime = NULL;
      extended = (turbo_agent_runtime_store_ex_t){0};
    } else if (extended.store.user_data_free) {
      extended.store.user_data_free(extended.store.user_data);
      extended = (turbo_agent_runtime_store_ex_t){0};
    }
    json_free(stored);
    stored = NULL;
    tstr_free(path);
    path = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory);
    directory = NULL;
  }

  it("permits exactly one concurrent winner without overwriting its record") {
    enum { CLAIM_WORKERS = 8, CLAIM_ROUNDS = 6 };
    cmeta_thread_t threads[CLAIM_WORKERS] = {0};
    sqlite_claim_worker_t workers[CLAIM_WORKERS];
    atomic_int ready, go, created, exists, errors;
    json_value_t *candidate = NULL;
    char key[32];
    int round, i;
    check_equal(turbo_runtime_store_sqlite_open_ex(&config, &extended), SALTS_OK);
    runtime = turbo_agent_runtime_create_ex(&extended);
    check_not_null(runtime);
    extended = (turbo_agent_runtime_store_ex_t){0};
    for (round = 0; round < CLAIM_ROUNDS; ++round) {
      atomic_init(&ready, 0);
      atomic_init(&go, 0);
      atomic_init(&created, 0);
      atomic_init(&exists, 0);
      atomic_init(&errors, 0);
      snprintf(key, sizeof(key), "claim-%d", round);
      for (i = 0; i < CLAIM_WORKERS; ++i) {
        workers[i] = (sqlite_claim_worker_t){runtime, i, key, &ready, &go,
                                              &created, &exists, &errors};
        check_equal(cmeta_thread_create(&threads[i], sqlite_claim_worker, &workers[i]), SALTS_OK);
      }
      while (atomic_load(&ready) != CLAIM_WORKERS) cmeta_thread_yield();
      atomic_store(&go, 1);
      for (i = 0; i < CLAIM_WORKERS; ++i)
        check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      check_equal(atomic_load(&created), 1);
      check_equal(atomic_load(&exists), CLAIM_WORKERS - 1);
      check_equal(atomic_load(&errors), 0);

      check_equal(turbo_agent_runtime_store_get_json_status(
          runtime, "sqlite_claims", key, &stored), SALTS_OK);
      check_not_null(stored);
      check_true(turbo_runtime_json_value_as_int64(json_object_get(stored, "writer"), -1) >= 0);
      check_true(turbo_runtime_json_value_as_int64(json_object_get(stored, "writer"), -1) <
                 CLAIM_WORKERS);
      json_free(stored);
      stored = NULL;

      candidate = json_create_object();
      check_not_null(candidate);
      check_equal(turbo_runtime_json_object_set_int64(candidate, "writer", 999),
                  TURBO_RUNTIME_JSON_OK);
      check_equal(turbo_agent_runtime_store_create_if_absent_json(
          runtime, "sqlite_claims", key, candidate), TURBO_AGENT_RUNTIME_STORE_CREATE_EXISTS);
      json_free(candidate);
      candidate = NULL;
      check_equal(turbo_agent_runtime_store_get_json_status(
          runtime, "sqlite_claims", key, &stored), SALTS_OK);
      check_not_null(stored);
      check_true(turbo_runtime_json_value_as_int64(json_object_get(stored, "writer"), -1) != 999);
      json_free(stored);
      stored = NULL;
    }
    json_free(candidate);
    {
      turbo_agent_runtime_store_t second = {0};
      check_equal(turbo_runtime_store_sqlite_open(&config, &second), SALTS_EBUSY);
      check_null(second.user_data);
      close_store(&second);
    }
  }

  it("reports an unsupported conditional-create capability on legacy stores") {
    turbo_agent_runtime_store_t legacy = {0};
    turbo_agent_runtime_t *legacy_runtime = NULL;
    json_value_t *record = json_create_object();
    check_equal(turbo_runtime_store_sqlite_open(&config, &legacy), SALTS_OK);
    legacy_runtime = turbo_agent_runtime_create(&legacy);
    check_not_null(legacy_runtime);
    legacy = (turbo_agent_runtime_store_t){0};
    check_not_null(record);
    check_equal(turbo_agent_runtime_store_create_if_absent_json(
        legacy_runtime, "sqlite_claims", "one", record), SALTS_ENOTSUP);
    json_free(record);
    turbo_agent_runtime_destroy(legacy_runtime);
  }
}
