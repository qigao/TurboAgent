#include "turbo_agent_harness.h"
#include "turbo_agent_job.h"
#include "turbo_agent_job_acceptance.h"
#include "turbo_agent_job_delivery.h"
#include "turbo_agent_job_verifier.h"
#include "turbo_agent_job_file_verifier.h"
#include <cmeta_fs.h>
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

enum {
  FIXTURE_FAILURE = 2, FIXTURE_CRASH = 86,
  ATTEMPT_SCHEMA_VERSION = 2, RUN_REFERENCE_SCHEMA_VERSION = 1
};
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
  {"before_delivery", "committed", "committed", 1},
  {"acceptance_before", "committed", "committed", 1},
  {"acceptance_after", "committed", "committed", 1},
  {"acceptance_lost_ack", "committed", "committed", 1},
  {"acceptance_returned", "committed", "committed", 1},
  {"delivery_intent_before", "committed", "committed", 1},
  {"delivery_intent_after", "committed", "committed", 1},
  {"delivery_intent_lost_ack", "committed", "committed", 1},
  {"delivery_intent_returned", "committed", "committed", 1},
  {"delivery_after_send", "committed", "committed", 1},
  {"delivery_receipt_before", "committed", "committed", 1},
  {"delivery_receipt_after", "committed", "committed", 1},
  {"delivery_receipt_lost_ack", "committed", "committed", 1},
  {"delivery_receipt_returned", "committed", "committed", 1}
};

typedef struct fixture_s {
  const crash_case_t *cut;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime, *oracle;
  turbo_tool_registry_t *registry;
  turbo_agent_harness_t *harness;
  cmeta_threadpool_t *pool;
  int inject, callbacks, transports, attempt_reads, attempt_writes, reference_writes;
  int job_mode, acceptance_mode, delivery_mode, verified_mode, file_mode;
  turbo_agent_job_file_verifier_t *file_verifier;
  char workspace_path[SALTS_FS_MAX_PATH], verification_path[SALTS_FS_MAX_PATH];
  json_value_t *execution_reference;
} fixture_t;

static int failure(const char *expression, int line) {
  fprintf(stderr, "Compiled Inspect process fixture line %d: %s\n", line, expression);
  return FIXTURE_FAILURE;
}
#define REQUIRE(expression) do { if (!(expression)) return failure(#expression, __LINE__); } while (0)

static int text_is(const json_value_t *record, const char *key, const char *expected) {
  const json_value_t *value = json_object_get(record, key);
  return expected && value && json_type(value) == JSON_STRING && json_string_len(value) == strlen(expected) &&
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
  if (!strcmp(collection, "agent_job_acceptance")) {
    if (!create) return SALTS_EPROTO;
    before = !strcmp(f->cut->name, "acceptance_before");
    after = !strcmp(f->cut->name, "acceptance_after");
  }
  if (!strcmp(collection, "agent_job_delivery_intents") || !strcmp(collection, "agent_job_delivery_receipts")) {
    int intent = !strcmp(collection, "agent_job_delivery_intents");
    if (!create) return SALTS_EPROTO;
    before = !strcmp(f->cut->name, intent ? "delivery_intent_before" : "delivery_receipt_before");
    after = !strcmp(f->cut->name, intent ? "delivery_intent_after" : "delivery_receipt_after");
  }
  if (f->inject && before) _Exit(FIXTURE_CRASH);
  rc = create ? f->inner.create_if_absent(f->inner.user_data, collection, id, text)
              : f->inner.put(f->inner.user_data, collection, id, text);
  if (!rc && f->inject && after) _Exit(FIXTURE_CRASH);
  if (!rc && f->inject && !strcmp(collection, "agent_job_acceptance") &&
      !strcmp(f->cut->name, "acceptance_lost_ack")) return SALTS_EIO;
  if (!rc && f->inject &&
      ((!strcmp(collection, "agent_job_delivery_intents") && !strcmp(f->cut->name, "delivery_intent_lost_ack")) ||
       (!strcmp(collection, "agent_job_delivery_receipts") && !strcmp(f->cut->name, "delivery_receipt_lost_ack"))))
    return SALTS_EIO;
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
  if (f->file_mode) {
    char path[SALTS_FS_MAX_PATH]; cmeta_fs_buf_t bytes = {0};
    REQUIRE(!cmeta_fs_path_join(path, sizeof(path), f->workspace_path, "source.bin"));
    REQUIRE(!cmeta_fs_read_file(path, &bytes));
    valid = bytes.len == 3 && !memcmp(bytes.base, "a\0b", 3);
    cmeta_fs_buf_free(&bytes);
    REQUIRE(valid);
  }
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

static int host_source(const json_value_t *snapshot, void *user) {
  fixture_t *f = user;
  const json_value_t *proposal = json_object_get(snapshot, "proposal");
  json_value_t *external = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "host_verification", "source", &external);
  if (rc == SALTS_OK && (!text_is(external, "source_revision", json_get_string(proposal, "source_revision")) ||
      !text_is(external, "source_json", json_get_string(proposal, "source_json")))) rc = SALTS_EPROTO;
  json_free(external);
  return rc;
}
static int host_evidence(const json_value_t *snapshot, const json_value_t *reference,
    const json_value_t *result, turbo_agent_job_acceptance_decision_t decision,
    const char *evidence_ref, void *user) {
  fixture_t *f = user;
  json_value_t *catalog = NULL, *external = NULL;
  const char *id;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "host_verification", "catalog", &catalog);
  if (rc != SALTS_OK) goto done;
  id = json_get_string(catalog, "record_id");
  if (!id || !id[0]) { rc = SALTS_EPROTO; goto done; }
  rc = turbo_agent_runtime_store_get_json_status(f->oracle, "host_job_evidence", id, &external);
  if (rc != SALTS_OK) goto done;
  if (!text_is(external, "evidence_ref", evidence_ref) ||
      !text_is(external, "job_id", json_get_string(snapshot, "id")) ||
      !text_is(external, "run_id", json_get_string(reference, "run_id")) ||
      !text_is(external, "execution_id", json_get_string(reference, "execution_id")) ||
      !text_is(external, "attempt_record_id", json_get_string(reference, "attempt_record_id")) ||
      !text_is(external, "criteria", json_get_string(snapshot, "acceptance_criteria")) ||
      !text_is(external, "decision", decision == TURBO_AGENT_JOB_ACCEPTED ? "accepted" : "rejected") ||
      !json_get_bool(result, "ok", false)) rc = SALTS_EPROTO;
