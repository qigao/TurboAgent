#include "../src/turbo_agent_tool_executor_internal.h"
#include "tinytest.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static const char identity_collection[] = "agent_tool_journal";
enum { IDENTITY_GATE_TIMEOUT_MS = 5000, IDENTITY_WAITER_OBSERVATION_MS = 50 };

typedef struct identity_probe_s {
  turbo_agent_runtime_store_t inner;
  atomic_int callbacks;
  atomic_int journal_writes;
  atomic_int hold_callback;
  atomic_int release_callback;
  atomic_int gate_timed_out;
  atomic_int fail_commit_once;
} identity_probe_t;

static int identity_wait(atomic_int *flag, int expected, uint64_t timeout_ms) {
  uint64_t start = cmeta_monotonic_ms();
  do {
    if (atomic_load(flag) == expected) return 1;
    cmeta_thread_yield();
  } while (cmeta_monotonic_ms() - start < timeout_ms);
  return atomic_load(flag) == expected;
}

static int identity_put(void *user, const char *collection, const char *id, const char *text) {
  identity_probe_t *probe = user;
  if (strcmp(collection, identity_collection) == 0) {
    atomic_fetch_add_explicit(&probe->journal_writes, 1, memory_order_relaxed);
    if (strstr(text, "\"committed\"") && atomic_exchange(&probe->fail_commit_once, 0))
      return SALTS_EIO;
  }
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

static int identity_create(void *user, const char *collection, const char *id,
                           const char *text) {
  identity_probe_t *probe = user;
  if (!probe->inner.create_if_absent) return SALTS_ENOTSUP;
  if (!strcmp(collection, identity_collection))
    atomic_fetch_add_explicit(&probe->journal_writes, 1, memory_order_relaxed);
  return probe->inner.create_if_absent(probe->inner.user_data, collection, id, text);
}

static turbo_tool_status_t identity_invoke(const char *arguments,
    const turbo_tool_execution_context_t *context, char **out, void *user) {
  identity_probe_t *probe = user;
  size_t bytes = strlen(arguments) + 1;
  (void)context;
  /* Publish callback entry after the preceding planned/started writes, so the
   * observing caller has an explicit happens-before edge for its gate checks. */
  atomic_fetch_add_explicit(&probe->callbacks, 1, memory_order_release);
  if (atomic_load(&probe->hold_callback) &&
      !identity_wait(&probe->release_callback, 1, IDENTITY_GATE_TIMEOUT_MS)) {
    atomic_store(&probe->gate_timed_out, 1);
    return TURBO_TOOL_ERROR;
  }
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

typedef struct identity_caller_s {
  turbo_agent_tool_executor_t *executor;
  turbo_agent_runtime_t *runtime;
  const turbo_tool_registry_t *registry;
  const turbo_agent_policy_t *policy;
  turbo_agent_tool_execution_t *call;
  const turbo_cancel_token_t *token;
  atomic_int entered;
  atomic_int done;
  int result;
} identity_caller_t;

static void identity_execute(void *user) {
  identity_caller_t *caller = user;
  atomic_store(&caller->entered, 1);
  caller->result = turbo_agent_tool_executor_execute(caller->executor, caller->runtime,
      caller->token, "thread", "run", NULL, NULL, caller->registry, caller->policy,
      caller->call, 1);
  atomic_store(&caller->done, 1);
}

spec("ToolExecutor effect identity admission and replay") {
  static identity_probe_t probe;
  static turbo_tool_registry_t *registry;
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_tool_executor_t *executor;
  static turbo_agent_tool_execution_t calls[3], replay;
  static turbo_agent_policy_t policy, denied_policy;
  static json_value_t *records;
  static cmeta_thread_t threads[2];
  static identity_caller_t callers[2];
  static int started;
  static turbo_cancel_source_t *source;
  static turbo_cancel_token_t *token;

  before_each() {
    turbo_agent_runtime_store_t callbacks = {0};
    turbo_tool_definition_v4_t definition = {0};
    atomic_init(&probe.callbacks, 0);
    atomic_init(&probe.journal_writes, 0);
    atomic_init(&probe.hold_callback, 0);
    atomic_init(&probe.release_callback, 0);
    atomic_init(&probe.gate_timed_out, 0);
    atomic_init(&probe.fail_commit_once, 0);
    started = 0;
    for (size_t i = 0; i < 2; ++i) {
      threads[i] = NULL;
      callers[i] = (identity_caller_t){0};
      atomic_init(&callers[i].entered, 0);
      atomic_init(&callers[i].done, 0);
    }
    probe.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(probe.inner.put);
    callbacks.put = identity_put;
    callbacks.get = identity_get;
    callbacks.list = identity_list;
    callbacks.create_if_absent = identity_create;
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
    denied_policy = turbo_agent_policy_default();
  }

  after_each() {
    /* TinyTest assertions may exit the body while a callback is held. Drain
     * both callers before releasing any borrowed calls, policy or registry. */
    atomic_store(&probe.release_callback, 1);
    for (int i = 0; i < started; ++i) cmeta_thread_join(&threads[i]);
    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    token = NULL;
    source = NULL;
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
    const char *outcomes[] = {"committed replay", "canonical alias replay", "changed arguments",
        "changed tool", "failed commit", "cancelled replay waiter", "cancelled unknown waiter",
        "cancelled distinct call", "denied replay waiter", "deadline distinct call"};
    for (size_t variant = 0; variant < sizeof(outcomes) / sizeof(outcomes[0]); ++variant) {
      it("serializes two callers on one owned executor for %s (%s)",
          outcomes[variant], parallel ? "parallel" : "sequential") {
        const int unknown = variant == 4 || variant == 6;
        const int conflict = variant == 2 || variant == 3;
        const int deadline = variant == 9;
        const int cancelled = (variant >= 5 && variant <= 7) || deadline;
        const int distinct = variant == 7 || deadline;
        const int denied = variant == 8;
        calls[0] = identity_call("turn", "shared");
        calls[1] = identity_call("turn", distinct ? "distinct" : "shared");
        if (parallel) calls[0].tool_name = calls[1].tool_name = "identity.parallel";
        if (variant == 1) calls[1].tool_name = parallel ? "identity_parallel" : "identity_probe";
        if (variant == 2) calls[1].arguments_json = "{\"changed\":true}";
        if (variant == 3) calls[1].tool_name = parallel ? "identity.probe" : "identity.parallel";
        atomic_store(&probe.hold_callback, 1);
        atomic_store(&probe.fail_commit_once, unknown);
        if (cancelled) {
          if (deadline) {
            turbo_cancel_source_config_t cancel_config = {0};
            cancel_config.struct_size = sizeof(cancel_config);
            cancel_config.abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION;
            cancel_config.deadline_mono_ms = cmeta_monotonic_ms() + 100;
            check_equal(turbo_cancel_source_create(&cancel_config, &source), SALTS_OK);
          } else {
            check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
          }
          check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
        }
        /* Freeze registry/policy and keep independent call buffers alive until
         * both callers join. This exercises the executor owner gate only; it
         * does not exercise concurrent Agent/Router or policy mutation. */
        for (size_t i = 0; i < 2; ++i) {
          callers[i].executor = executor;
          callers[i].runtime = runtime;
          callers[i].registry = registry;
          callers[i].policy = &policy;
          callers[i].call = &calls[i];
        }
        callers[1].token = token;
        if (denied) callers[1].policy = &denied_policy;
        check_equal(cmeta_thread_create(&threads[0], identity_execute, &callers[0]), SALTS_OK);
        ++started;
        check_true(identity_wait(&probe.callbacks, 1, IDENTITY_GATE_TIMEOUT_MS));
        check_equal(cmeta_thread_create(&threads[1], identity_execute, &callers[1]), SALTS_OK);
        ++started;
        check_true(identity_wait(&callers[1].entered, 1, IDENTITY_GATE_TIMEOUT_MS));
        if (cancelled && !deadline)
          check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
        /* A cancellable waiter must terminate before the owner is released.
         * Other callers remain serialized until the first callback drains. */
        if (cancelled)
          check_true(identity_wait(&callers[1].done, 1, IDENTITY_GATE_TIMEOUT_MS));
        else
          check_false(identity_wait(&callers[1].done, 1,
                                    IDENTITY_WAITER_OBSERVATION_MS));
        check_equal(atomic_load(&probe.callbacks), 1);
        check_equal(atomic_load(&probe.journal_writes), 2);
        atomic_store(&probe.release_callback, 1);
        for (int i = 0; i < started; ++i)
          check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
        started = 0;
        check_equal(atomic_load(&probe.gate_timed_out), 0);
        check_equal(callers[0].result, SALTS_OK);
        check_equal(callers[1].result, conflict ? SALTS_EPROTO : SALTS_OK);
        check_equal(calls[0].status, unknown ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK);
        check_false(calls[0].replayed);
        if (unknown) check_null(calls[0].output);
        else check_equal(calls[0].output, "{}");
        if (cancelled) {
          check_false(calls[1].replayed);
          check_equal(calls[1].status, deadline ? TURBO_TOOL_DEADLINE_EXCEEDED
                                                : TURBO_TOOL_CANCELLED);
          check_null(calls[1].output);
        } else if (conflict || distinct || denied) {
          check_equal(calls[1].replayed, denied);
          check_null(calls[1].output);
          if (denied) {
            check_equal(calls[1].status, TURBO_TOOL_ERROR);
            check_equal(calls[1].policy_reason, "custom_tools_disabled");
          }
        } else {
          check_true(calls[1].replayed);
          check_equal(calls[1].status, unknown ? TURBO_TOOL_UNKNOWN_SIDE_EFFECT : TURBO_TOOL_OK);
          if (unknown) check_null(calls[1].output);
          else {
            check_equal(calls[1].output, "{}");
            check_true(calls[1].output != calls[0].output);
          }
        }
        check_equal(atomic_load(&probe.callbacks), 1);
        check_equal(atomic_load(&probe.journal_writes),
                    cancelled ? 3 : (distinct ? 4 : 3));
        check_equal(turbo_agent_runtime_store_list_json(runtime, identity_collection,
            NULL, NULL, &records), SALTS_OK);
        check_equal(json_array_size(records),
                    cancelled ? 1 : (distinct ? 2 : 1));
        for (size_t i = 0; i < json_array_size(records); ++i) {
          const json_value_t *record = json_array_get(records, i);
          if (strcmp(json_get_string(record, "call_id"), "shared") == 0) {
            check_equal(json_get_string(record, "phase"), unknown ? "started" : "committed");
            check_equal(json_get_string(record, "tool_name"), calls[0].tool_name);
            if (unknown) check_null(json_object_get(record, "output"));
            else check_equal(json_get_string(record, "output"), "{}");
          } else {
            check_equal(json_get_string(record, "call_id"), "distinct");
            check_equal(json_get_string(record, "phase"), "planned");
          }
        }
      }
    }
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
