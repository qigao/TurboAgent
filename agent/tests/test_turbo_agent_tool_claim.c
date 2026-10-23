#include "../src/turbo_agent_tool_executor_internal.h"
#include "../src/turbo_agent_tool_journal_internal.h"
#include "tinytest.h"
#include "turbo_agent_policy.h"
#include "turbo_runtime_store.h"

#include <salts/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct tool_claim_store_s {
  turbo_agent_runtime_store_t inner;
  cmeta_mutex_t mutex;
  atomic_int callbacks;
  atomic_int gate_journal_reads;
  atomic_int journal_reads;
  atomic_int fail_batch_first_commit_once;
} tool_claim_store_t;

static int tool_claim_put(void *user, const char *collection, const char *id,
                          const char *record) {
  tool_claim_store_t *store = user;
  json_value_t *value = NULL;
  int rc;
  if (!strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION)) {
    const char *phase;
    const char *call_id;
    value = json_parse(record, strlen(record));
    phase = value ? json_get_string(value, "phase") : NULL;
    call_id = value ? json_get_string(value, "call_id") : NULL;
    if (phase && call_id && !strcmp(phase, "committed") &&
        !strcmp(call_id, "batch-first") &&
        atomic_exchange_explicit(&store->fail_batch_first_commit_once, 0,
                                 memory_order_acq_rel)) {
      json_free(value);
      return SALTS_EIO;
    }
    json_free(value);
  }
  cmeta_mutex_lock(&store->mutex);
  rc = store->inner.put(store->inner.user_data, collection, id, record);
  cmeta_mutex_unlock(&store->mutex);
  return rc;
}

static int tool_claim_get(void *user, const char *collection, const char *id,
                          char **out_record) {
  tool_claim_store_t *store = user;
  int read_index = -1;
  int rc;
  cmeta_mutex_lock(&store->mutex);
  rc = store->inner.get(store->inner.user_data, collection, id, out_record);
  cmeta_mutex_unlock(&store->mutex);
  if ((rc == SALTS_OK || rc == SALTS_ENOENT) &&
      !strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION) &&
      atomic_load_explicit(&store->gate_journal_reads, memory_order_acquire)) {
    read_index = atomic_fetch_add_explicit(&store->journal_reads, 1, memory_order_acq_rel);
    if (read_index < 2) {
      while (atomic_load_explicit(&store->journal_reads, memory_order_acquire) < 2)
        cmeta_thread_yield();
    }
  }
  return rc;
}

static int tool_claim_list(void *user, const char *collection, const char *key,
                           const char *value, char **out_records) {
  tool_claim_store_t *store = user;
  int rc;
  cmeta_mutex_lock(&store->mutex);
  rc = store->inner.list(store->inner.user_data, collection, key, value, out_records);
  cmeta_mutex_unlock(&store->mutex);
  return rc;
}

static int tool_claim_create(void *user, const char *collection, const char *id,
                             const char *record) {
  tool_claim_store_t *store = user;
  int rc;
  cmeta_mutex_lock(&store->mutex);
  rc = store->inner.create_if_absent(store->inner.user_data, collection, id, record);
  cmeta_mutex_unlock(&store->mutex);
  return rc;
}

static turbo_tool_status_t tool_claim_invoke(
    const char *arguments, const turbo_tool_execution_context_t *context, char **out,
    void *user) {
  tool_claim_store_t *store = user;
  (void)arguments;
  (void)context;
  atomic_fetch_add_explicit(&store->callbacks, 1, memory_order_relaxed);
  *out = malloc(3);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, "ok", 3);
  return TURBO_TOOL_OK;
}

static turbo_agent_tool_execution_t tool_claim_call(const char *id, const char *arguments) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = id;
  call.turn_key = "turn";
  call.tool_name = "claim.probe";
  call.arguments_json = arguments;
  return call;
}

typedef struct tool_claim_caller_s {
  turbo_agent_tool_executor_t *executor;
  turbo_agent_runtime_t *runtime;
  const turbo_tool_registry_t *registry;
  const turbo_agent_policy_t *policy;
  turbo_agent_tool_execution_t call;
  int rc;
} tool_claim_caller_t;