done:
  json_free(catalog); json_free(external);
  return rc;
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
  if (f->file_mode) {
    turbo_agent_job_file_verifier_config_t config;
    turbo_agent_job_verifier_t verifier;
    REQUIRE(!cmeta_fs_path_join(f->workspace_path, sizeof(f->workspace_path), directory, "workspace"));
    REQUIRE(!cmeta_fs_path_join(f->verification_path, sizeof(f->verification_path), directory, "verification"));
    if (create) { REQUIRE(!cmeta_fs_mkdir(f->workspace_path, 0700)); REQUIRE(!cmeta_fs_mkdir(f->verification_path, 0700)); }
    turbo_agent_job_file_verifier_config_init(&config);
    config.workspace_root = f->workspace_path; config.verification_root = f->verification_path;
    rc = turbo_agent_job_file_verifier_create(&config, &f->file_verifier);
    if (rc == SALTS_OK) rc = turbo_agent_job_file_verifier_callbacks(f->file_verifier, &verifier);
    if (rc == SALTS_OK) rc = turbo_agent_harness_create_verified_with_store(&harness, &verifier, &f->harness);
    if (rc != SALTS_OK) goto done;
  } else if (f->verified_mode) {
    turbo_agent_job_verifier_t verifier;
    turbo_agent_job_verifier_init(&verifier);
    verifier.source = host_source; verifier.evidence = host_evidence; verifier.user_data = f;
    rc = turbo_agent_harness_create_verified_with_store(&harness, &verifier, &f->harness);
    if (rc != SALTS_OK) goto done;
  } else {
    f->harness = turbo_agent_harness_create(&harness);
    if (!f->harness) { rc = SALTS_ENOMEM; goto done; }
  }
  f->runtime = turbo_agent_session_runtime(turbo_agent_app_session(turbo_agent_harness_app(f->harness)));
  rc = f->runtime ? SALTS_OK : SALTS_EIO;
