#include "../src/turbo_agent_tool_executor_internal.h"
#include "../src/turbo_agent_tool_journal_internal.h"
#include "tinytest.h"
#include "turbo_runtime_store.h"

#include <stdlib.h>
#include <string.h>

/* The cancel source is triggered only *after* an actual atomic Store write.
 * No sleeps, racing scheduler or production fallback is involved. */
typedef struct cancel_entry_fixture_s {
  turbo_agent_runtime_store_t inner;
  turbo_cancel_source_t *source;
  int cancel_after; /* 1=ADMITTED claim, 2=STARTED Journal */
  int triggered;
  int cancel_rc;
  int callbacks;
} cancel_entry_fixture_t;

static int cancel_entry_record_is(const char *payload, const char *phase) {
  json_value_t *record = payload ? json_parse(payload, strlen(payload)) : NULL;
  const char *actual_phase = json_get_string(record, "phase");
  const char *call_id = json_get_string(record, "call_id");
  int match = actual_phase && call_id &&
      !strcmp(actual_phase, phase) && !strcmp(call_id, "cancelled-call");
  json_free(record);
  return match;
}

static void cancel_entry_at_boundary(cancel_entry_fixture_t *fixture) {
  if (fixture->triggered) return;
  ++fixture->triggered;
  fixture->cancel_rc = turbo_cancel_source_cancel(fixture->source, TURBO_CANCEL_USER);
}

static int cancel_entry_put(void *user, const char *collection,
                            const char *id, const char *payload) {
  cancel_entry_fixture_t *fixture = user;
  int rc = fixture->inner.put(fixture->inner.user_data, collection, id, payload);
  if (rc == SALTS_OK && fixture->cancel_after == 2 &&
      !strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION) &&
      cancel_entry_record_is(payload, "started"))
    cancel_entry_at_boundary(fixture);
  return rc;
}

static int cancel_entry_create(void *user, const char *collection,
                               const char *id, const char *payload) {
  cancel_entry_fixture_t *fixture = user;
  int rc = fixture->inner.create_if_absent(
      fixture->inner.user_data, collection, id, payload);
  if (rc == SALTS_OK && fixture->cancel_after == 1 &&
      !strcmp(collection, TURBO_AGENT_TOOL_CLAIM_COLLECTION) &&
      cancel_entry_record_is(payload, "admitted"))
    cancel_entry_at_boundary(fixture);
  return rc;
}

static int cancel_entry_get(void *user, const char *collection,
                            const char *id, char **out) {
  cancel_entry_fixture_t *fixture = user;
  return fixture->inner.get(fixture->inner.user_data, collection, id, out);
}

static int cancel_entry_list(void *user, const char *collection,
                             const char *key, const char *value, char **out) {
  cancel_entry_fixture_t *fixture = user;
  return fixture->inner.list(
      fixture->inner.user_data, collection, key, value, out);
}

static turbo_tool_status_t cancel_entry_invoke(
    const char *arguments, const turbo_tool_execution_context_t *context,
    char **out, void *user) {
  cancel_entry_fixture_t *fixture = user;
  (void)arguments;
  if (!out || !context) return TURBO_TOOL_INVALID_ARGUMENT;
  ++fixture->callbacks;
  *out = malloc(3);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, "ok", 3);
  return TURBO_TOOL_OK;
}

static turbo_agent_tool_execution_t cancel_entry_call(const char *id) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = id;
  call.turn_key = "turn";
  call.tool_name = "cancel.probe";
  call.arguments_json = "{}";
  return call;
}

static void cancel_entry_clear(turbo_agent_tool_execution_t *call) {
  free(call->output);
  json_free(call->detail);
  *call = (turbo_agent_tool_execution_t){0};
}

static int cancel_entry_execute(
    turbo_agent_tool_executor_t *executor, turbo_agent_runtime_t *runtime,
    const turbo_cancel_token_t *token, const turbo_tool_registry_t *registry,
    const turbo_agent_policy_t *policy, turbo_agent_tool_execution_t *call) {
  return turbo_agent_tool_executor_execute(
      executor, runtime, token, "thread", "run", NULL, NULL,
      registry, policy, call, 1);
}