static void tool_claim_execute(void *user) {
  tool_claim_caller_t *caller = user;
  caller->rc = turbo_agent_tool_executor_execute(
      caller->executor, caller->runtime, NULL, "thread", "run", NULL, NULL,
      caller->registry, caller->policy, &caller->call, 1);
}

spec("Tool Journal atomic backend-entry admission") {
  it("fences independent owners and replays interrupted distinct-identity batches") {
    tool_claim_store_t store = {0};
    turbo_agent_runtime_store_t callbacks = {0};
    turbo_agent_runtime_t *runtimes[2] = {0};
    turbo_agent_tool_executor_t *executors[2] = {0};
    turbo_tool_registry_t *registry = NULL;
    turbo_tool_definition_v4_t definition = {0};
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_agent_tool_execution_t seed = tool_claim_call("shared", "{}");
    tool_claim_caller_t callers[2] = {0};
    cmeta_thread_t threads[2] = {0};
    json_value_t *records = NULL;
    int joined = 0;

    store.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(store.inner.put);
    check_not_null(store.inner.create_if_absent);
    cmeta_mutex_init(&store.mutex);
    atomic_init(&store.callbacks, 0);
    atomic_init(&store.gate_journal_reads, 0);
    atomic_init(&store.journal_reads, 0);
    atomic_init(&store.fail_batch_first_commit_once, 0);
    callbacks.put = tool_claim_put;
    callbacks.get = tool_claim_get;
    callbacks.list = tool_claim_list;
    callbacks.user_data = &store;
    callbacks.create_if_absent = tool_claim_create;
    for (size_t i = 0; i < 2; ++i) {
      runtimes[i] = turbo_agent_runtime_create(&callbacks);
      check_not_null(runtimes[i]);
      check_equal(turbo_agent_tool_executor_create(NULL, &executors[i]), SALTS_OK);
    }
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "claim.probe";
    definition.definition.description = "Independent executor admission probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &store;
    definition.context_handler = tool_claim_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    policy.allow_custom_tools = 1;

    /* Cancellation after durable planning leaves a recoverable planned record
     * without admitting the backend effect. */
    check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_agent_tool_executor_execute(
                    executors[0], runtimes[0], token, "thread", "run", NULL, NULL,
                    registry, &policy, &seed, 1),
                SALTS_OK);
    check_equal(seed.status, TURBO_TOOL_CANCELLED);
    check_equal(atomic_load(&store.callbacks), 0);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtimes[0], TURBO_AGENT_TOOL_JOURNAL_COLLECTION, NULL, NULL,
                    &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "planned");
    json_free(records);
    records = NULL;

    atomic_store_explicit(&store.gate_journal_reads, 1, memory_order_release);
    for (size_t i = 0; i < 2; ++i) {
      callers[i].executor = executors[i];
      callers[i].runtime = runtimes[i];
      callers[i].registry = registry;
      callers[i].policy = &policy;
      callers[i].call = tool_claim_call("shared", "{}");
      check_equal(cmeta_thread_create(&threads[i], tool_claim_execute, &callers[i]), SALTS_OK);
    }
    for (size_t i = 0; i < 2; ++i) {
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
      ++joined;
    }
    check_equal(joined, 2);
    check_equal(callers[0].rc, SALTS_OK);
    check_equal(callers[1].rc, SALTS_OK);
    check_equal(atomic_load(&store.callbacks), 1);
    check_equal(callers[0].call.replayed + callers[1].call.replayed, 1);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtimes[0], TURBO_AGENT_TOOL_JOURNAL_COLLECTION, NULL, NULL,
                    &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtimes[0], TURBO_AGENT_TOOL_CLAIM_COLLECTION, NULL, NULL,
                    &records),
                SALTS_OK);
    check_equal(json_array_size(records), 1);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "admitted");
    json_free(records);
    records = NULL;

    /* A fresh same-key race with different canonical arguments must resolve at
     * atomic planning: one identity wins, the conflicting caller fails before
     * callback entry, and the winner's committed record cannot be overwritten. */
    for (size_t i = 0; i < 2; ++i) {
      free(callers[i].call.output);
      json_free(callers[i].call.detail);
      callers[i].call = tool_claim_call(
          "fresh", i == 0 ? "{\"owner\":0}" : "{\"owner\":1}");
      callers[i].rc = SALTS_OK;
      threads[i] = NULL;
    }
    atomic_store_explicit(&store.journal_reads, 0, memory_order_release);
    for (size_t i = 0; i < 2; ++i)
      check_equal(cmeta_thread_create(&threads[i], tool_claim_execute, &callers[i]), SALTS_OK);
    for (size_t i = 0; i < 2; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    check_equal((callers[0].rc == SALTS_OK) + (callers[1].rc == SALTS_OK), 1);
    check_equal((callers[0].rc == SALTS_EPROTO) + (callers[1].rc == SALTS_EPROTO), 1);
    check_equal(atomic_load(&store.callbacks), 2);
    check_equal(turbo_agent_runtime_store_list_json(
                    runtimes[0], TURBO_AGENT_TOOL_JOURNAL_COLLECTION, NULL, NULL,
                    &records),
                SALTS_OK);
    check_equal(json_array_size(records), 2);
    for (size_t i = 0; i < json_array_size(records); ++i)
      check_equal(json_get_string(json_array_get(records, i), "phase"), "committed");
    json_free(records);
    records = NULL;
    check_equal(turbo_agent_runtime_store_list_json(
                    runtimes[0], TURBO_AGENT_TOOL_CLAIM_COLLECTION, NULL, NULL,
                    &records),
                SALTS_OK);
    check_equal(json_array_size(records), 2);

    json_free(records);
    records = NULL;

    /* A failed terminal commit makes only that identity unknown. The next
     * already-planned sequential identity may finish, and replaying the whole
     * interrupted batch must recover both records without another callback. */
    {
      turbo_agent_tool_execution_t batch[2] = {
          tool_claim_call("batch-first", "{}"),
          tool_claim_call("batch-second", "{}")};
      turbo_agent_tool_execution_t replay[2] = {
          tool_claim_call("batch-first", "{}"),
          tool_claim_call("batch-second", "{}")};
      atomic_store_explicit(&store.fail_batch_first_commit_once, 1,
                            memory_order_release);
      check_equal(turbo_agent_tool_executor_execute(
                      executors[0], runtimes[0], NULL, "thread", "run", NULL,
                      NULL, registry, &policy, batch, 2),
                  SALTS_OK);
      check_equal(atomic_load(&store.callbacks), 4);
      check_equal(batch[0].status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_false(batch[0].replayed);
      check_equal(batch[1].status, TURBO_TOOL_OK);
      check_false(batch[1].replayed);
      check_equal(turbo_agent_tool_executor_execute(
                      executors[1], runtimes[1], NULL, "thread", "run", NULL,
                      NULL, registry, &policy, replay, 2),
                  SALTS_OK);
      check_equal(atomic_load(&store.callbacks), 4);
      check_equal(replay[0].status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_true(replay[0].replayed);
      check_null(replay[0].output);
      check_equal(replay[1].status, TURBO_TOOL_OK);
      check_true(replay[1].replayed);
      check_equal(replay[1].output, "ok");
      free(batch[0].output);
      free(batch[1].output);
      free(replay[0].output);
      free(replay[1].output);
      json_free(batch[0].detail);
      json_free(batch[1].detail);
      json_free(replay[0].detail);
      json_free(replay[1].detail);
    }

    free(seed.output);
    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    for (size_t i = 0; i < 2; ++i) {
      free(callers[i].call.output);
      json_free(callers[i].call.detail);
      turbo_agent_tool_executor_destroy(executors[i]);
      turbo_agent_runtime_destroy(runtimes[i]);
    }
    turbo_tool_registry_destroy(registry);
    cmeta_mutex_destroy(&store.mutex);
    store.inner.user_data_free(store.inner.user_data);
  }
}