done:
  tstr_free(journal_path); tstr_free(oracle_path);
  return rc;
}
static void fixture_close(fixture_t *f) {
  json_free(f->execution_reference);
  if (f->harness) turbo_agent_harness_release(f->harness);
  turbo_agent_job_file_verifier_destroy(f->file_verifier);
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
  if (f->job_mode) {
    json_value_t *reference = NULL, *run = NULL, *journal = NULL;
    int rc = turbo_agent_harness_execute_job_inspect_json(f->harness, "bound-job", 2,
        &config, NULL, diagnostic, status, result, certificate, &reference);
    if (rc == SALTS_OK) {
      REQUIRE(reference && text_is(reference, "job_id", "bound-job") &&
          text_is(reference, "attempt_id", ATTEMPT_ID) && text_is(reference, "thread_id", THREAD_ID) &&
          json_get_int(reference, "job_revision", -1) == 2 &&
          json_get_int(reference, "proposal_version", -1) == 1 && !json_object_get(reference, "result"));
      REQUIRE(!turbo_agent_runtime_store_get_json_status(f->runtime, "runs",
          json_get_string(reference, "run_id"), &run));
      REQUIRE(text_is(run, "status", "completed") && text_is(run, "thread_id",
          json_get_string(reference, "run_thread_id")));
      REQUIRE(!turbo_agent_runtime_store_get_json_status(f->runtime, RUN_REFERENCES,
          json_get_string(reference, "execution_id"), &journal));
      REQUIRE(text_is(journal, "phase", "committed") && text_is(journal, "attempt_record_id",
          json_get_string(reference, "attempt_record_id")) && !json_object_get(journal, "result"));
    } else REQUIRE(!reference);
    if (f->acceptance_mode && rc == SALTS_OK) {
      f->execution_reference = reference;
      reference = NULL;
    }
    json_free(reference); json_free(run); json_free(journal);
    return rc;
  }
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
  if (f->job_mode) {
    turbo_agent_job_t *job = NULL;
    json_value_t *snapshot = NULL;
    const json_value_t *proposal, *binding;
    REQUIRE(!turbo_agent_job_open(f->runtime, "bound-job", &job));
    REQUIRE(!turbo_agent_job_get_snapshot(job, &snapshot));
    proposal = json_object_get(snapshot, "proposal");
    binding = json_object_get(snapshot, "attempt");
    REQUIRE(text_is(snapshot, "state", "attempt_bound") && json_get_int(snapshot, "revision", -1) == 2 &&
        text_is(proposal, "source_json", source) && text_is(binding, "attempt_id", ATTEMPT_ID) &&
        text_is(binding, "thread_id", THREAD_ID) && !json_object_get(snapshot, "result"));
    json_free(snapshot);
    turbo_agent_job_destroy(job);
  }
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
        json_get_int(attempt, "schema_version", -1) == ATTEMPT_SCHEMA_VERSION &&
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
        json_get_int(reference, "schema_version", -1) == RUN_REFERENCE_SCHEMA_VERSION &&
        text_is(reference, "thread_id", THREAD_ID) && text_is(reference, "attempt_id", ATTEMPT_ID) &&
        text_is(reference, "plan_hash", json_get_string(attempt, "plan_hash")) &&
        !json_object_get(reference, "result");
    if (valid && crash_only) valid = text_is(reference, "phase", f->cut->run_phase);
  }
  if (!valid) fprintf(stderr,
      "%s evidence: expected phase=%s; attempts=%zu schema=%d generation=%d references=%zu\n",
      f->cut->name, phase ? phase : "absent", json_array_size(attempts),
      json_get_int(attempt, "schema_version", -1), json_get_int(attempt, "authority_generation", -1),
      json_array_size(references));
  json_free(attempts); json_free(references);
  REQUIRE(valid);
  return 0;
}

