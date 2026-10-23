#include "../src/turbo_agent_tool_executor_internal.h"
#include "tinytest.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char identity_collection[] = "agent_tool_journal";

typedef struct identity_probe_s {
  turbo_agent_runtime_store_t inner;
  atomic_int callbacks;
  atomic_int journal_writes;
} identity_probe_t;

static int identity_put(void *user, const char *collection, const char *id, const char *text) {
  identity_probe_t *probe = user;
  if (strcmp(collection, identity_collection) == 0)
    atomic_fetch_add_explicit(&probe->journal_writes, 1, memory_order_relaxed);
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int identity_get(void *user, const char *collection, const char *id, char **out) {
  identity_probe_t *probe = user;
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int identity_list(void *user, const char *collection, const char *key,
                         const char *value, char **out) {
  identity_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

static turbo_tool_status_t identity_invoke(const char *arguments,
    const turbo_tool_execution_context_t *context, char **out, void *user) {
  identity_probe_t *probe = user;
  size_t bytes = strlen(arguments) + 1;
  (void)context;
  atomic_fetch_add_explicit(&probe->callbacks, 1, memory_order_relaxed);
  *out = malloc(bytes);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, arguments, bytes);
  return TURBO_TOOL_OK;
}

static turbo_agent_tool_execution_t identity_call(const char *turn, const char *id) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = id;
  call.turn_key = turn;
  call.tool_name = "identity.probe";
  call.arguments_json = "{}";
  return call;
}

spec("ToolExecutor batch effect identity admission") {
  static identity_probe_t probe;
  static turbo_tool_registry_t *registry;
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t calls[3], replay;
  static turbo_agent_policy_t policy;
  static json_value_t *records;

  before_each() {
    turbo_agent_runtime_store_t callbacks = {0};
    turbo_tool_definition_v4_t definition = {0};
    atomic_init(&probe.callbacks, 0);
    atomic_init(&probe.journal_writes, 0);
    probe.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(probe.inner.put);
    callbacks.put = identity_put;
    callbacks.get = identity_get;
    callbacks.list = identity_list;
    callbacks.user_data = &probe;
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "identity.probe";
    definition.definition.description = "Identity admission probe";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &probe;
    definition.context_handler = identity_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    definition.definition.name = "identity.parallel";
    definition.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    policy = turbo_agent_policy_default();
    policy.allow_custom_tools = 1;
  }

  after_each() {
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
    if (probe.inner.user_data_free) probe.inner.user_data_free(probe.inner.user_data);
    probe.inner = (turbo_agent_runtime_store_t){0};
    for (size_t i = 0; i < 3; ++i) {
      free(calls[i].output);
      json_free(calls[i].detail);
      calls[i] = (turbo_agent_tool_execution_t){0};
    }
    free(replay.output);
    json_free(replay.detail);
    replay = (turbo_agent_tool_execution_t){0};
    json_free(records);
    records = NULL;
    registry = NULL;
    runtime = NULL;
    executor = NULL;
  }

  for (int parallel = 0; parallel < 2; ++parallel) {
    for (int durable = 0; durable < 2; ++durable) {
      const char *variants[] = {"identical calls", "changed arguments", "caller-marked replay",
                                 "denied call", "canonical alias", "different registered tool"};
      for (size_t variant = 0; variant < sizeof(variants) / sizeof(variants[0]); ++variant) {
        it("rejects duplicate turn/call identity with %s (%s, %s) before any effect or Journal write",
            variants[variant], durable ? "durable" : "stateless", parallel ? "parallel" : "sequential") {
          calls[0] = identity_call("turn", "unrelated-prefix");
          calls[1] = identity_call("turn", "duplicate");
          calls[2] = identity_call("turn", "duplicate");
          if (parallel) {
            for (size_t c = 0; c < 3; ++c) calls[c].tool_name = "identity.parallel";
          }
          if (variant == 1) calls[2].arguments_json = "{\"changed\":true}";
          if (variant == 2) calls[2].replayed = 1;
          if (variant == 3) calls[2].tool_name = "not-registered";
          if (variant == 4) calls[2].tool_name = parallel ? "identity_parallel" : "identity_probe";
          if (variant == 5) calls[2].tool_name = parallel ? "identity.probe" : "identity.parallel";
          check_equal(turbo_agent_tool_executor_execute(executor, durable ? runtime : NULL,
              NULL, "thread", "run", NULL, NULL, registry, &policy, calls, 3), SALTS_EPROTO);
          check_equal(atomic_load(&probe.callbacks), 0);
          check_equal(atomic_load(&probe.journal_writes), 0);
          check_equal(turbo_agent_runtime_store_list_json(runtime, identity_collection,
              NULL, NULL, &records), 0);
          check_equal(json_array_size(records), 0);
        }
      }
    }
  }

  for (int parallel = 0; parallel < 2; ++parallel) {
    const char *distinct[] = {"different turn", "different call", "call suffix", "turn suffix"};
    for (size_t variant = 0; variant < sizeof(distinct) / sizeof(distinct[0]); ++variant) {
      it("keeps independent identities and exact replay distinct for %s (%s)",
          distinct[variant], parallel ? "parallel" : "sequential") {
        calls[0] = identity_call("turn", "one");
        calls[1] = identity_call("turn", "one");
        if (parallel) calls[0].tool_name = calls[1].tool_name = "identity.parallel";
        if (variant == 0) calls[1].turn_key = "other-turn";
        if (variant == 1) calls[1].call_id = "two";
        if (variant == 2) calls[1].call_id = "one-more";
        if (variant == 3) calls[1].turn_key = "turn-more";
        check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
            "thread", "run", NULL, NULL, registry, &policy, calls, 2), SALTS_OK);
        check_equal(atomic_load(&probe.callbacks), 2);
        check_equal(atomic_load(&probe.journal_writes), 6);
        check_equal(turbo_agent_runtime_store_list_json(runtime, identity_collection,
            NULL, NULL, &records), 0);
        check_equal(json_array_size(records), 2);
        replay = identity_call(calls[1].turn_key, calls[1].call_id);
        replay.tool_name = calls[1].tool_name;
        check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
            "thread", "run", NULL, NULL, registry, &policy, &replay, 1), SALTS_OK);
        check_true(replay.replayed);
        check_equal(replay.output, "{}");
        check_equal(atomic_load(&probe.callbacks), 2);
        check_equal(atomic_load(&probe.journal_writes), 6);
      }
    }
  }
}
