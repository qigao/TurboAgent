#include "../../src/turbo_agent_tool_executor_internal.h"
#include "../../src/turbo_agent_tool_journal_internal.h"
#include "turbo_tool_schema.h"
#include "turbo_runtime_store_sqlite.h"
#include <fmt.h>
#include <tstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIXTURE_FAILURE = 2, FIXTURE_CRASH = 86 };

typedef struct crash_case_s {
  const char *name, *phase;
  int claim, effects, unknown;
} crash_case_t;

static const crash_case_t cases[] = {
  {"before_planned", NULL, 0, 0, 0}, {"after_planned", "planned", 0, 0, 0},
  {"before_admitted", "planned", 0, 0, 0},
  {"after_admitted", "planned", 1, 0, 1},
  {"before_started", "planned", 1, 0, 1},
  {"after_started", "started", 1, 0, 1},
  {"after_effect", "started", 1, 1, 1},
  {"before_committed", "started", 1, 1, 1},
  {"after_committed", "committed", 1, 1, 0},
  {"before_delivery", "committed", 1, 1, 0}
};

typedef struct crash_fixture_s {
  const crash_case_t *cut;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *journal, *oracle;
  turbo_tool_registry_t *registry;
  turbo_agent_tool_executor_t *executor;
  int inject, callbacks, gets, puts, lists, bad_context;
} crash_fixture_t;

static int failure(const char *expression, int line) {
  fprintf(stderr, "Tool Journal process fixture line %d: %s\n", line, expression);
  return FIXTURE_FAILURE;
}
#define REQUIRE(expression) do { if (!(expression)) return failure(#expression, __LINE__); } while (0)

static int text_is(const json_value_t *record, const char *key, const char *expected) {
  const json_value_t *value = json_object_get(record, key);
  return value && json_type(value) == JSON_STRING &&
      json_string_len(value) == strlen(expected) && !memcmp(json_string(value), expected, strlen(expected));
}

static int counter_put(turbo_agent_runtime_t *runtime, const char *id, int count) {
  json_value_t *record = json_create_object();
  int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", count))
    rc = turbo_agent_runtime_store_put_json(runtime, "external_counter", id, record);
  json_free(record);
  return rc;
}

static int counter_get(turbo_agent_runtime_t *runtime, const char *id, int *count) {
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(runtime, "external_counter", id, &record);
  int64_t number = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (number < 0 || number > 100)) rc = SALTS_EPROTO;
  if (!rc) *count = (int)number;
  json_free(record);
  return rc;
}

static int counter_add(turbo_agent_runtime_t *runtime, const char *id, int *count) {
  int rc = counter_get(runtime, id, count);
  if (rc) return rc;
  if (*count >= 100) return SALTS_ERANGE;
  ++*count;
  return counter_put(runtime, id, *count);
}

static int fixture_put(void *user, const char *collection, const char *id, const char *text) {
  crash_fixture_t *f = user;
  json_value_t *record = NULL;
  const char *phase = NULL;
  int before = 0, after = 0, rc;
  ++f->puts;
  if (!strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION)) {
    record = json_parse(text, strlen(text)); phase = json_get_string(record, "phase");
    if (!phase) { json_free(record); return SALTS_EPROTO; }
    before = (!strcmp(f->cut->name, "before_planned") && !strcmp(phase, "planned")) ||
        (!strcmp(f->cut->name, "before_started") && !strcmp(phase, "started")) ||
        (!strcmp(f->cut->name, "before_committed") && !strcmp(phase, "committed"));
    after = (!strcmp(f->cut->name, "after_planned") && !strcmp(phase, "planned")) ||
        (!strcmp(f->cut->name, "after_started") && !strcmp(phase, "started")) ||
        (!strcmp(f->cut->name, "after_committed") && !strcmp(phase, "committed"));
  }
  if (f->inject && before) _Exit(FIXTURE_CRASH);
  rc = f->inner.put(f->inner.user_data, collection, id, text);
  if (!rc && f->inject && after) _Exit(FIXTURE_CRASH);
  json_free(record);
  return rc;
}

