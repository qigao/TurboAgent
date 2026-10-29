#include "../src/turbo_agent_tool_executor_internal.h"
#include "tinytest.h"
#include "turbo_agent_policy.h"
#include "turbo_runtime_store.h"
#include "turbo_runtime_store_sqlite.h"
#include "turbo_agent_evidence_diagram.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

static const char journal_output[] = "saved result";
static const char journal_collection[] = "agent_tool_journal";

typedef struct journal_store_s {
  turbo_agent_runtime_store_t inner;
  const char *fail_phase;
  int fail_get, fail_list, get_error;
  int writes, failures, calls, lists, reject_claim;
  const char *get_error_id;
} journal_store_t;

static int journal_put(void *user, const char *collection, const char *id, const char *text) {
  journal_store_t *store = user;
  if (strcmp(collection, journal_collection) == 0) {
    json_value_t *record = json_parse(text, strlen(text));
    const char *phase = json_get_string(record, "phase");
    int fail = store->fail_phase && phase && strcmp(phase, store->fail_phase) == 0;
    json_free(record);
    ++store->writes;
    if (fail) {
      ++store->failures;
      return -1;
    }
  }
  return store->inner.put(store->inner.user_data, collection, id, text);
}

static int journal_create(void *user, const char *collection, const char *id,
                          const char *text) {
  journal_store_t *store = user;
  if (!store->inner.create_if_absent ||
      (store->reject_claim && strcmp(collection, "agent_tool_claims") == 0))
    return SALTS_ENOTSUP;
  if (strcmp(collection, journal_collection) == 0) {
    json_value_t *record = json_parse(text, strlen(text));
    const char *phase = record ? json_get_string(record, "phase") : NULL;
    int fail = store->fail_phase && phase && strcmp(phase, store->fail_phase) == 0;
    json_free(record);
    ++store->writes;
    if (fail) { ++store->failures; return -1; }
  }
  return store->inner.create_if_absent(store->inner.user_data, collection, id, text);
}

static int journal_get(void *user, const char *collection, const char *id, char **out) {
  journal_store_t *store = user;
  if (store->get_error && (!store->get_error_id || !strcmp(store->get_error_id, id))) return store->get_error;
  if (store->fail_get) return -1;
  return store->inner.get(store->inner.user_data, collection, id, out);
}

static int journal_list(void *user, const char *collection, const char *key, const char *value,
                        char **out) {
  journal_store_t *store = user;
  ++store->lists;
  if (store->fail_list) return -1;
  return store->inner.list(store->inner.user_data, collection, key, value, out);
}

static void journal_destroy(void *user) {
  journal_store_t *store = user;
  if (store->inner.user_data_free) store->inner.user_data_free(store->inner.user_data);
  store->inner = (turbo_agent_runtime_store_t){0};
}

static turbo_tool_status_t journal_invoke(const char *arguments,
                                          const turbo_tool_execution_context_t *context, char **out,
                                          void *user) {
  static const char text[] = "{\"host\":\"retained detail\"}";
  journal_store_t *store = user;
  json_value_t *detail;
  (void)arguments;
  *out = NULL;
  ++store->calls;
  detail = json_parse(text, sizeof(text) - 1);
  if (!detail) return TURBO_TOOL_OUT_OF_MEMORY;
  if (context && context->detail_sink) context->detail_sink(detail, context->detail_sink_user_data);
  json_free(detail);
  /* Public text callbacks transfer malloc-owned buffers. */
  *out = malloc(sizeof(journal_output));
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, journal_output, sizeof(journal_output));
  return TURBO_TOOL_OK;
}

typedef struct bounded_journal_probe_s {
  int calls;
  size_t output_bytes;
} bounded_journal_probe_t;

static turbo_tool_status_t bounded_journal_invoke(
    const char *arguments, const turbo_tool_execution_context_t *context,
    char **out, void *user) {
  bounded_journal_probe_t *probe = user;
  (void)arguments;
  (void)context;
  *out = NULL;
  ++probe->calls;
  *out = malloc(probe->output_bytes + 1);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memset(*out, 'x', probe->output_bytes);
  (*out)[probe->output_bytes] = '\0';
  return TURBO_TOOL_OK;
}

