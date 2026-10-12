#include "tinytest.h"
#include "turbo_runtime_store.h"
#include <cmeta_fs.h>
#include <cmeta_uuid.h>
#include <fmt.h>
#include <salts/thread.h>
#include <salts/error_codes.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Callback buffers deliberately use the public malloc/free ABI. */
static char *store_test_text(const char *text) {
  size_t size;
  char *copy;
  if (!text) return NULL;
  size = strlen(text) + 1;
  copy = malloc(size);
  if (copy) memcpy(copy, text, size);
  return copy;
}

typedef struct store_test_probe_s {
  const char *get_text;
  const char *list_text;
  int put_result;
  int get_result;
  int list_result;
  int destroyed;
  atomic_int active;
  atomic_int overlaps;
  atomic_int calls;
  atomic_int go;
} store_test_probe_t;

static void store_test_enter(store_test_probe_t *probe) {
  if (atomic_fetch_add(&probe->active, 1) != 0) atomic_fetch_add(&probe->overlaps, 1);
  cmeta_thread_yield();
  atomic_fetch_add(&probe->calls, 1);
}

static void store_test_leave(store_test_probe_t *probe) {
  atomic_fetch_sub(&probe->active, 1);
}

static int store_test_put(void *data, const char *collection, const char *id,
                          const char *record) {
  store_test_probe_t *probe = data;
  (void)collection; (void)id; (void)record;
  store_test_enter(probe);
  store_test_leave(probe);
  return probe->put_result;
}

static int store_test_get(void *data, const char *collection, const char *id,
                          char **record) {
  store_test_probe_t *probe = data;
  (void)collection; (void)id;
  store_test_enter(probe);
  *record = store_test_text(probe->get_text);
  store_test_leave(probe);
  return probe->get_result;
}

static int store_test_list(void *data, const char *collection, const char *key,
                           const char *value, char **records) {
  store_test_probe_t *probe = data;
  (void)collection; (void)key; (void)value;
  store_test_enter(probe);
  *records = store_test_text(probe->list_text);
  store_test_leave(probe);
  return probe->list_result;
}

static void store_test_destroy(void *data) {
  ++((store_test_probe_t *)data)->destroyed;
}

static turbo_agent_runtime_store_t store_test_callbacks(store_test_probe_t *probe) {
  turbo_agent_runtime_store_t store = {
      store_test_put, store_test_get, store_test_list, probe, store_test_destroy};
  return store;
}

enum { STORE_TEST_WORKERS = 4, STORE_TEST_ROUNDS = 32 };
typedef struct store_test_worker_s {
  turbo_agent_runtime_t *runtime;
  store_test_probe_t *probe;
  int result;
} store_test_worker_t;

static void store_test_worker(void *data) {
  store_test_worker_t *worker = data;
  json_value_t *record = json_create_object();
  json_value_t *output = NULL;
  int i;
  worker->result = -1;
  if (!record) return;
  while (!atomic_load(&worker->probe->go)) cmeta_thread_yield();
  for (i = 0; i < STORE_TEST_ROUNDS; ++i) {
    if (turbo_agent_runtime_store_put_json(worker->runtime, "items", "one", record) != 0 ||
        turbo_agent_runtime_store_get_json(worker->runtime, "items", "one", &output) != 0)
      goto done;
    json_free(output);
    output = NULL;
    if (turbo_agent_runtime_store_list_json(worker->runtime, "items", NULL, NULL, &output) != 0)
      goto done;
    json_free(output);
    output = NULL;
  }
  worker->result = 0;
done:
  json_free(output);
  json_free(record);
}

typedef struct store_claim_worker_s {
  turbo_agent_runtime_t *runtime;
  atomic_int *go;
  int result;
} store_claim_worker_t;