static int fixture_create(void *user, const char *collection, const char *id,
                          const char *text) {
  crash_fixture_t *f = user;
  json_value_t *record = NULL;
  const char *phase = NULL;
  int before = 0, after = 0, rc;
  if (!strcmp(collection, TURBO_AGENT_TOOL_JOURNAL_COLLECTION) ||
      !strcmp(collection, TURBO_AGENT_TOOL_CLAIM_COLLECTION)) {
    record = json_parse(text, strlen(text));
    phase = json_get_string(record, "phase");
    if (!phase) { json_free(record); return SALTS_EPROTO; }
    before = (!strcmp(f->cut->name, "before_planned") && !strcmp(phase, "planned")) ||
        (!strcmp(f->cut->name, "before_admitted") && !strcmp(phase, "admitted"));
    after = (!strcmp(f->cut->name, "after_planned") && !strcmp(phase, "planned")) ||
        (!strcmp(f->cut->name, "after_admitted") && !strcmp(phase, "admitted"));
  }
  if (f->inject && before) _Exit(FIXTURE_CRASH);
  rc = f->inner.create_if_absent(f->inner.user_data, collection, id, text);
  if (!rc && f->inject && after) _Exit(FIXTURE_CRASH);
  json_free(record);
  return rc;
}

static int fixture_get(void *user, const char *collection, const char *id, char **out) {
  crash_fixture_t *f = user; ++f->gets;
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int fixture_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  crash_fixture_t *f = user; ++f->lists;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static void fixture_store_free(void *user) {
  crash_fixture_t *f = user;
  if (f->inner.user_data_free) f->inner.user_data_free(f->inner.user_data);
  f->inner = (turbo_agent_runtime_store_t){0};
}

static turbo_tool_status_t fixture_effect(const char *arguments, const turbo_tool_execution_context_t *context,
    char **out, void *user) {
  crash_fixture_t *f = user;
  json_value_t *result = NULL;
  char *serialized = NULL;
  int count, rc = TURBO_TOOL_ERROR;
  ++f->callbacks; *out = NULL;
  if (!context || !context->thread_id || strcmp(context->thread_id, "crash-thread") ||
      !context->run_id || strcmp(context->run_id, "crash-run") || !context->turn_id ||
      strcmp(context->turn_id, "turn") || !context->tool_call_id || strcmp(context->tool_call_id, "call") ||
      !context->detail_sink || strcmp(arguments, "{\"value\":1}")) {
    f->bad_context = 1; return TURBO_TOOL_ERROR;
  }
  if (counter_add(f->oracle, "effect", &count)) return TURBO_TOOL_ERROR;
  /* Independent committed effect evidence survives without a Tool result. */
  if (f->inject && !strcmp(f->cut->name, "after_effect")) _Exit(FIXTURE_CRASH);
  result = json_create_object();
  if (!result || turbo_runtime_json_object_set_int64(result, "effect_count", count)) goto done;
  context->detail_sink(result, context->detail_sink_user_data);
  serialized = json_serialize(result, NULL);
  if (!serialized) goto done;
  /* Text callback ABI transfers malloc-owned storage, not a borrowed tstr. */
  *out = malloc(strlen(serialized) + 1);
  if (!*out) { rc = TURBO_TOOL_OUT_OF_MEMORY; goto done; }
  memcpy(*out, serialized, strlen(serialized) + 1); rc = TURBO_TOOL_OK;
done:
  json_serialize_free(serialized); json_free(result);
  return (turbo_tool_status_t)rc;
}

static int fixture_open(crash_fixture_t *f, const char *directory, int create) {
  static const char *capabilities[] = {"custom_tools"};
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t oracle_store = {0}, callbacks;
  turbo_tool_definition_v6_t definition = {0};
  turbo_agent_tool_executor_config_t executor_config;
  tstr journal_path = tstr_format("{}/journal.db", directory);
  tstr oracle_path = tstr_format("{}/external.db", directory);
  int rc = SALTS_ENOMEM;
  if (!journal_path || !oracle_path) goto done;
  turbo_runtime_sqlite_config_init(&config); config.path = journal_path; config.create_if_missing = create;
  config.max_record_bytes = config.max_list_bytes = 16384; config.max_list_records = 8;
  rc = turbo_runtime_store_sqlite_open(&config, &f->inner);
  if (rc) goto done;
  callbacks = (turbo_agent_runtime_store_t){fixture_put, fixture_get, fixture_list, f,
                                            fixture_store_free, fixture_create};
  f->journal = turbo_agent_runtime_create(&callbacks);
  if (!f->journal) { rc = SALTS_ENOMEM; goto done; }
  config.path = oracle_path;
  rc = turbo_runtime_store_sqlite_open(&config, &oracle_store);
  if (rc) goto done;
  f->oracle = turbo_agent_runtime_create(&oracle_store);
  if (!f->oracle) {
    oracle_store.user_data_free(oracle_store.user_data); rc = SALTS_ENOMEM; goto done;
  }
  f->registry = turbo_tool_registry_create();
  if (!f->registry) { rc = SALTS_ENOMEM; goto done; }
  definition.struct_size = sizeof(definition); definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = "probe"; definition.definition.description = "Independent durable counter fixture";
  definition.definition.parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},\"required\":[\"value\"],\"additionalProperties\":false}";
  definition.definition.strict = 1; definition.definition.user_data = f; definition.context_handler = fixture_effect;
  definition.execution_policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE};
  definition.required_capabilities = capabilities; definition.required_capability_count = 1;
  definition.result_schema_json = "{\"type\":\"object\",\"properties\":{\"effect_count\":{\"type\":\"integer\"}},\"required\":[\"effect_count\"],\"additionalProperties\":false}";
  definition.strict_result = 1; definition.effect_flags = TURBO_TOOL_EFFECT_WRITE;
  rc = turbo_tool_registry_add_v6(f->registry, &definition);
  if (rc != TURBO_TOOL_OK) { rc = SALTS_EPROTO; goto done; }
  turbo_agent_tool_executor_config_init(&executor_config);
  executor_config.max_workers = executor_config.queue_capacity = executor_config.max_batch_calls = 1;
  executor_config.max_arguments_bytes = 1024; executor_config.max_output_bytes = 4096;
  rc = turbo_agent_tool_executor_create(&executor_config, &f->executor);
