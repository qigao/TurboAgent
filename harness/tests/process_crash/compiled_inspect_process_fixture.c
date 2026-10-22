#include "turbo_agent_harness.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_session.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store_sqlite.h"
#include "turbo_tool_schema.h"
#include <salts/thread_pool.h>
#include <fmt.h>
#include <tstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIXTURE_FAILURE = 2, FIXTURE_CRASH = 86 };
#define ATTEMPTS "compiled_inspect_attempts"
#define RUN_REFERENCES "agent_compiled_execution_journal"
#define THREAD_ID "compiled-crash-thread"
#define ATTEMPT_ID "inspect-attempt"

static const char source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"hello\\\"}\"}";
static const char alternate_source[] = "{\"template_id\":\"inspect\",\"step_id\":\"read-1\",\"tool\":\"repo_inspect\",\"arguments_json\":\"{\\\"value\\\":\\\"different\\\"}\"}";

typedef struct crash_case_s {
  const char *name, *attempt_phase, *run_phase;
  int calls;
} crash_case_t;
static const crash_case_t cases[] = {
  {"before_claim", NULL, NULL, 0},
  {"after_claim", "started", NULL, 0},
  {"before_run_started", "started", NULL, 0},
  {"after_run_started", "started", "started", 0},
  {"in_callback", "started", "started", 1},
  {"before_committed", "started", "started", 1},
  {"after_committed", "committed", "started", 1},
  {"after_run_committed", "committed", "committed", 1},
  {"before_delivery", "committed", "committed", 1}
};

typedef struct fixture_s {
  const crash_case_t *cut;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime, *oracle;
  turbo_tool_registry_t *registry;
  turbo_agent_harness_t *harness;
  cmeta_threadpool_t *pool;
  int inject, callbacks, transports, attempt_reads, attempt_writes, reference_writes;
} fixture_t;

static int failure(const char *expression, int line) {
  fprintf(stderr, "Compiled Inspect process fixture line %d: %s\n", line, expression);
  return FIXTURE_FAILURE;
}
#define REQUIRE(expression) do { if (!(expression)) return failure(#expression, __LINE__); } while (0)

static int text_is(const json_value_t *record, const char *key, const char *expected) {
  const json_value_t *value = json_object_get(record, key);
  return value && json_type(value) == JSON_STRING && json_string_len(value) == strlen(expected) &&
      !memcmp(json_string(value), expected, strlen(expected));
}
static int counter_put(fixture_t *f, const char *id, int count) {
  json_value_t *record = json_create_object();
  int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", count))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "external_counter", id, record);
  json_free(record);
  return rc;
}
static int counter_get(fixture_t *f, const char *id, int *count) {
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "external_counter", id, &record);
  int64_t n = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (n < 0 || n > 100)) rc = SALTS_EPROTO;
  if (!rc) *count = (int)n;
  json_free(record);
  return rc;
}
static int counter_add(fixture_t *f, const char *id) {
  int count;
  int rc = counter_get(f, id, &count);
  return rc ? rc : count >= 100 ? SALTS_ERANGE : counter_put(f, id, count + 1);
}