static void store_claim_worker(void *arg) {
  store_claim_worker_t *worker = (store_claim_worker_t *)arg;
  json_value_t *record = json_create_object();
  worker->result = SALTS_ENOMEM;
  if (!record) return;
  while (!atomic_load_explicit(worker->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->result = turbo_agent_runtime_store_put_json_if_absent(
      worker->runtime, "claims", "shared-identity", record);
  json_free(record);
}

spec("Atomic per-Runtime claim-if-absent") {
  it("admits exactly one of eight simultaneous claims") {
    enum { CLAIMERS = 8 };
    turbo_agent_runtime_store_t backend = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&backend);
    store_claim_worker_t workers[CLAIMERS] = {0};
    cmeta_thread_t threads[CLAIMERS] = {0};
    json_value_t *stored = NULL;
    atomic_int go;
    int succeeded = 0, conflicts = 0;
    atomic_init(&go, 0);
    check_not_null(runtime);
    for (int i = 0; i < CLAIMERS; ++i) {
      workers[i].runtime = runtime;
      workers[i].go = &go;
      check_equal(cmeta_thread_create(&threads[i], store_claim_worker, &workers[i]),
                  SALTS_OK);
    }
    atomic_store_explicit(&go, 1, memory_order_release);
    for (int i = 0; i < CLAIMERS; ++i) {
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      if (workers[i].result == SALTS_OK) ++succeeded;
      if (workers[i].result == SALTS_EALREADY) ++conflicts;
    }
    check_equal(succeeded, 1);
    check_equal(conflicts, CLAIMERS - 1);
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "claims", "shared-identity", &stored), SALTS_OK);
    check_not_null(stored);
    json_free(stored);
    turbo_agent_runtime_destroy(runtime);
  }

  it("preserves unknown backend read failure and never substitutes a write") {
    store_test_probe_t probe = {0};
    turbo_agent_runtime_store_t backend = store_test_callbacks(&probe);
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&backend);
    json_value_t *record = json_create_object();
    check_not_null(runtime);
    check_not_null(record);
    probe.get_result = SALTS_EIO;
    check_equal(turbo_agent_runtime_store_put_json_if_absent(
        runtime, "claims", "one", record), SALTS_EIO);
    check_equal(atomic_load(&probe.calls), 1);
    json_free(record);
    turbo_agent_runtime_destroy(runtime);
  }
}

spec("File store enumeration failure") {
  static char *directory;
  static tstr path;
  static turbo_agent_runtime_store_t store;
  static char *text;
  after_each() {
    if (store.user_data_free) store.user_data_free(store.user_data);
    store = (turbo_agent_runtime_store_t){0};
    free(text); text = NULL;
    tstr_free(path); path = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }
  it("distinguishes an empty collection from a non-directory collection") {
    cmeta_fs_buf_t bytes = {"{}", 2};
    directory = tt_make_temp_dir("turbo-file-errors");
    check_not_null(directory);
    store = turbo_agent_runtime_store_file_create(directory);
    check_not_null(store.list);
    check_equal(store.list(store.user_data, "missing", NULL, NULL, &text), 0);
    check_equal(text, "[]");
    free(text); text = NULL;
    path = tstr_format("{}/items", directory);
    check_not_null(path);
    check_equal(cmeta_fs_write_file(path, &bytes), 0);
    check_equal(store.list(store.user_data, "items", NULL, NULL, &text), -1);
    check_null(text);
  }
}