static turbo_agent_tool_execution_t journal_call(const char *id) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = id;
  call.turn_key = "turn";
  call.tool_name = "probe";
  call.arguments_json = "{}";
  call.policy =
      (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE};
  return call;
}

static int journal_execute(turbo_agent_tool_executor_t *executor, turbo_agent_runtime_t *runtime,
                           turbo_tool_registry_t *registry, turbo_agent_tool_execution_t *calls,
                           size_t count) {
  turbo_agent_policy_t host_policy = turbo_agent_policy_default();
  /* The journal suite tests recovery under an explicitly authorized Host Tool.
   * It must not rely on newly created Agents granting legacy tools. */
  host_policy.allow_custom_tools = 1;
  return turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run", NULL, NULL,
                                           registry, &host_policy, calls, count);
}

spec("durable tool journal across SQLite reopen") {
  static journal_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_tool_registry_t *registry;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t first, second;
  static char *directory;
  static tstr path;
  static turbo_runtime_sqlite_config_t config;

  before_each() {
    turbo_tool_definition_v4_t definition = {0};
    turbo_agent_runtime_store_t callbacks;
    directory = tt_make_temp_dir("turbo-journal");
    check_not_null(directory);
    path = tstr_format("{}/journal.db", directory);
    check_not_null(path);
    turbo_runtime_sqlite_config_init(&config); config.path = path;
    config.max_list_records = 1;
    store = (journal_store_t){0};
    check_equal(turbo_runtime_store_sqlite_open(&config, &store.inner), SALTS_OK);
    callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list, &store, journal_destroy, journal_create};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "probe";
    definition.definition.description = "Durable journal probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &store;
    definition.context_handler = journal_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
  }
  after_each() {
    turbo_agent_tool_executor_destroy(executor); executor = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    journal_destroy(&store);
    free(first.output); json_free(first.detail); first = (turbo_agent_tool_execution_t){0};
    free(second.output); json_free(second.detail); second = (turbo_agent_tool_execution_t){0};
    tstr_free(path); path = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  it("admits new calls beyond the collection scan budget without listing") {
    static const char *ids[] = {"one", "two", "three"};
    for (size_t i = 0; i < 3; ++i) {
      first = journal_call(ids[i]);
      check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_OK);
      check_equal(first.status, TURBO_TOOL_OK);
      free(first.output); json_free(first.detail); first = (turbo_agent_tool_execution_t){0};
    }
    check_equal(store.calls, 3);
    check_equal(store.lists, 0);
  }

  for (int scoped = 0; scoped < 2; ++scoped) for (int fail_commit = 0; fail_commit < 2; ++fail_commit) {
    it("does not repeat %s side effects after reopen when committed write %s",
        scoped ? "nested" : "root", fail_commit ? "fails" : "succeeds") {
      turbo_agent_runtime_store_t callbacks;
      store.fail_phase = fail_commit ? "committed" : NULL;
      first = journal_call("durable-call");
      if (scoped) { first.turn_key = "executor:0:0"; first.legacy_turn_key = "0"; }
      check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_OK);
      check_equal(store.calls, 1);
      check_equal(first.status, fail_commit ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK);
      turbo_agent_tool_executor_destroy(executor); executor = NULL;
      turbo_agent_runtime_destroy(runtime); runtime = NULL;
      store.fail_phase = NULL;
      config.create_if_missing = 0;
      check_equal(turbo_runtime_store_sqlite_open(&config, &store.inner), SALTS_OK);
      callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list, &store, journal_destroy, journal_create};
      runtime = turbo_agent_runtime_create(&callbacks);
      check_not_null(runtime);
      check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
      second = journal_call("durable-call");
      if (scoped) { second.turn_key = "executor:0:0"; second.legacy_turn_key = "0"; }
      check_equal(journal_execute(executor, runtime, registry, &second, 1), SALTS_OK);
      check_equal(store.calls, 1);
      check_true(second.replayed);
      check_equal(second.status, fail_commit ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK);
      if (fail_commit) check_null(second.output);
      else {
        check_equal(second.output, journal_output);
        check_equal(json_get_string(second.detail, "host"), "retained detail");
      }
    }
  }
}