static int write_host_file(const char *directory, const char *name, const void *bytes, size_t size) {
  char path[SALTS_FS_MAX_PATH];
  cmeta_fs_buf_t buffer = cmeta_fs_buf_init((char *)bytes, size);
  REQUIRE(!cmeta_fs_path_join(path, sizeof(path), directory, name));
  return cmeta_fs_write_file(path, &buffer);
}
static json_value_t *host_file_entry(const char *path, const void *bytes, size_t size) {
  json_value_t *r = json_create_object(); char hash[65];
  if (!r || turbo_agent_job_file_verifier_digest_bytes(bytes, size, hash) ||
      turbo_runtime_json_object_set_string(r, "path", path) ||
      turbo_runtime_json_object_set_int64(r, "byte_count", (int64_t)size) ||
      turbo_runtime_json_object_set_string(r, "sha256", hash)) { json_free(r); return NULL; }
  return r;
}
static int save_host_manifest(fixture_t *f, const char *name, const json_value_t *record, char *hash) {
  size_t size;
  char *bytes = json_serialize(record, &size);
  int rc;
  REQUIRE(bytes);
  rc = write_host_file(f->verification_path, name, bytes, size);
  if (!rc && hash) rc = turbo_agent_job_file_verifier_digest_bytes(bytes, size, hash);
  json_serialize_free(bytes);
  return rc;
}
static int file_sources_init(fixture_t *f, char revision[72]) {
  char hash[65];
  json_value_t *manifest = json_create_object(), *files = json_create_array();
  REQUIRE(manifest && files);
  REQUIRE(!write_host_file(f->workspace_path, "source.bin", "a\0b", 3));
  REQUIRE(!write_host_file(f->verification_path, "check.txt", "pass\n", 5));
  REQUIRE(!turbo_agent_job_file_verifier_digest_bytes(source, strlen(source), hash));
  REQUIRE(!turbo_runtime_json_object_set_int64(manifest, "schema_version", 1) &&
      !turbo_runtime_json_object_set_string(manifest, "kind", "job_file_sources") &&
      !turbo_runtime_json_object_set_string(manifest, "job_id", "bound-job") &&
      !turbo_runtime_json_object_set_string(manifest, "source_json_sha256", hash));
  REQUIRE(!turbo_runtime_json_array_append(files, host_file_entry("source.bin", "a\0b", 3)));
  REQUIRE(!turbo_runtime_json_object_set(manifest, "files", files));
  REQUIRE(!save_host_manifest(f, "bound-job.sources.json", manifest, hash));
  (void)snprintf(revision, 72, "sha256-%s", hash);
  json_free(manifest);
  return SALTS_OK;
}
static int publish_file_evidence(fixture_t *f, const json_value_t *result) {
  turbo_agent_job_t *job = NULL;
  json_value_t *snapshot = NULL, *manifest = json_create_object(), *files = json_create_array();
  char criteria_hash[65], result_hash[65];
  REQUIRE(manifest && files && !turbo_agent_job_open(f->runtime, "bound-job", &job));
  REQUIRE(!turbo_agent_job_get_snapshot(job, &snapshot)); turbo_agent_job_destroy(job);
  REQUIRE(!turbo_agent_job_file_verifier_digest_json(json_object_get(snapshot, "acceptance_criteria"), criteria_hash));
  REQUIRE(!turbo_agent_job_file_verifier_digest_json(result, result_hash));
  REQUIRE(!turbo_runtime_json_object_set_int64(manifest, "schema_version", 1) &&
      !turbo_runtime_json_object_set_string(manifest, "kind", "job_file_evidence") &&
      !turbo_runtime_json_object_set_string(manifest, "job_id", "bound-job") &&
      !turbo_runtime_json_object_set_int64(manifest, "job_revision", 2) &&
      !turbo_runtime_json_object_set_int64(manifest, "proposal_version", 1) &&
      !turbo_runtime_json_object_set_string(manifest, "source_revision", json_get_string(json_object_get(snapshot, "proposal"), "source_revision")) &&
      !turbo_runtime_json_object_set_string(manifest, "decision", "accepted") &&
      !turbo_runtime_json_object_set_string(manifest, "evidence_ref", "host-evidence-v1") &&
      !turbo_runtime_json_object_set_string(manifest, "criteria_sha256", criteria_hash) &&
      !turbo_runtime_json_object_set_string(manifest, "result_sha256", result_hash) &&
      !turbo_runtime_json_object_set_clone(manifest, "execution_reference", f->execution_reference));
  REQUIRE(!turbo_runtime_json_array_append(files, host_file_entry("check.txt", "pass\n", 5)));
  REQUIRE(!turbo_runtime_json_object_set(manifest, "files", files));
  REQUIRE(!save_host_manifest(f, "host-evidence-v1.evidence.json", manifest, NULL));
  json_free(manifest); json_free(snapshot);
  return SALTS_OK;
}