spec("Runtime record store without Agent") {
  static turbo_agent_runtime_t *runtime;
  static json_value_t *record;
  static json_value_t *output;
  static tstr root;
  static tstr record_path;
  static store_test_probe_t probe;
  static cmeta_thread_t threads[STORE_TEST_WORKERS];
  static store_test_worker_t workers[STORE_TEST_WORKERS];
  static int started;

  before_each() {
    runtime = NULL;
    record = NULL;
    output = NULL;
    root = NULL;
    record_path = NULL;
    started = 0;
    memset(&probe, 0, sizeof(probe));
    atomic_init(&probe.active, 0);
    atomic_init(&probe.overlaps, 0);
    atomic_init(&probe.calls, 0);
    atomic_init(&probe.go, 0);
    probe.get_text = "{\"value\":1}";
    probe.list_text = "[]";
    memset(threads, 0, sizeof(threads));
    memset(workers, 0, sizeof(workers));
  }

  after_each() {
    int i;
    atomic_store(&probe.go, 1);
    for (i = 0; i < started; ++i) cmeta_thread_join(&threads[i]);
    turbo_agent_runtime_destroy(runtime);
    json_free(output);
    json_free(record);
    tstr_free(record_path);
    if (root) {
      /* Only remove paths created by this fixture; never traverse the temp tree. */
      static const char *paths[] = {"items/one.json", "items", "threads", "runs", "checkpoints"};
      for (i = 0; i < 5; ++i) {
        tstr path = tstr_format("{}/{}", root, paths[i]);
        if (path) {
          if (i == 0) cmeta_fs_unlink(path);
          else cmeta_fs_rmdir(path);
          tstr_free(path);
        }
      }
      cmeta_fs_rmdir(root);
      tstr_free(root);
    }
  }

  it("preserves typed read errors while the legacy getter retains its contract") {
    turbo_agent_runtime_store_t backend = store_test_callbacks(&probe);
    runtime = turbo_agent_runtime_create(&backend);
    check_not_null(runtime);
    probe.get_result = SALTS_ENOENT;
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "items", "one", &output), SALTS_ENOENT);
    check_null(output);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), -1);
    probe.get_result = SALTS_EIO;
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "items", "one", &output), SALTS_EIO);
    check_null(output);
    probe.get_result = 0; probe.get_text = "{";
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "items", "one", &output), SALTS_EPROTO);
    check_null(output);
  }

  it("keeps independent values and isolates collections, replacements, and filters") {
    static const char input[] = "{\"group\":\"a\",\"value\":9007199254740993}";
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    record = json_parse(input, sizeof(input) - 1);
    check_not_null(record);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "items", "one", record), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "other", "one", record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "group", "b"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "items", "one", record), 0);
    check_equal(turbo_runtime_json_object_set_string(record, "group", "changed"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), 0);
    check_equal(json_get_string(output, "group"), "b");
    check_true(turbo_runtime_json_value_as_int64(json_object_get(output, "value"), 0) ==
               INT64_C(9007199254740993));
    check_equal(turbo_runtime_json_object_set_string(output, "group", "local"), TURBO_RUNTIME_JSON_OK);
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", "group", "b", &output), 0);
    check_equal(json_array_size(output), 1);
    check_equal(json_get_string(json_array_get(output, 0), "group"), "b");
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", "group", "a", &output), 0);
    check_equal(json_array_size(output), 0);
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "other", "one", &output), 0);
    check_equal(json_get_string(output, "group"), "a");
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "missing", &output), -1);
    check_null(output);
  }

  it("transfers backend ownership only on successful creation and copies callbacks") {
    turbo_agent_runtime_store_t store = store_test_callbacks(&probe);
    store.list = NULL;
    check_null(turbo_agent_runtime_create(&store));
    check_equal(probe.destroyed, 0);
    store.list = store_test_list;
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store.get = NULL;
    store.user_data_free = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), 0);
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    check_equal(probe.destroyed, 1);
    check_true(turbo_runtime_json_value_as_int64(json_object_get(output, "value"), 0) == 1);
    turbo_agent_runtime_destroy(NULL);
    check_equal(probe.destroyed, 1);
  }

  it("propagates failures, clears rejected outputs, and releases the callback lock") {
    turbo_agent_runtime_store_t store = store_test_callbacks(&probe);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    record = json_create_object();
    check_not_null(record);
    probe.put_result = -73;
    check_equal(turbo_agent_runtime_store_put_json(runtime, "items", "one", record), -73);
    probe.get_result = -9;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), -1);
    check_null(output);
    probe.get_result = 0;
    probe.get_text = "{invalid";
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), -1);
    check_null(output);
    probe.get_text = NULL;
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), -1);
    check_null(output);
    probe.list_text = "{}";
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", NULL, NULL, &output), -1);
    check_null(output);
    probe.list_text = "[]";
    probe.list_result = -8;
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", NULL, NULL, &output), -1);
    check_null(output);
    output = (json_value_t *)(uintptr_t)1;
    check_equal(turbo_agent_runtime_store_get_json(NULL, "items", "one", &output), -1);
    check_null(output);
    output = (json_value_t *)(uintptr_t)1;
    check_equal(turbo_agent_runtime_store_list_json(runtime, NULL, NULL, NULL, &output), -1);
    check_null(output);
    probe.list_result = 0;
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", NULL, NULL, &output), 0);
    check_equal(json_array_size(output), 0);
  }

  it("serializes callbacks from concurrent record callers") {
    turbo_agent_runtime_store_t store = store_test_callbacks(&probe);
    int i;
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    for (i = 0; i < STORE_TEST_WORKERS; ++i) {
      workers[i].runtime = runtime;
      workers[i].probe = &probe;
      check_equal(cmeta_thread_create(&threads[i], store_test_worker, &workers[i]), SALTS_OK);
      ++started;
    }
    atomic_store(&probe.go, 1);
    for (i = 0; i < STORE_TEST_WORKERS; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    started = 0;
    for (i = 0; i < STORE_TEST_WORKERS; ++i) check_equal(workers[i].result, 0);
    check_equal(atomic_load(&probe.calls), STORE_TEST_WORKERS * STORE_TEST_ROUNDS * 3);
    check_equal(atomic_load(&probe.overlaps), 0);
    check_equal(atomic_load(&probe.active), 0);
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    check_equal(probe.destroyed, 1);
  }

  it("reopens legacy file records and rejects corrupt JSON even outside the filter") {
    static char legacy_record[] = "{\"group\":\"persisted\",\"exact\":9007199254740993}";
    cmeta_fs_buf_t legacy_bytes = {legacy_record, sizeof(legacy_record) - 1};
    char temp_dir[4096];
    char uuid_text[SALTS_UUID_STRING_SIZE];
    cmeta_uuid_t uuid;
    turbo_agent_runtime_store_t store;
    check_equal(cmeta_fs_get_tmpdir(temp_dir, sizeof(temp_dir)), SALTS_OK);
    check_equal(cmeta_uuid_v4_generate(&uuid), SALTS_OK);
    check_equal(cmeta_uuid_format(&uuid, uuid_text, sizeof(uuid_text)), SALTS_OK);
    root = tstr_format("{}/turbo-runtime-store-{}", temp_dir, uuid_text);
    check_not_null(root);
    store = turbo_agent_runtime_store_file_create(root);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    record = json_create_object();
    check_not_null(record);
    check_equal(turbo_runtime_json_object_set_string(record, "group", "persisted"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "items", "one", record), 0);
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    record_path = tstr_format("{}/items/one.json", root);
    check_not_null(record_path);
    check_equal(cmeta_fs_write_file(record_path, &legacy_bytes), SALTS_OK);
    store = turbo_agent_runtime_store_file_create(root);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), 0);
    check_equal(json_get_string(output, "group"), "persisted");
    check_true(turbo_runtime_json_value_as_int64(json_object_get(output, "exact"), 0) ==
               INT64_C(9007199254740993));
    json_free(output); output = NULL;
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", "group", "persisted", &output), 0);
    check_equal(json_array_size(output), 1);
    json_free(output); output = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    store = turbo_agent_runtime_store_file_create(root);
    /* Corrupt through the raw backend before transferring it to the handle. */
    check_not_null(store.put);
    check_equal(store.put(store.user_data, "items", "one", "{invalid"), 0);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_equal(turbo_agent_runtime_store_get_json(runtime, "items", "one", &output), -1);
    check_null(output);
    check_equal(turbo_agent_runtime_store_list_json(runtime, "items", "group", "absent", &output), -1);
    check_null(output);
  }
}