spec("legacy file tool journal recovery") {
  static journal_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_tool_registry_t *registry;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t first, second;
  static char *directory;

  before_each() {
    turbo_tool_definition_v4_t definition = {0};
    turbo_agent_runtime_store_t callbacks;
    directory = tt_make_temp_dir("turbo-journal-file");
    check_not_null(directory);
    store = (journal_store_t){0};
    store.inner = turbo_agent_runtime_store_file_create(directory);
    check_not_null(store.inner.put);
    check_not_null(store.inner.get);
    check_not_null(store.inner.list);
    check_null(store.inner.create_if_absent);
    callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list,
                                              &store, journal_destroy, journal_create};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "probe";
    definition.definition.description = "Legacy file journal probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &store;
    definition.context_handler = journal_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
  }
  after_each() {
    turbo_agent_tool_executor_destroy(executor); executor = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    journal_destroy(&store);
    free(first.output); json_free(first.detail); first = (turbo_agent_tool_execution_t){0};
    free(second.output); json_free(second.detail); second = (turbo_agent_tool_execution_t){0};
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  it("rejects new file-store effects without atomic admission, including after reopen") {
    turbo_agent_runtime_store_t callbacks;
    json_value_t *records = NULL;
    first = journal_call("file-no-atomic-claim");
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_ENOTSUP);
    check_equal(store.calls, 0);
    check_equal(store.writes, 0);
    check_false(first.replayed);
    check_null(first.output);
    check_equal(turbo_agent_runtime_store_list_json(
        runtime, journal_collection, NULL, NULL, &records), SALTS_OK);
    check_equal(json_array_size(records), 0);
    json_free(records); records = NULL;

    turbo_agent_tool_executor_destroy(executor); executor = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    store.inner = turbo_agent_runtime_store_file_create(directory);
    check_not_null(store.inner.put);
    check_null(store.inner.create_if_absent);
    callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list,
                                              &store, journal_destroy, journal_create};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    second = journal_call("file-no-atomic-claim");
    check_equal(journal_execute(executor, runtime, registry, &second, 1), SALTS_ENOTSUP);
    check_equal(store.calls, 0);
    check_equal(store.writes, 0);
    check_null(second.output);
  }
}