spec("ToolExecutor cancellation between durable claim and backend entry") {
  it("stops after ADMITTED or STARTED without a callback, preserving UNKNOWN replay") {
    for (int cut = 1; cut <= 2; ++cut) {
      cancel_entry_fixture_t fixture = {0};
      turbo_agent_runtime_store_t callbacks = {0};
      turbo_agent_runtime_t *runtime[2] = {0};
      turbo_agent_tool_executor_t *executor[2] = {0};
      turbo_tool_registry_t *registry = NULL;
      turbo_tool_definition_v4_t definition = {0};
      turbo_agent_policy_t policy = turbo_agent_policy_default();
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      turbo_agent_tool_execution_t cancelled = cancel_entry_call("cancelled-call");
      turbo_agent_tool_execution_t replay = cancel_entry_call("cancelled-call");
      turbo_agent_tool_execution_t independent = cancel_entry_call("independent-call");
      json_value_t *records = NULL;
      const json_value_t *record;

      fixture.cancel_after = cut;
      fixture.cancel_rc = SALTS_EINVAL;
      fixture.inner = turbo_agent_runtime_store_memory_create();
      check_not_null(fixture.inner.put);
      check_not_null(fixture.inner.create_if_absent);
      check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
      check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);
      fixture.source = source;
      callbacks.put = cancel_entry_put;
      callbacks.get = cancel_entry_get;
      callbacks.list = cancel_entry_list;
      callbacks.create_if_absent = cancel_entry_create;
      callbacks.user_data = &fixture;
      /* Stack-owned hook and shared inner Store: destroy both Runtimes before
       * destroying its backing Store; every operation in this case is serial. */
      for (size_t i = 0; i < 2; ++i) {
        runtime[i] = turbo_agent_runtime_create(&callbacks);
        check_not_null(runtime[i]);
        check_equal(turbo_agent_tool_executor_create(NULL, &executor[i]), SALTS_OK);
      }
      registry = turbo_tool_registry_create();
      check_not_null(registry);
      definition.struct_size = sizeof(definition);
      definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
      definition.definition.name = "cancel.probe";
      definition.definition.description = "Atomic Store cancellation entry probe";
      definition.definition.parameters_json = "{\"type\":\"object\"}";
      definition.definition.user_data = &fixture;
      definition.context_handler = cancel_entry_invoke;
      check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
      policy.allow_custom_tools = 1;

      check_equal(cancel_entry_execute(
          executor[0], runtime[0], token, registry, &policy, &cancelled), SALTS_OK);
      check_equal(fixture.triggered, 1);
      check_equal(fixture.cancel_rc, SALTS_OK);
      check_equal(turbo_cancel_token_check(token), SALTS_ECANCELED);
      check_equal(cancelled.status, TURBO_TOOL_CANCELLED);
      check_false(cancelled.replayed);
      check_null(cancelled.output);
      check_null(cancelled.detail);
      check_equal(fixture.callbacks, 0);

      check_equal(turbo_agent_runtime_store_list_json(
          runtime[1], TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
          "call_id", "cancelled-call", &records), SALTS_OK);
      check_equal(json_array_size(records), 1u);
      record = json_array_get(records, 0);
      check_equal(json_get_string(record, "phase"), cut == 1 ? "planned" : "started");
      check_null(json_object_get(record, "output"));
      check_null(json_object_get(record, "detail"));
      json_free(records); records = NULL;

      check_equal(turbo_agent_runtime_store_list_json(
          runtime[1], TURBO_AGENT_TOOL_CLAIM_COLLECTION,
          "call_id", "cancelled-call", &records), SALTS_OK);
      check_equal(json_array_size(records), 1u);
      check_equal(json_get_string(json_array_get(records, 0), "phase"), "admitted");
      json_free(records); records = NULL;

      /* No cancel token on the second owner: the durable claim still forbids
       * re-entering a possibly executed backend, even if this fixture knows
       * the first callback was never entered. */
      check_equal(cancel_entry_execute(
          executor[1], runtime[1], NULL, registry, &policy, &replay), SALTS_OK);
      check_equal(replay.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
      check_true(replay.replayed);
      check_null(replay.output);
      check_null(replay.detail);
      check_equal(fixture.callbacks, 0);

      /* An unrelated canonical effect is not blocked by the cancelled key. */
      check_equal(cancel_entry_execute(
          executor[1], runtime[1], NULL, registry, &policy, &independent), SALTS_OK);
      check_equal(independent.status, TURBO_TOOL_OK);
      check_false(independent.replayed);
      check_equal(independent.output, "ok");
      check_equal(fixture.callbacks, 1);

      check_equal(turbo_agent_runtime_store_list_json(
          runtime[1], TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
          "call_id", "cancelled-call", &records), SALTS_OK);
      check_equal(json_array_size(records), 1u);
      check_equal(json_get_string(json_array_get(records, 0), "phase"),
                  cut == 1 ? "planned" : "started");
      json_free(records); records = NULL;

      cancel_entry_clear(&cancelled);
      cancel_entry_clear(&replay);
      cancel_entry_clear(&independent);
      turbo_tool_registry_destroy(registry);
      for (size_t i = 0; i < 2; ++i) {
        turbo_agent_tool_executor_destroy(executor[i]);
        turbo_agent_runtime_destroy(runtime[i]);
      }
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      fixture.inner.user_data_free(fixture.inner.user_data);
    }
  }
}