static int store_write(fixture_t *f, const char *collection, const char *id,
    const char *text, int create) {
  json_value_t *record = NULL;
  const char *phase;
  int before = 0, after = 0, rc;
  if (!strcmp(collection, ATTEMPTS) || !strcmp(collection, RUN_REFERENCES)) {
    record = json_parse(text, strlen(text));
    phase = json_get_string(record, "phase");
    if (!phase) { json_free(record); return SALTS_EPROTO; }
    if (!strcmp(collection, ATTEMPTS)) {
      ++f->attempt_writes;
      if (!strcmp(phase, "started")) {
        if (!create) { json_free(record); return SALTS_EPROTO; }
        before = !strcmp(f->cut->name, "before_claim");
        after = !strcmp(f->cut->name, "after_claim");
      } else if (!strcmp(phase, "committed")) {
        before = !strcmp(f->cut->name, "before_committed");
        after = !strcmp(f->cut->name, "after_committed");
      }
    } else {
      ++f->reference_writes;
      if (json_object_get(record, "result")) { json_free(record); return SALTS_EPROTO; }
      if (!strcmp(phase, "started")) {
        before = !strcmp(f->cut->name, "before_run_started");
        after = !strcmp(f->cut->name, "after_run_started");
      } else if (!strcmp(phase, "committed")) after = !strcmp(f->cut->name, "after_run_committed");
    }
  }
  if (f->inject && before) _Exit(FIXTURE_CRASH);
  rc = create ? f->inner.create_if_absent(f->inner.user_data, collection, id, text)
              : f->inner.put(f->inner.user_data, collection, id, text);
  if (!rc && f->inject && after) _Exit(FIXTURE_CRASH);
  json_free(record);
  return rc;
}
static int store_put(void *user, const char *collection, const char *id, const char *text) {
  return store_write(user, collection, id, text, 0);
}
static int store_create(void *user, const char *collection, const char *id, const char *text) {
  return store_write(user, collection, id, text, 1);
}
static int store_get(void *user, const char *collection, const char *id, char **out) {
  fixture_t *f = user;
  if (!strcmp(collection, ATTEMPTS)) ++f->attempt_reads;
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int store_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  fixture_t *f = user;
  if (!strcmp(collection, ATTEMPTS)) ++f->attempt_reads;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static void store_free(void *user) {
  fixture_t *f = user;
  if (f->inner.user_data_free) f->inner.user_data_free(f->inner.user_data);
  f->inner = (turbo_agent_runtime_store_t){0};
}
static int no_transport(const char *request, char **response, void *user) {
  fixture_t *f = user;
  (void)request; (void)response;
  ++f->transports;
  return -1;
}
static int inspect_callback(const json_value_t *arguments, json_value_t **out, void *user) {
  fixture_t *f = user;
  json_value_t *records = NULL, *ok;
  int valid;
  *out = NULL;
  ++f->callbacks;
  if (!text_is(arguments, "value", "hello") ||
      turbo_agent_runtime_store_list_json(f->runtime, ATTEMPTS, NULL, NULL, &records)) return -1;
  valid = json_array_size(records) == 1 && text_is(json_array_get(records, 0), "phase", "started");
  json_free(records);
  if (!valid || counter_add(f, "invocation")) return -1;
  /* Instrumentation observes callback entry; this does not admit WRITE tools. */
  if (f->inject && !strcmp(f->cut->name, "in_callback")) _Exit(FIXTURE_CRASH);
  *out = json_create_object();
  ok = json_create_bool(1);
  if (!*out || !ok || turbo_runtime_json_object_set(*out, "ok", ok)) {
    json_free(ok); /* The checked setter transfers ownership only on success. */
    json_free(*out); *out = NULL; return -1;
  }
  return 0;
}

static int fixture_open(fixture_t *f, const char *directory, int create) {
  static const char *capabilities[] = {"runtime_tools"};
  turbo_runtime_sqlite_config_t sqlite;
  turbo_agent_runtime_store_t oracle = {0}, callbacks = {0};
  turbo_tool_definition_v6_t tool = {0};
  cmeta_threadpool_config_t pool = {.num_threads = 1, .queue_capacity = 1};
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t harness;
  tstr journal_path = tstr_format("{}/session.db", directory);
  tstr oracle_path = tstr_format("{}/external.db", directory);
  int rc = SALTS_ENOMEM;
  if (!journal_path || !oracle_path) goto done;
  turbo_runtime_sqlite_config_init(&sqlite);
  sqlite.path = oracle_path; sqlite.create_if_missing = create;
  sqlite.max_record_bytes = 65536; sqlite.max_list_bytes = 1048576; sqlite.max_list_records = 256;
  rc = turbo_runtime_store_sqlite_open(&sqlite, &oracle);
  if (rc) goto done;
  f->oracle = turbo_agent_runtime_create(&oracle);
  if (!f->oracle) { oracle.user_data_free(oracle.user_data); rc = SALTS_ENOMEM; goto done; }
  f->registry = turbo_tool_registry_create();
  f->pool = cmeta_threadpool_create_with_config(&pool);
  if (!f->registry || !f->pool) { rc = SALTS_ENOMEM; goto done; }
  tool.struct_size = sizeof(tool); tool.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  tool.definition.name = "repo.inspect"; tool.definition.description = "Read-only compiled crash fixture";
  tool.definition.parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},\"required\":[\"value\"],\"additionalProperties\":false}";
  tool.definition.json_value_handler = inspect_callback; tool.definition.user_data = f;
  tool.execution_policy = (turbo_tool_execution_policy_t){TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
  tool.required_capabilities = capabilities; tool.required_capability_count = 1;
  tool.result_schema_json = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"],\"additionalProperties\":false}";
  tool.strict_result = 1; tool.effect_flags = TURBO_TOOL_EFFECT_READ;
  if (turbo_tool_registry_add_v6(f->registry, &tool) != TURBO_TOOL_OK) { rc = SALTS_EPROTO; goto done; }
  sqlite.path = journal_path;
  rc = turbo_runtime_store_sqlite_open(&sqlite, &f->inner);
  if (rc) goto done;
  callbacks.put = store_put; callbacks.get = store_get; callbacks.list = store_list;
  callbacks.create_if_absent = store_create; callbacks.user_data = f; callbacks.user_data_free = store_free;
  session.thread_id = THREAD_ID; session.runtime_store = callbacks;
  session.agent_config.model = "offline-compiled-fixture";
  session.agent_config.transport_fn = no_transport; session.agent_config.transport_user_data = f;
  session.agent_config.tool_registry = f->registry;
  app.session_config = &session;
  turbo_agent_harness_config_init(&harness); harness.app_config = &app; harness.executor = f->pool;
  f->harness = turbo_agent_harness_create(&harness);
  if (!f->harness) { rc = SALTS_ENOMEM; goto done; }
  f->runtime = turbo_agent_session_runtime(turbo_agent_app_session(turbo_agent_harness_app(f->harness)));
  rc = f->runtime ? SALTS_OK : SALTS_EIO;
done:
  tstr_free(journal_path); tstr_free(oracle_path);
  return rc;
}
static void fixture_close(fixture_t *f) {
  if (f->harness) turbo_agent_harness_release(f->harness);
  store_free(f); /* Cleared by Session destruction if it consumed the store. */
  turbo_tool_registry_destroy(f->registry);
  if (f->pool) cmeta_threadpool_destroy(f->pool);
  turbo_agent_runtime_destroy(f->oracle);
}
static turbo_agent_t *fixture_agent(fixture_t *f) {
  return turbo_agent_session_agent(turbo_agent_app_session(turbo_agent_harness_app(f->harness)));
}
static int grant(fixture_t *f, int allow) {
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  policy.allow_runtime_tools = allow;
  return turbo_agent_set_tool_policy(fixture_agent(f), &policy);
}
static int execute(fixture_t *f, const char *input, turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_tool_status_t *status, json_value_t **result, json_value_t **certificate) {
  static const char *capabilities[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = capabilities; config.allowed_capability_count = 1;
  return turbo_agent_harness_execute_compiled_inspect_attempt_json(f->harness, &config,
      input, strlen(input), NULL, ATTEMPT_ID, diagnostic, status, result, certificate);
}
static int result_valid(fixture_t *f, const json_value_t *result) {
  return result && turbo_tool_schema_validate_result_json_value(f->registry, "repo.inspect", result, NULL, 0)
      == TURBO_TOOL_SCHEMA_VALID && json_get_bool(result, "ok", false);
}

static int evidence(fixture_t *f, const char *phase, int calls, int deliveries, int crash_only) {
  json_value_t *attempts = NULL, *references = NULL;
  const json_value_t *attempt;
  int n, valid;
  REQUIRE(!counter_get(f, "invocation", &n) && n == calls);
  REQUIRE(!counter_get(f, "delivery", &n) && n == deliveries);
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, ATTEMPTS, NULL, NULL, &attempts));
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, RUN_REFERENCES, NULL, NULL, &references));
  attempt = json_array_get(attempts, 0);
  valid = attempts && json_type(attempts) == JSON_ARRAY && json_array_size(attempts) == (phase ? 1u : 0u) &&
      references && json_type(references) == JSON_ARRAY;
  if (valid && phase) {
    valid = text_is(attempt, "phase", phase) && text_is(attempt, "thread_id", THREAD_ID) &&
        text_is(attempt, "attempt_id", ATTEMPT_ID) && text_is(attempt, "tool_name", "repo.inspect") &&
        json_get_string(attempt, "id") && json_get_string(attempt, "execution_id") &&
        json_get_string(attempt, "plan_hash") && json_object_get(attempt, "certificate") &&
        json_get_int(attempt, "schema_version", -1) == 1 &&
        json_get_int(attempt, "effects", -1) == TURBO_TOOL_EFFECT_READ &&
        json_get_int(attempt, "authority_generation", -1) == 2;
    if (!strcmp(phase, "committed"))
      valid = valid && json_get_int(attempt, "tool_status", -1) == TURBO_TOOL_OK &&
          result_valid(f, json_object_get(attempt, "result"));
    else valid = valid && !json_object_get(attempt, "result") &&
        json_get_int(attempt, "tool_status", -1) == TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
  }
  if (valid && crash_only) valid = json_array_size(references) == (f->cut->run_phase ? 1u : 0u);
  for (size_t i = 0; valid && i < json_array_size(references); ++i) {
    const json_value_t *reference = json_array_get(references, i);
    valid = phase && text_is(reference, "attempt_record_id", json_get_string(attempt, "id")) &&
        text_is(reference, "thread_id", THREAD_ID) && text_is(reference, "attempt_id", ATTEMPT_ID) &&
        text_is(reference, "plan_hash", json_get_string(attempt, "plan_hash")) &&
        !json_object_get(reference, "result");
    if (valid && crash_only) valid = text_is(reference, "phase", f->cut->run_phase);
  }
  json_free(attempts); json_free(references);
  REQUIRE(valid);
  return 0;
}