static int exercise(fixture_t *f, const char *mode) {
  turbo_agent_compile_diagnostic_t diagnostic = {0};
  turbo_tool_status_t status = TURBO_TOOL_ERROR;
  json_value_t *result = NULL, *certificate = NULL;
  int rc = FIXTURE_FAILURE, operation = SALTS_EIO;
  if (!strcmp(mode, "init")) {
    if (f->job_mode) {
      turbo_agent_job_t *job = NULL;
      char revision[72] = "fixture-source-v1";
      if (f->file_mode) REQUIRE(!file_sources_init(f, revision));
      REQUIRE(!turbo_agent_job_create(f->runtime, "bound-job", "inspect repository", "Host verifies", &job));
      REQUIRE(!turbo_agent_job_propose(job, 0, source, strlen(source), revision));
      REQUIRE(!turbo_agent_job_bind_compiled_inspect(job, 1, THREAD_ID, ATTEMPT_ID));
      turbo_agent_job_destroy(job);
    }
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
static int acceptance_operation(fixture_t *f, int write,
    turbo_agent_job_acceptance_decision_t decision, json_value_t **out) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
  if (write) return turbo_agent_harness_record_job_acceptance_json(f->harness,
      "bound-job", 2, &config, f->execution_reference, decision, "host-evidence-v1", &diagnostic, out);
  return turbo_agent_harness_get_job_acceptance_json(f->harness, "bound-job", 2,
      &config, &diagnostic, out);
}
static int exercise_acceptance(fixture_t *f, const char *mode) {
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_tool_status_t status;
  json_value_t *result = NULL, *certificate = NULL, *record = NULL, *runs = NULL;
  int rc;
  if (!strcmp(mode, "init")) return exercise(f, mode);
  REQUIRE(!grant(f, strcmp(mode, "denied") != 0));
  if (!strcmp(mode, "crash")) {
    REQUIRE(!execute(f, source, &diagnostic, &status, &result, &certificate));
    REQUIRE(result_valid(f, result) && certificate && f->execution_reference);
    /* Persist the Host's input metadata in the independent oracle before the
     * acceptance cut. It contains no typed tool result or fabricated decision. */
    REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, "host_acceptance_input",
        "candidate", f->execution_reference));
    json_free(result); json_free(certificate);
    f->inject = 1;
    rc = acceptance_operation(f, 1, TURBO_AGENT_JOB_ACCEPTED, &record);
    if (!strcmp(f->cut->name, "acceptance_lost_ack")) {
      REQUIRE(rc == SALTS_EIO && !record);
      _Exit(87);
    }
    REQUIRE(!strcmp(f->cut->name, "acceptance_returned") && rc == SALTS_OK && record);
    _Exit(FIXTURE_CRASH);
  }
  REQUIRE(!turbo_agent_runtime_store_get_json_status(f->oracle, "host_acceptance_input",
      "candidate", &f->execution_reference));
  if (!strcmp(mode, "denied") || !strcmp(mode, "stale")) {
    if (!strcmp(mode, "stale")) {
      REQUIRE(!grant(f, 1)); /* Quiescent same-grant generation replacement. */
      rc = acceptance_operation(f, 1, TURBO_AGENT_JOB_ACCEPTED, &record);
    } else rc = acceptance_operation(f, 0, TURBO_AGENT_JOB_ACCEPTED, &record);
    REQUIRE(rc == SALTS_EPROTO && !record);
  } else {
    rc = acceptance_operation(f, 0, TURBO_AGENT_JOB_ACCEPTED, &record);
    if (!strcmp(mode, "inspect") && !strcmp(f->cut->name, "acceptance_before")) {
      REQUIRE(rc == SALTS_ENOENT && !record);
    } else {
      if (!strcmp(mode, "recover") && !strcmp(f->cut->name, "acceptance_before")) {
        REQUIRE(rc == SALTS_ENOENT && !record);
        REQUIRE(!acceptance_operation(f, 1, TURBO_AGENT_JOB_ACCEPTED, &record));
      } else REQUIRE(rc == SALTS_OK && record);
      REQUIRE(text_is(record, "decision", "accepted") && text_is(record, "evidence_ref", "host-evidence-v1") &&
          !json_object_get(record, "result"));
      json_free(record); record = NULL;
      if (!strcmp(mode, "recover")) {
        REQUIRE(acceptance_operation(f, 1, TURBO_AGENT_JOB_REJECTED, &record) == SALTS_EALREADY && !record);
        REQUIRE(!acceptance_operation(f, 0, TURBO_AGENT_JOB_ACCEPTED, &record));
        REQUIRE(text_is(record, "decision", "accepted"));
        json_free(record); record = NULL;
      }
    }
  }
  REQUIRE(!f->callbacks && !f->transports);
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, "runs", NULL, NULL, &runs));
  REQUIRE(json_array_size(runs) == 1); /* Recovery/review never creates a Run. */
  json_free(runs);
  return evidence(f, "committed", 1, 0, 1); /* Acceptance is not external delivery. */
}