spec("bounded SQLite tool journal recovery") {
  static bounded_journal_probe_t probe;
  static turbo_agent_runtime_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_tool_registry_t *registry;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t first, replay;
  static turbo_runtime_sqlite_config_t sqlite_config;
  static turbo_agent_tool_executor_config_t executor_config;
  static char *directory;
  static char *large_call_id;
  static tstr path;
  static json_value_t *records;

  before_each() {
    turbo_tool_definition_v4_t definition = {0};
    directory = tt_make_temp_dir("turbo-journal-limit");
    check_not_null(directory);
    path = tstr_format("{}/journal.db", directory);
    check_not_null(path);
    turbo_runtime_sqlite_config_init(&sqlite_config);
    sqlite_config.path = path;
    probe = (bounded_journal_probe_t){0};
    store = (turbo_agent_runtime_store_t){0};
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "bounded-probe";
    definition.definition.description = "Bounded Journal probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &probe;
    definition.execution_policy = (turbo_tool_execution_policy_t){
        TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE};
    definition.context_handler = bounded_journal_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    turbo_agent_tool_executor_config_init(&executor_config);
    executor_config.max_output_bytes = 4096;
  }

  after_each() {
    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    if (store.user_data_free) store.user_data_free(store.user_data);
    store = (turbo_agent_runtime_store_t){0};
    free(first.output);
    json_free(first.detail);
    first = (turbo_agent_tool_execution_t){0};
    free(replay.output);
    json_free(replay.detail);
    replay = (turbo_agent_tool_execution_t){0};
    json_free(records);
    records = NULL;
    free(large_call_id);
    large_call_id = NULL;
    tstr_free(path);
    path = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory);
    directory = NULL;
  }

  it("rejects an over-quota planned record before callback admission") {
    sqlite_config.max_record_bytes = 2;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    first = journal_call("planned-quota");
    first.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_ENOSPC);
    check_equal(probe.calls, 0);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 0);
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 0);
  }

  it("rejects a database-full planned record before callback admission") {
    sqlite_config.max_record_bytes = 256 * 1024;
    sqlite_config.max_database_bytes = 65536;
    large_call_id = malloc(96 * 1024 + 1);
    check_not_null(large_call_id);
    memset(large_call_id, 'p', 96 * 1024);
    large_call_id[96 * 1024] = '\0';
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    first = journal_call(large_call_id);
    first.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_ENOSPC);
    check_equal(probe.calls, 0);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 0);
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 0);
  }

  it("keeps a database-full admission fence before effect and retries only after quota increases") {
    sqlite_config.max_record_bytes = 256 * 1024;
    sqlite_config.max_database_bytes = 65536;
    large_call_id = malloc(32 * 1024 + 1);
    check_not_null(large_call_id);
    memset(large_call_id, 'a', 32 * 1024);
    large_call_id[32 * 1024] = '\0';
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    first = journal_call(large_call_id);
    first.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_OK);
    check_equal(probe.calls, 0);
    check_true(first.replayed);
    check_equal(first.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "planned");
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 0);
    json_free(records);
    records = NULL;

    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    sqlite_config.create_if_missing = 0;
    sqlite_config.max_database_bytes = 256 * 1024;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    replay = journal_call(large_call_id);
    replay.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &replay, 1), SALTS_OK);
    check_equal(probe.calls, 1);
    check_false(replay.replayed);
    check_equal(replay.status, TURBO_TOOL_OK);
    check_not_null(replay.output);
    check_equal(strlen(replay.output), 0);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "admitted");
  }

  it("keeps an over-quota committed result unknown across reopen without retry") {
    sqlite_config.max_record_bytes = 1024;
    probe.output_bytes = 2048;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    first = journal_call("commit-quota");
    first.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(first.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_false(first.replayed);
    check_null(first.output);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "started");
    check_null(json_object_get(json_array_get(records, 0), "output"));
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "admitted");
    json_free(records);
    records = NULL;

    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    sqlite_config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    replay = journal_call("commit-quota");
    replay.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &replay, 1), SALTS_OK);
    check_equal(probe.calls, 1);
    check_true(replay.replayed);
    check_equal(replay.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_null(replay.output);
  }

  it("keeps a database-full committed result unknown across reopen without retry") {
    sqlite_config.max_record_bytes = 256 * 1024;
    sqlite_config.max_database_bytes = 65536;
    executor_config.max_output_bytes = 256 * 1024;
    probe.output_bytes = 128 * 1024;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    first = journal_call("commit-database-full");
    first.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &first, 1), SALTS_OK);
    check_equal(probe.calls, 1);
    check_equal(first.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_false(first.replayed);
    check_null(first.output);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, journal_collection, NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "started");
    check_null(json_object_get(json_array_get(records, 0), "output"));
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtime, "agent_tool_claims", NULL, NULL, &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "admitted");
    json_free(records);
    records = NULL;

    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    sqlite_config.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&sqlite_config, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    store = (turbo_agent_runtime_store_t){0};
    check_equal(turbo_agent_tool_executor_create(&executor_config, &executor), SALTS_OK);
    replay = journal_call("commit-database-full");
    replay.tool_name = "bounded-probe";
    check_equal(journal_execute(executor, runtime, registry, &replay, 1), SALTS_OK);
    check_equal(probe.calls, 1);
    check_true(replay.replayed);
    check_equal(replay.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_null(replay.output);
  }
}