static int exercise(fixture_t *f, const char *mode) {
  turbo_agent_compile_diagnostic_t diagnostic = {0};
  turbo_tool_status_t status = TURBO_TOOL_ERROR;
  json_value_t *result = NULL, *certificate = NULL;
  int rc = FIXTURE_FAILURE, operation = SALTS_EIO;
  if (!strcmp(mode, "init")) {
    REQUIRE(!counter_put(f, "invocation", 0) && !counter_put(f, "delivery", 0));
    return evidence(f, NULL, 0, 0, 0);
  }
  if (!strcmp(mode, "inspect")) return evidence(f, f->cut->attempt_phase, f->cut->calls, 0, 1);
  REQUIRE(!grant(f, strcmp(mode, "denied") != 0));
  if (!strcmp(mode, "stale")) {
    turbo_agent_tool_authority_snapshot_t before = {0}, after = {0};
    REQUIRE(!turbo_agent_tool_authority_snapshot_get(fixture_agent(f), &before));
    /* Both setters finish on the quiescent control plane before execute. */
    REQUIRE(!grant(f, 1));
    REQUIRE(!turbo_agent_tool_authority_snapshot_get(fixture_agent(f), &after));
    REQUIRE(before.capability_mask == after.capability_mask && before.generation != after.generation);
    REQUIRE(turbo_agent_tool_authority_snapshot_is_current(fixture_agent(f), &before) == 0);
  }
  f->attempt_reads = f->attempt_writes = f->reference_writes = 0;
  f->inject = !strcmp(mode, "crash");
  operation = execute(f, !strcmp(mode, "mismatch") ? alternate_source : source,
      &diagnostic, &status, &result, &certificate);
  if (f->transports) goto done;
  if (!strcmp(mode, "denied")) {
    if (operation == SALTS_EPROTO && diagnostic.status == TURBO_AGENT_COMPILE_CAPABILITY_DENIED &&
        !result && !certificate && !f->callbacks && !f->attempt_reads && !f->attempt_writes && !f->reference_writes)
      rc = evidence(f, f->cut->attempt_phase, f->cut->calls, 0, 1);
  } else if (!strcmp(mode, "stale") || !strcmp(mode, "mismatch")) {
    if (operation == SALTS_EPROTO && diagnostic.status == TURBO_AGENT_COMPILE_SOURCE_INVALID &&
        !result && certificate && !f->callbacks && !f->attempt_writes && !f->reference_writes)
      rc = evidence(f, f->cut->attempt_phase, f->cut->calls, 0, 1);
  } else if (!strcmp(mode, "crash") || !strcmp(mode, "recover") || !strcmp(mode, "replay")) {
    int replay = !strcmp(mode, "replay");
    int unknown = f->cut->attempt_phase && !strcmp(f->cut->attempt_phase, "started") && !f->inject;
    if (unknown) {
      if (operation == SALTS_EBUSY && status == TURBO_TOOL_UNKNOWN_SIDE_EFFECT && !result && certificate &&
          !f->callbacks && !f->attempt_writes)
        rc = evidence(f, "started", f->cut->calls, 0, 0);
    } else {
      int entered = f->inject || (!replay && !f->cut->attempt_phase);
      if (operation || status != TURBO_TOOL_OK || !certificate || !result_valid(f, result) ||
          f->callbacks != entered) goto done;
      if (f->inject && !strcmp(f->cut->name, "before_delivery")) _Exit(FIXTURE_CRASH);
      if (f->inject) goto done;
      if (!replay && counter_add(f, "delivery")) goto done;
      rc = evidence(f, "committed", 1, 1, 0);
    }
  }
done:
  if (rc) fprintf(stderr, "%s/%s: rc=%d status=%d diagnostic=%d (%s) callbacks=%d reads=%d writes=%d references=%d transport=%d\n",
      f->cut->name, mode, operation, (int)status, (int)diagnostic.status, diagnostic.message,
      f->callbacks, f->attempt_reads, f->attempt_writes, f->reference_writes, f->transports);
  json_free(result); json_free(certificate);
  return rc;
}
int main(int argc, char **argv) {
  fixture_t fixture = {0};
  int rc;
  if (argc != 4) return failure("mode/cut/private-directory required", __LINE__);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    if (!strcmp(argv[2], cases[i].name)) fixture.cut = &cases[i];
  if (!fixture.cut) return failure("unknown crash cut", __LINE__);
  rc = fixture_open(&fixture, argv[3], !strcmp(argv[1], "init"));
  if (!rc) rc = exercise(&fixture, argv[1]);
  else fprintf(stderr, "Compiled Inspect fixture open failed: %d\n", rc);
  fixture_close(&fixture);
  return rc ? FIXTURE_FAILURE : 0;
}