static int delivery_operation(fixture_t *f, int op, json_value_t **out) {
  const char *allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_t config;
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_agent_compiler_config_init(&config);
  config.allowed_capabilities = allowed; config.allowed_capability_count = 1;
  if (op == 0) return turbo_agent_harness_prepare_job_delivery_json(f->harness,
      "bound-job", 2, &config, "delivery-1", "fixture-destination", "fixture-payload", &diagnostic, out);
  if (op == 2) return turbo_agent_harness_record_job_delivery_receipt_json(f->harness,
      "bound-job", 2, &config, "delivery-1", "fixture-remote-receipt", &diagnostic, out);
  return turbo_agent_harness_get_job_delivery_json(f->harness, "bound-job", 2, &config, &diagnostic, out);
}
static int remote_receipt_valid(const json_value_t *receipt) {
  return text_is(receipt, "idempotency_key", "bound-job:delivery-1") &&
      text_is(receipt, "destination_ref", "fixture-destination") &&
      text_is(receipt, "payload_ref", "fixture-payload") &&
      text_is(receipt, "receipt_ref", "fixture-remote-receipt");
}
/* Test-only destination in a separate SQLite DB, never a real network send.
 * The Host queries it during reconciliation; Runtime never invokes this path. */
static int host_send(fixture_t *f, const json_value_t *intent) {
  json_value_t *receipt = json_clone(intent);
  int rc;
  REQUIRE(receipt && text_is(intent, "idempotency_key", "bound-job:delivery-1") &&
      text_is(intent, "destination_ref", "fixture-destination") && text_is(intent, "payload_ref", "fixture-payload"));
  REQUIRE(!counter_add(f, "send_requests"));
  REQUIRE(!turbo_runtime_json_object_set_string(receipt, "receipt_ref", "fixture-remote-receipt"));
  rc = turbo_agent_runtime_store_create_json_if_absent(f->oracle, "destination_receipts", "delivery-1", receipt);
  json_free(receipt);
  REQUIRE(rc == SALTS_OK || rc == SALTS_EALREADY);
  return rc == SALTS_OK ? counter_add(f, "delivery") : SALTS_OK;
}
static int host_verification_init(fixture_t *f) {
  json_value_t *record = json_create_object();
  REQUIRE(record && !turbo_runtime_json_object_set_string(record, "source_json", source) &&
      !turbo_runtime_json_object_set_string(record, "source_revision", "fixture-source-v1"));
  REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, "host_verification", "source", record));
  json_free(record); record = json_create_object();
  REQUIRE(record && !turbo_runtime_json_object_set_string(record, "record_id", "evidence-v1"));
  REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, "host_verification", "catalog", record));
  json_free(record);
  return SALTS_OK;
}
static int publish_host_evidence(fixture_t *f) {
  /* The independent Host fixture records execution identity and its explicit
   * criterion decision; this is metadata, not another canonical result. */
  json_value_t *record = json_clone(f->execution_reference);
  REQUIRE(record && !turbo_runtime_json_object_set_string(record, "evidence_ref", "host-evidence-v1") &&
      !turbo_runtime_json_object_set_string(record, "criteria", "Host verifies") &&
      !turbo_runtime_json_object_set_string(record, "decision", "accepted"));
  REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, "host_job_evidence", "evidence-v1", record));
  json_free(record);
  return SALTS_OK;
}
static int verification_failure_stage(fixture_t *f, const char *mode) {
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_tool_status_t status;
  json_value_t *original = NULL, *changed = NULL, *record = NULL, *result = NULL, *certificate = NULL;
  int source_changed = !strcmp(mode, "verification_source_changed");
  int missing = !strcmp(mode, "verification_evidence_missing");
  const char *collection = source_changed || missing ? "host_verification" : "host_job_evidence";
  const char *id = source_changed ? "source" : missing ? "catalog" : "evidence-v1";
  const char *key = source_changed ? "source_revision" : missing ? "record_id" : "run_id";
  int expected = missing ? SALTS_ENOENT : SALTS_EPROTO;
  REQUIRE(!turbo_agent_runtime_store_get_json_status(f->oracle, collection, id, &original));
  changed = json_clone(original);
  REQUIRE(changed && !turbo_runtime_json_object_set_string(changed, key, "unrelated-or-missing"));
  REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, collection, id, changed));
  json_free(changed);
  for (int op = 0; op < 3; ++op) {
    REQUIRE(delivery_operation(f, op, &record) == expected && !record);
  }
  REQUIRE(acceptance_operation(f, 0, TURBO_AGENT_JOB_ACCEPTED, &record) == expected && !record);
  if (source_changed) {
    REQUIRE(execute(f, source, &diagnostic, &status, &result, &certificate) == expected);
    REQUIRE(!result && !certificate && !f->execution_reference);
  }
  /* Test Host explicitly restores its external view; no production API repairs
   * or writes these independently owned manifests. */
  REQUIRE(!turbo_agent_runtime_store_put_json(f->oracle, collection, id, original));
  json_free(original);
  return SALTS_OK;
}
static int file_failure_stage(fixture_t *f, const char *mode) {
  int source_changed = !strcmp(mode, "verification_source_changed");
  int missing = !strcmp(mode, "verification_evidence_missing");
  const char *directory = source_changed ? f->workspace_path : f->verification_path;
  const char *name = source_changed ? "source.bin" : missing ? "host-evidence-v1.evidence.json" : "check.txt";
  cmeta_fs_buf_t original = {0};
  char path[SALTS_FS_MAX_PATH];
  json_value_t *record = NULL, *result = NULL, *certificate = NULL;
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_tool_status_t status;
  int expected = missing ? SALTS_ENOENT : SALTS_EPROTO;
  REQUIRE(!cmeta_fs_path_join(path, sizeof(path), directory, name));
  REQUIRE(!cmeta_fs_read_file(path, &original));
  if (missing) REQUIRE(!cmeta_fs_unlink(path));
  else REQUIRE(!write_host_file(directory, name, source_changed ? "abc" : "fail\n", source_changed ? 3 : 5));
  for (int op = 0; op < 3; ++op) REQUIRE(delivery_operation(f, op, &record) == expected && !record);
  REQUIRE(acceptance_operation(f, 0, TURBO_AGENT_JOB_ACCEPTED, &record) == expected && !record);
  if (source_changed) {
    REQUIRE(execute(f, source, &diagnostic, &status, &result, &certificate) == expected);
    REQUIRE(!result && !certificate && !f->execution_reference);
  }
  /* Only test Host restores the independently owned file after observing refusal. */
  REQUIRE(!cmeta_fs_write_file(path, &original)); cmeta_fs_buf_free(&original);
  return SALTS_OK;
}
static int exercise_delivery(fixture_t *f, const char *mode) {
  turbo_agent_compile_diagnostic_t diagnostic;
  turbo_tool_status_t status;
  json_value_t *record = NULL, *result = NULL, *certificate = NULL, *remote = NULL, *runs = NULL;
  int intent_cut = !strncmp(f->cut->name, "delivery_intent_", 16);
  int receipt_committed = !strcmp(f->cut->name, "delivery_receipt_after") ||
      !strcmp(f->cut->name, "delivery_receipt_lost_ack") || !strcmp(f->cut->name, "delivery_receipt_returned");
  int deliveries = !strcmp(mode, "recover") || !strcmp(mode, "replay") || !intent_cut;
  int rc, n;
  if (!strcmp(mode, "init")) {
    REQUIRE(!exercise(f, mode));
    if (f->verified_mode) REQUIRE(!host_verification_init(f));
    return counter_put(f, "send_requests", 0);
  }
  REQUIRE(!grant(f, strcmp(mode, "denied") != 0));
  if (!strcmp(mode, "crash")) {
    REQUIRE(!execute(f, source, &diagnostic, &status, &result, &certificate));
    REQUIRE(result_valid(f, result) && certificate && f->execution_reference);
    if (f->file_mode) REQUIRE(!publish_file_evidence(f, result));
    json_free(result); json_free(certificate);
    if (f->verified_mode) REQUIRE(!publish_host_evidence(f));
    REQUIRE(!acceptance_operation(f, 1, TURBO_AGENT_JOB_ACCEPTED, &record));
    json_free(record); record = NULL;
    f->inject = 1;
    rc = delivery_operation(f, 0, &record);
    if (!strcmp(f->cut->name, "delivery_intent_lost_ack")) {
      REQUIRE(rc == SALTS_EIO && !record); _Exit(87);
    }
    REQUIRE(rc == SALTS_OK && record);
    if (!strcmp(f->cut->name, "delivery_intent_returned")) _Exit(FIXTURE_CRASH);
    REQUIRE(!host_send(f, record));
    json_free(record); record = NULL;
    if (!strcmp(f->cut->name, "delivery_after_send")) _Exit(FIXTURE_CRASH);
    rc = delivery_operation(f, 2, &record);
    if (!strcmp(f->cut->name, "delivery_receipt_lost_ack")) {
      REQUIRE(rc == SALTS_EIO && !record); _Exit(87);
    }
    REQUIRE(!strcmp(f->cut->name, "delivery_receipt_returned") && rc == SALTS_OK && record);
    _Exit(FIXTURE_CRASH);
  }
  if ((f->verified_mode || f->file_mode) && !strncmp(mode, "verification_", 13)) {
    REQUIRE(!(f->file_mode ? file_failure_stage(f, mode) : verification_failure_stage(f, mode)));
  } else if (!strcmp(mode, "denied") || !strcmp(mode, "stale")) {
    if (!strcmp(mode, "stale")) REQUIRE(!grant(f, 1));
    for (int op = 0; op < 3; ++op) {
      REQUIRE(delivery_operation(f, op, &record) == SALTS_EPROTO && !record);
    }
  } else {
    rc = delivery_operation(f, 1, &record);
    if (!strcmp(mode, "inspect") && !strcmp(f->cut->name, "delivery_intent_before")) {
      REQUIRE(rc == SALTS_ENOENT && !record);
    } else {
      if (!strcmp(mode, "recover") && !strcmp(f->cut->name, "delivery_intent_before")) {
        REQUIRE(rc == SALTS_ENOENT && !record);
        REQUIRE(!delivery_operation(f, 0, &record));
        json_free(record); record = NULL;
        REQUIRE(!delivery_operation(f, 1, &record));
      } else REQUIRE(rc == SALTS_OK && record);
      REQUIRE(text_is(record, "status", (!strcmp(mode, "replay") || receipt_committed) ? "delivered" : "outcome_unknown"));
      if (!strcmp(mode, "recover") && !receipt_committed) {
        /* Explicit Host reconciliation: query destination before choosing send.
         * A post-send crash must find its remote receipt and issue no new send. */
        rc = turbo_agent_runtime_store_get_json_status(f->oracle, "destination_receipts", "delivery-1", &remote);
        if (rc == SALTS_ENOENT) {
          REQUIRE(!host_send(f, json_object_get(record, "intent")));
          REQUIRE(!turbo_agent_runtime_store_get_json_status(f->oracle, "destination_receipts", "delivery-1", &remote));
        } else REQUIRE(rc == SALTS_OK);
        REQUIRE(remote_receipt_valid(remote));
        json_free(remote); remote = NULL;
        json_free(record); record = NULL;
        REQUIRE(!delivery_operation(f, 2, &record));
        json_free(record); record = NULL;
        REQUIRE(!delivery_operation(f, 1, &record) && text_is(record, "status", "delivered"));
      }
      json_free(record); record = NULL;
      if (!strcmp(mode, "recover") || !strcmp(mode, "replay")) {
        REQUIRE(delivery_operation(f, 0, &record) == SALTS_EALREADY && !record);
        REQUIRE(delivery_operation(f, 2, &record) == SALTS_EALREADY && !record);
      }
    }
  }
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, "runs", NULL, NULL, &runs));
  REQUIRE(json_array_size(runs) == 1 && !f->callbacks && !f->transports);
  json_free(runs);
  REQUIRE(!counter_get(f, "send_requests", &n) && n == deliveries);
  rc = turbo_agent_runtime_store_get_json_status(f->oracle, "destination_receipts", "delivery-1", &remote);
  REQUIRE(deliveries ? rc == SALTS_OK && remote_receipt_valid(remote) : rc == SALTS_ENOENT && !remote);
  json_free(remote);
  return evidence(f, "committed", 1, deliveries, 1);
}

