#include "../src/turbo_agent_tool_executor_internal.h"
#include "tinytest.h"
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

static const char journal_output[] = "saved result";
static const char journal_collection[] = "agent_tool_journal";

typedef struct journal_store_s {
  turbo_agent_runtime_store_t inner;
  const char *fail_phase;
  int fail_get, fail_list;
  int writes, failures, calls;
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

static int journal_get(void *user, const char *collection, const char *id, char **out) {
  journal_store_t *store = user;
  if (store->fail_get) return -1;
  return store->inner.get(store->inner.user_data, collection, id, out);
}

static int journal_list(void *user, const char *collection, const char *key, const char *value,
                        char **out) {
  journal_store_t *store = user;
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
  return turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run", NULL, NULL,
                                           registry, NULL, calls, count);
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
    callbacks = (turbo_agent_runtime_store_t){journal_put, journal_get, journal_list, &store,
                                              journal_destroy};
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
        check_equal(calls[0].status, i == 1 ? TURBO_TOOL_ERROR : TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_null(calls[0].output);
      store.fail_phase = NULL;
      calls[1] = journal_call("fault");
      check_equal(journal_execute(executor, runtime, registry, calls + 1, 1), SALTS_OK);
      check_equal(store.calls, 2);
      check_equal(calls[1].status, i == 2 ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK);
      check_equal(calls[1].replayed, i == 2 ? 1 : 0);
    }
  }
}