spec("tool journal admission and persistence failures") {
  static journal_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_tool_registry_t *registry;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t seed, calls[2];
  static json_value_t *records, *record, *value, *stored;
  static tstr id;
  static char *expected, *actual;

  before_each() {
    turbo_agent_runtime_store_t callbacks;
    turbo_tool_definition_v4_t definition = {0};
    store = (journal_store_t){0};
    store.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(store.inner.put);
    callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list, &store, journal_destroy, journal_create};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "probe";
    definition.definition.description = "Journal probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &store;
    definition.context_handler = journal_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    seed = journal_call("seed");
    check_equal(journal_execute(executor, runtime, registry, &seed, 1), SALTS_OK);
    check_equal(seed.status, TURBO_TOOL_OK);
    check_equal(store.calls, 1);
    check_equal(turbo_agent_runtime_store_list_json(runtime, journal_collection, "call_id", "seed",
                                                    &records),
                0);
    check_equal(json_array_size(records), 1);
    record = json_clone(json_array_get(records, 0));
    check_not_null(record);
    id = tstr_dup(json_get_string(record, "journal_id"));
    check_not_null(id);
    store.writes = 0;
  }

  after_each() {
    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    journal_destroy(&store);
    free(seed.output);
    json_free(seed.detail);
    seed = (turbo_agent_tool_execution_t){0};
    for (size_t i = 0; i < 2; ++i) {
      free(calls[i].output);
      json_free(calls[i].detail);
      calls[i] = (turbo_agent_tool_execution_t){0};
    }
    json_free(records);
    json_free(record);
    json_free(value);
    json_free(stored);
    records = record = value = stored = NULL;
    tstr_free(id);
    id = NULL;
    json_serialize_free(expected);
    json_serialize_free(actual);
    expected = actual = NULL;
  }

  it("refuses an unsupported backend-entry claim before invoking a tool") {
    json_value_t *claims = NULL;
    store.reject_claim = 1;
    calls[0] = journal_call("claim-unsupported");
    check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_OK);
    check_equal(calls[0].status, TURBO_TOOL_ERROR);
    check_true(calls[0].replayed);
    check_null(calls[0].output);
    check_equal(store.calls, 1);
    check_equal(turbo_agent_runtime_store_list_json(
        runtime, "agent_tool_claims", NULL, NULL, &claims), SALTS_OK);
    check_equal(json_array_size(claims), 1); /* seed's claim only */
    json_free(claims);
    claims = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
        runtime, journal_collection, "call_id", "claim-unsupported", &claims), SALTS_OK);
    check_equal(json_array_size(claims), 1);
    check_equal(json_get_string(json_array_get(claims, 0), "phase"), "planned");
    json_free(claims);
  }

  const char *legacy_phases[] = {"planned", "started", "committed"};
  for (size_t i = 0; i < 3; ++i) {
    it("refuses ambiguous legacy %s records without migration or execution", legacy_phases[i]) {
      check_equal(turbo_runtime_json_object_set_string(record, "phase", legacy_phases[i]), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, journal_collection, id, record), 0);
      expected = json_serialize(record, NULL); check_not_null(expected);
      store.writes = 0; store.lists = 0;
      calls[0] = journal_call("seed"); calls[0].turn_key = "executor:1:0";
      calls[0].legacy_turn_key = "turn";
      check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_EBUSY);
      check_equal(store.calls, 1); check_equal(store.writes, 0); check_equal(store.lists, 0);
      check_false(calls[0].replayed); check_null(calls[0].output);
      check_equal(turbo_agent_runtime_store_get_json(runtime, journal_collection, id, &stored), 0);
      actual = json_serialize(stored, NULL); check_not_null(actual); check_equal(actual, expected);
    }
  }
  const int legacy_errors[] = {SALTS_EIO, SALTS_EPROTO, -1};
  for (size_t i = 0; i < sizeof(legacy_errors) / sizeof(legacy_errors[0]); ++i) {
    it("refuses legacy read error %d before creating a scoped record", legacy_errors[i]) {
      store.get_error = legacy_errors[i]; store.get_error_id = id;
      calls[0] = journal_call("seed"); calls[0].turn_key = "executor:0:0";
      calls[0].legacy_turn_key = "turn";
      check_equal(journal_execute(executor, runtime, registry, calls, 1),
          legacy_errors[i] == -1 ? SALTS_EIO : legacy_errors[i]);
      check_equal(store.calls, 1); check_equal(store.writes, 0);
    }
  }

  it("replays an existing scoped journal without consulting ambiguous legacy identity") {
    calls[0] = journal_call("new-scope"); calls[0].turn_key = "executor:0:0";
    calls[0].legacy_turn_key = "turn";
    check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_OK);
    check_equal(store.calls, 2);
    calls[1] = journal_call("new-scope");
    check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
    check_equal(store.calls, 3);
    free(calls[1].output); json_free(calls[1].detail);
    store.writes = 0;
    calls[1] = journal_call("new-scope"); calls[1].turn_key = "executor:0:0";
    calls[1].legacy_turn_key = "turn";
    check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
    check_true(calls[1].replayed); check_equal(calls[1].output, journal_output);
    check_equal(store.calls, 3); check_equal(store.writes, 0);
  }

  it("projects an executor-produced journal without invoking or publishing again") {
    static const char head[] = "{\"schema_version\":1,\"id\":\"thread\",\"thread_id\":\"thread\","
        "\"run_snapshot\":{\"id\":\"run\",\"thread_id\":\"thread\",\"status\":\"completed\","
        "\"state_snapshot\":{},\"latest_checkpoint_id\":null}}";
    const char *ids[1];
    int writes, lists;
    ids[0] = id;
    json_free(record); record = NULL;
    record = json_parse(head, sizeof(head) - 1); check_not_null(record);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "thread_heads", "thread", record), 0);
    writes = store.writes; lists = store.lists;
    check_equal(turbo_agent_thread_tool_diagram(runtime, "thread", ids, 1, &value), SALTS_OK);
    check_equal(store.calls, 1); check_equal(store.writes, writes); check_equal(store.lists, lists);
    check_equal(json_get_string(json_array_get(json_object_get(value, "nodes"), 1), "call_id"), "seed");
    check_equal(json_get_string(json_array_get(json_object_get(value, "nodes"), 1), "phase"), "committed");
    expected = json_serialize(value, NULL); check_not_null(expected);
    check_null(strstr(expected, journal_output)); check_null(strstr(expected, "retained detail"));
  }

  const struct {
    const char *name, *field, *json;
  } corrupt[] = {{"null status", "status", "null"},
                 {"string status", "status", "\"0\""},
                 {"fractional status", "status", "0.5"},
                 {"out-of-range status", "status", "1e100"},
                 {"unknown positive status", "status", "1"},
                 {"unknown negative status", "status", "-99"},
                 {"object output", "output", "{}"},
                 {"null output", "output", "null"},
                 {"embedded NUL output", "output", "\"saved\\u0000suffix\""},
                 {"embedded NUL identity", "thread_id", "\"thread\\u0000suffix\""},
                 {"embedded NUL phase", "phase", "\"committed\\u0000suffix\""},
                 {"embedded NUL call ID", "call_id", "\"seed\\u0000suffix\""},
                 {"changed embedded journal ID", "journal_id", "\"different\""},
                 {"null embedded journal ID", "journal_id", "null"},
                 {"fractional version", "schema_version", "1.5"},
                 {"changed policy", "execution_mode", "2"}};
  for (size_t i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); ++i) {
    it("rejects %s before any callback in the batch", corrupt[i].name) {
      value = json_parse(corrupt[i].json, strlen(corrupt[i].json));
      check_not_null(value);
      check_equal(turbo_runtime_json_object_set(record, corrupt[i].field, value), 0);
      value = NULL;
      expected = json_serialize(record, NULL);
      check_not_null(expected);
      check_equal(turbo_agent_runtime_store_put_json(runtime, journal_collection, id, record), 0);
      calls[0] = journal_call("fresh");
      calls[1] = journal_call("seed");
      check_equal(journal_execute(executor, runtime, registry, calls, 2), SALTS_EPROTO);
      check_equal(store.calls, 1);
      check_equal(turbo_agent_runtime_store_get_json(runtime, journal_collection, id, &stored), 0);
      actual = json_serialize(stored, NULL);
      check_not_null(actual);
      check_equal(actual, expected);
    }
  }

  it("admits a confirmed missing record without consulting a failing list") {
    store.fail_list = 1;
    store.lists = 0;
    calls[0] = journal_call("new-call");
    check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_OK);
    check_equal(calls[0].status, TURBO_TOOL_OK);
    check_equal(store.calls, 2);
    check_equal(store.lists, 0);
  }

  it("propagates a typed read failure before writing or invoking tools") {
    store.get_error = SALTS_EIO;
    store.lists = 0;
    calls[0] = journal_call("new-call");
    check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_EIO);
    check_equal(store.calls, 1);
    check_equal(store.writes, 0);
    check_equal(store.lists, 0);
  }

  for (int fail_list = 0; fail_list < 2; ++fail_list) {
    it("rejects failed record reads with %s listing", fail_list ? "failed" : "nonempty") {
      store.fail_get = 1;
      store.fail_list = fail_list;
      calls[0] = journal_call("seed");
      check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_EIO);
      check_equal(store.calls, 1);
      check_equal(store.writes, 0);
      store.fail_get = store.fail_list = 0;
      calls[1] = journal_call("seed");
      check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
      check_true(calls[1].replayed);
      check_equal(calls[1].output, journal_output);
      check_equal(store.calls, 1);
      check_equal(store.writes, 0);
    }
  }

  for (int status = TURBO_TOOL_TRAPPED; status <= TURBO_TOOL_OK; ++status) {
    it("replays valid status %d with independent detail and no new write", status) {
      check_equal(turbo_runtime_json_object_set_int64(record, "status", status), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, journal_collection, id, record), 0);
      store.writes = 0;
      calls[0] = journal_call("seed");
      check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_OK);
      check_equal(calls[0].status, status);
      check_true(calls[0].replayed);
      check_equal(calls[0].output, journal_output);
      check_equal(json_get_string(calls[0].detail, "host"), "retained detail");
      check_equal(store.calls, 1);
      check_equal(store.writes, 0);
      check_equal(turbo_runtime_json_object_set_string(calls[0].detail, "host", "changed"), 0);
      check_equal(turbo_agent_runtime_store_get_json(runtime, journal_collection, id, &stored), 0);
      check_equal(json_get_string(json_object_get(stored, "detail"), "host"), "retained detail");
    }
  }

  it("enforces a reduced replay output cap without losing the persisted result") {
    turbo_agent_tool_executor_config_t config;
    turbo_agent_tool_executor_config_init(&config);
    config.max_output_bytes = sizeof(journal_output) - 2;
    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    check_equal(turbo_agent_tool_executor_create(&config, &executor), SALTS_OK);
    calls[0] = journal_call("seed");
    check_equal(journal_execute(executor, runtime, registry, calls, 1), SALTS_OK);
    check_equal(calls[0].status, TURBO_TOOL_OUTPUT_LIMIT);
    check_null(calls[0].output);
    check_true(calls[0].replayed);
    check_equal(json_get_string(calls[0].detail, "host"), "retained detail");
    config.max_output_bytes = sizeof(journal_output) - 1;
    turbo_agent_tool_executor_destroy(executor);
    executor = NULL;
    check_equal(turbo_agent_tool_executor_create(&config, &executor), SALTS_OK);
    calls[1] = journal_call("seed");
    check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
    check_equal(calls[1].status, TURBO_TOOL_OK);
    check_equal(calls[1].output, journal_output);
    check_true(calls[1].replayed);
    check_equal(store.calls, 1);
    check_equal(store.writes, 0);
  }

  const char *phases[] = {"planned", "started", "committed"};
  for (size_t i = 0; i < 3; ++i) {
    it("preserves the invocation boundary when %s persistence fails", phases[i]) {
      store.fail_phase = phases[i];
      calls[0] = journal_call("fault");
      check_equal(journal_execute(executor, runtime, registry, calls, 1),
                  i == 0 ? SALTS_EIO : SALTS_OK);
      check_equal(store.failures, 1);
      check_equal(store.calls, i == 2 ? 2 : 1);
      if (i != 0)
        check_equal(calls[0].status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      if (i == 1) check_true(calls[0].replayed);
      check_null(calls[0].output);
      store.fail_phase = NULL;
      calls[1] = journal_call("fault");
      check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
      check_equal(store.calls, i == 1 ? 1 : 2);
      check_equal(calls[1].status,
                  i == 0 ? TURBO_TOOL_OK : TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_equal(calls[1].replayed, i != 0);
    }
  }
}