int main(int argc, char **argv) {
  fixture_t fixture = {0};
  int rc;
  if (argc != 4 && argc != 5) return failure("mode/cut/private-directory/[job] required", __LINE__);
  if (argc == 5) {
    if (strcmp(argv[4], "job") && strcmp(argv[4], "acceptance") && strcmp(argv[4], "delivery") && strcmp(argv[4], "verified") && strcmp(argv[4], "files"))
      return failure("unknown execution mode", __LINE__);
    fixture.job_mode = 1;
    fixture.verified_mode = !strcmp(argv[4], "verified");
    fixture.file_mode = !strcmp(argv[4], "files");
    fixture.delivery_mode = !strcmp(argv[4], "delivery") || fixture.verified_mode || fixture.file_mode;
    fixture.acceptance_mode = !strcmp(argv[4], "acceptance") || fixture.delivery_mode;
  }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    if (!strcmp(argv[2], cases[i].name)) fixture.cut = &cases[i];
  if (!fixture.cut) return failure("unknown crash cut", __LINE__);
  rc = fixture_open(&fixture, argv[3], !strcmp(argv[1], "init"));
  if (!rc) rc = fixture.delivery_mode ? exercise_delivery(&fixture, argv[1]) :
      fixture.acceptance_mode ? exercise_acceptance(&fixture, argv[1]) : exercise(&fixture, argv[1]);
  else fprintf(stderr, "Compiled Inspect fixture open failed: %d\n", rc);
  fixture_close(&fixture);
  return rc ? FIXTURE_FAILURE : 0;
}