done:
  tstr_free(journal_path); tstr_free(oracle_path);
  return rc;
}

static void fixture_close(crash_fixture_t *f) {
  turbo_agent_tool_executor_destroy(f->executor);
  turbo_tool_registry_destroy(f->registry);
  turbo_agent_runtime_destroy(f->journal);
  if (!f->journal) fixture_store_free(f);
  turbo_agent_runtime_destroy(f->oracle);
}

static turbo_agent_tool_execution_t fixture_call(void) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = "call"; call.turn_key = "turn"; call.tool_name = "probe";
  call.arguments_json = "{\"value\":1}";
  return call;
}
static void call_clear(turbo_agent_tool_execution_t *call) {
  free(call->output); json_free(call->detail); *call = (turbo_agent_tool_execution_t){0};
}
static int execute(crash_fixture_t *f, turbo_agent_tool_execution_t *call, const char *thread, int authorized) {
  turbo_agent_policy_t policy = turbo_agent_policy_default(); policy.allow_custom_tools = authorized;
  return turbo_agent_tool_executor_execute(f->executor, f->journal, NULL, thread, "crash-run",
      NULL, NULL, f->registry, &policy, call, 1);
}

static int result_valid(crash_fixture_t *f, const char *output, const json_value_t *detail) {
  json_value_t *result = output ? json_parse(output, strlen(output)) : NULL;
  int valid = result && turbo_tool_schema_validate_result_json_value(f->registry, "probe", result, NULL, 0)
      == TURBO_TOOL_SCHEMA_VALID &&
      turbo_runtime_json_value_as_int64(json_object_get(result, "effect_count"), -1) == 1 &&
      detail && turbo_runtime_json_value_as_int64(json_object_get(detail, "effect_count"), -1) == 1;
  json_free(result);
  return valid;
}

static int expect_evidence(crash_fixture_t *f, const char *phase, int claim, int effects,
                           int deliveries) {
  json_value_t *records = NULL, *claims = NULL;
  const json_value_t *record;
  int effect_count = -1, delivery_count = -1, valid;
  REQUIRE(!counter_get(f->oracle, "effect", &effect_count) && effect_count == effects);
  REQUIRE(!counter_get(f->oracle, "delivery", &delivery_count) && delivery_count == deliveries);
  REQUIRE(!turbo_agent_runtime_store_list_json(f->journal, TURBO_AGENT_TOOL_JOURNAL_COLLECTION, NULL, NULL, &records));
  valid = records && json_type(records) == JSON_ARRAY && json_array_size(records) == (phase ? 1u : 0u);
  record = json_array_get(records, 0);
  if (valid && phase) {
    valid = text_is(record, "phase", phase) && text_is(record, "thread_id", "crash-thread") &&
        text_is(record, "run_id", "crash-run") && text_is(record, "turn_key", "turn") &&
        text_is(record, "call_id", "call") && text_is(record, "tool_name", "probe") &&
        json_get_string(record, "journal_id") &&
        turbo_runtime_json_value_as_int64(json_object_get(record, "status"), -1) == TURBO_TOOL_OK;
    if (!strcmp(phase, "committed"))
      valid = valid && result_valid(f, json_get_string(record, "output"), json_object_get(record, "detail"));
    else valid = valid && !json_object_get(record, "output") && !json_object_get(record, "detail");
  }
  REQUIRE(valid);
  json_free(records);
  records = NULL;
  REQUIRE(!turbo_agent_runtime_store_list_json(f->journal, TURBO_AGENT_TOOL_CLAIM_COLLECTION,
                                                NULL, NULL, &claims));
  valid = claims && json_type(claims) == JSON_ARRAY &&
      json_array_size(claims) == (claim ? 1u : 0u);
  record = json_array_get(claims, 0);
  if (valid && claim)
    valid = text_is(record, "phase", "admitted") &&
        text_is(record, "thread_id", "crash-thread") &&
        text_is(record, "run_id", "crash-run") && text_is(record, "turn_key", "turn") &&
        text_is(record, "call_id", "call") && text_is(record, "tool_name", "probe");
  json_free(claims);
  REQUIRE(valid);
  return 0;
}

static int exercise(crash_fixture_t *f, const char *mode) {
  turbo_agent_tool_execution_t call = fixture_call();
  int rc = FIXTURE_FAILURE, operation = 0, delivery;
  if (!strcmp(mode, "init")) {
    REQUIRE(!counter_put(f->oracle, "effect", 0) && !counter_put(f->oracle, "delivery", 0));
    return expect_evidence(f, NULL, 0, 0, 0);
  }
  if (!strcmp(mode, "inspect"))
    return expect_evidence(f, f->cut->phase, f->cut->claim, f->cut->effects, 0);
  if (!strcmp(mode, "denied")) {
    operation = execute(f, &call, "crash-thread", 0);
    if (!operation && call.status == TURBO_TOOL_ERROR && call.policy_reason && !call.output && !call.detail &&
        !f->callbacks && !f->gets && !f->puts && !f->lists)
      rc = expect_evidence(f, f->cut->phase, f->cut->claim, f->cut->effects, 0);
  } else if (!strcmp(mode, "mismatch")) {
    call.arguments_json = "{\"value\":2}";
    operation = execute(f, &call, "crash-thread", 1);
    if (operation != SALTS_EPROTO || call.output || call.detail || f->callbacks || f->puts || f->lists) goto done;
    call_clear(&call); call = fixture_call();
    operation = execute(f, &call, "foreign-thread", 1);
    if (operation == SALTS_EPROTO && !call.output && !call.detail && !f->callbacks && !f->puts && !f->lists)
      rc = expect_evidence(f, f->cut->phase, f->cut->claim, f->cut->effects, 0);
  } else if (!strcmp(mode, "crash") || !strcmp(mode, "recover") || !strcmp(mode, "replay")) {
    int replay = !strcmp(mode, "replay");
    f->inject = !strcmp(mode, "crash");
    operation = execute(f, &call, "crash-thread", 1);
    if (operation || f->bad_context || f->lists) goto done;
    if (f->cut->unknown && !f->inject) {
      if (call.status == TURBO_TOOL_UNKNOWN_SIDE_EFFECT && call.replayed && !call.output && !call.detail &&
          !f->callbacks && !f->puts)
        rc = expect_evidence(f, f->cut->phase, f->cut->claim, f->cut->effects, 0);
    } else {
      int entered = f->inject || (!replay && (!f->cut->phase || !strcmp(f->cut->phase, "planned")));
      if (call.status != TURBO_TOOL_OK || f->callbacks != entered || call.replayed != !entered ||
          !result_valid(f, call.output, call.detail)) goto done;
      if (f->inject && !strcmp(f->cut->name, "before_delivery")) _Exit(FIXTURE_CRASH);
      if (f->inject) goto done; /* A selected cut must terminate, never return success. */
      if (!replay && counter_add(f->oracle, "delivery", &delivery)) goto done;
      rc = expect_evidence(f, "committed", 1, 1, 1);
    }
  }
done:
  if (rc) fprintf(stderr, "%s/%s: operation=%d status=%d replayed=%d callbacks=%d gets=%d puts=%d lists=%d bad_context=%d\n",
      f->cut->name, mode, operation, (int)call.status, call.replayed, f->callbacks, f->gets, f->puts, f->lists, f->bad_context);
  call_clear(&call);
  if (rc) return failure(mode, __LINE__);
  return 0;
}

int main(int argc, char **argv) {
  crash_fixture_t fixture = {0};
  int rc;
  if (argc != 4) return failure("mode/cut/private-directory required", __LINE__);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    if (!strcmp(argv[2], cases[i].name)) fixture.cut = &cases[i];
  if (!fixture.cut) return failure("unknown crash cut", __LINE__);
  rc = fixture_open(&fixture, argv[3], !strcmp(argv[1], "init"));
  if (!rc) rc = exercise(&fixture, argv[1]);
  else fprintf(stderr, "Process fixture open failed: %d\n", rc);
  fixture_close(&fixture);
  return rc ? FIXTURE_FAILURE : 0;
}
