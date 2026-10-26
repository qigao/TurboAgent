#include "turbo_agent_job_acceptance.h"
#include "turbo_agent_job.h"
#include "turbo_agent_harness_internal.h"
#include "turbo_tool_schema.h"

#include <stdio.h>
#include <string.h>

#define ACCEPTANCE "agent_job_acceptance"
#define ATTEMPTS "compiled_inspect_attempts"
#define JOURNAL "agent_compiled_execution_journal"

static const char *text(const json_value_t *record, const char *key, size_t max) {
  const json_value_t *value = json_object_get(record, key);
  const char *s;
  if (!value || json_type(value) != JSON_STRING || !json_string_len(value) ||
      json_string_len(value) > max) return NULL;
  s = json_string(value);
  return s && strlen(s) == json_string_len(value) ? s : NULL;
}
static int is(const json_value_t *record, const char *key, const char *expected) {
  const char *s;
  if (!expected) return 0;
  s = text(record, key, strlen(expected));
  return s && !strcmp(s, expected);
}
static int id_valid(const char *s, size_t max) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= max; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!c) return 1;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 0;
}
static int64_t number(const json_value_t *record, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(record, key), -1);
}
static int schema(const json_value_t *record, int version) {
  int64_t n;
  if (!record || json_type(record) != JSON_OBJECT) return SALTS_EPROTO;
  n = number(record, "schema_version");
  return n == version ? SALTS_OK : n > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
}
static int json_equal(const json_value_t *left, const json_value_t *right) {
  char *a, *b;
  int same;
  if (!left || !right) return 0;
  a = json_serialize(left, NULL); b = json_serialize(right, NULL);
  same = !a || !b ? -1 : !strcmp(a, b);
  json_serialize_free(a); json_serialize_free(b);
  return same;
}
int turbo_agent_harness_job_reference_shape(const json_value_t *r) {
  const char *keys[] = {"job_id", "thread_id", "attempt_id", "attempt_record_id",
      "run_id", "run_thread_id", "execution_id", "plan_hash"};
  const size_t bounds[] = {64, 80, 64, 149, 128, 128, 128, 16};
  size_t i;
  int rc = schema(r, 1);
  if (rc != SALTS_OK) return rc;
  if (json_object_size(r) != 13 || !is(r, "kind", "compiled_inspect") ||
      number(r, "job_revision") < 1 || number(r, "job_revision") > TURBO_AGENT_JOB_MAX_REVISIONS ||
      number(r, "proposal_version") < 1 || number(r, "authority_generation") < 0) return SALTS_EPROTO;
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
    if (!id_valid(text(r, keys[i], bounds[i]), bounds[i])) return SALTS_EPROTO;
  return SALTS_OK;
}
static int identity(const json_value_t *record, const json_value_t *reference,
    const turbo_agent_job_proof_t *proof) {
  return is(record, "thread_id", text(reference, "thread_id", 80)) &&
      is(record, "attempt_id", text(reference, "attempt_id", 64)) &&
      is(record, "plan_hash", proof->plan_hash) &&
      is(record, "tool_name", proof->admission.name) &&
      number(record, "authority_generation") == (int64_t)proof->authority.generation;
}
static int validate_reference_impl(const turbo_agent_job_proof_t *proof, const json_value_t *reference,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out_result) {
  json_value_t *attempt = NULL, *journal = NULL, *run = NULL;
  const json_value_t *binding = json_object_get(proof->snapshot, "attempt");
  const json_value_t *proposal = json_object_get(proof->snapshot, "proposal");
  const json_value_t *execution;
  const char *thread = text(binding, "thread_id", 80);
  const char *attempt_id = text(binding, "attempt_id", 64);
  const char *stage = "canonical attempt";
  char key[160];
  int equal, rc = turbo_agent_harness_job_reference_shape(reference);
  if (out_result) *out_result = NULL;
  if (rc != SALTS_OK) return rc;
  if (!is(reference, "job_id", json_get_string(proof->snapshot, "id")) ||
      number(reference, "job_revision") != number(proof->snapshot, "revision") ||
      number(reference, "proposal_version") != number(proposal, "revision") ||
      !is(reference, "thread_id", thread) || !is(reference, "attempt_id", attempt_id) ||
      !is(reference, "plan_hash", proof->plan_hash) ||
      number(reference, "authority_generation") != (int64_t)proof->authority.generation) return SALTS_EPROTO;
  (void)snprintf(key, sizeof(key), "%03u_%s_%s", (unsigned)strlen(thread), thread, attempt_id);
  if (!is(reference, "attempt_record_id", key)) return SALTS_EPROTO;
  rc = turbo_agent_runtime_store_get_json_status(proof->runtime, ATTEMPTS, key, &attempt);
  if (rc != SALTS_OK) goto done;
  rc = schema(attempt, 2);
  if (rc != SALTS_OK) goto done;
  if (is(attempt, "phase", "started") || is(attempt, "phase", "outcome_unknown")) {
    rc = SALTS_EBUSY; goto done;
  }
  rc = SALTS_EPROTO;
  if (!identity(attempt, reference, proof) || !is(attempt, "id", key) ||
      !is(attempt, "phase", "committed") || number(attempt, "tool_status") != TURBO_TOOL_OK ||
      !is(attempt, "backend", "runtime_tools") || !is(attempt, "execution_mode", "compiled_inspect") ||
      !is(attempt, "result_schema", proof->admission.result_schema) ||
      number(attempt, "effects") != (int64_t)proof->admission.effects ||
      turbo_tool_schema_validate_result_json_value(proof->admission.approved, proof->admission.name,
          json_object_get(attempt, "result"), NULL, 0) != TURBO_TOOL_SCHEMA_VALID) goto done;
  equal = json_equal(json_object_get(attempt, "certificate"), proof->admission.certificate);
  if (equal != 1) { rc = equal < 0 ? SALTS_ENOMEM : SALTS_EPROTO; goto done; }
  stage = "compiled journal";
  rc = turbo_agent_runtime_store_get_json_status(proof->runtime, JOURNAL,
      text(reference, "execution_id", 128), &journal);
  if (rc != SALTS_OK) goto done;
  rc = schema(journal, 1);
  if (rc != SALTS_OK) goto done;
  rc = SALTS_EPROTO;
  if (!identity(journal, reference, proof) ||
      !is(journal, "execution_id", text(reference, "execution_id", 128)) ||
      !is(journal, "attempt_record_id", key) || !is(journal, "phase", "committed") ||
      number(journal, "tool_status") != TURBO_TOOL_OK || json_object_get(journal, "result")) goto done;
  equal = json_equal(json_object_get(journal, "certificate"), proof->admission.certificate);
  if (equal != 1) { rc = equal < 0 ? SALTS_ENOMEM : SALTS_EPROTO; goto done; }
  stage = "Runtime Run";
  rc = turbo_agent_runtime_store_get_json_status(proof->runtime, "runs", text(reference, "run_id", 128), &run);
  if (rc != SALTS_OK) goto done;
  rc = SALTS_EPROTO;
  if (!is(run, "id", text(reference, "run_id", 128)) || !is(run, "status", "completed") ||
      !is(run, "thread_id", text(reference, "run_thread_id", 128)) ||
      !is(run, "graph_name", "turbo-agent-compiled-inspect-v1")) goto done;
  stage = "Runtime Run execution snapshot";
  execution = json_object_get(json_object_get(run, "state_snapshot"), "compiled_execution");
  rc = schema(execution, 1);
  if (rc != SALTS_OK) goto done;
  rc = SALTS_EPROTO;
  if (!identity(execution, reference, proof) ||
      !is(execution, "execution_id", text(reference, "execution_id", 128)) ||
      !is(execution, "attempt_record_id", key)) goto done;
  equal = json_equal(json_object_get(execution, "certificate"), proof->admission.certificate);
  rc = equal == 1 ? SALTS_OK : equal < 0 ? SALTS_ENOMEM : SALTS_EPROTO;
  if (rc == SALTS_OK) {
    if (out_result) {
      stage = "canonical result allocation";
      *out_result = json_clone(json_object_get(attempt, "result"));
      if (!*out_result) rc = SALTS_ENOMEM;
    } else {
      stage = "Host evidence";
      rc = turbo_agent_harness_verify_job_evidence(proof, reference,
          json_object_get(attempt, "result"), decision, evidence_ref, diagnostic);
    }
  }
done:
  if (rc != SALTS_OK && diagnostic && diagnostic->status == TURBO_AGENT_COMPILE_OK) {
    diagnostic->status = rc == SALTS_ENOMEM ? TURBO_AGENT_COMPILE_OUT_OF_MEMORY : TURBO_AGENT_COMPILE_SOURCE_INVALID;
    (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
        "Job acceptance reference validation failed at %s (status %d)", stage, rc);
  }
  json_free(attempt); json_free(journal); json_free(run);
  return rc;
}
static int validate_reference(const turbo_agent_job_proof_t *proof, const json_value_t *reference,
    turbo_agent_job_acceptance_decision_t decision, const char *evidence_ref,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  return validate_reference_impl(proof, reference, decision, evidence_ref, diagnostic, NULL);
}
int turbo_agent_harness_load_job_result(const turbo_agent_job_proof_t *proof,
    const json_value_t *reference, turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  return validate_reference_impl(proof, reference, TURBO_AGENT_JOB_ACCEPTED, NULL, diagnostic, out);
}
static int config_valid(const turbo_agent_compiler_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
      config->abi_version == TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION && config->deny_unlisted_capabilities &&
      (!config->allowed_capability_count || config->allowed_capabilities);
}
static int current(const turbo_agent_job_proof_t *proof) {
  return turbo_agent_tool_authority_snapshot_is_current(proof->agent, &proof->authority) == 1 ?
      SALTS_OK : SALTS_EPROTO;
}
static int record_shape(const json_value_t *record, const char *id, uint32_t revision) {
  int rc = schema(record, 1);
  if (rc != SALTS_OK) return rc;
  return json_object_size(record) == 8 && is(record, "kind", "host_job_acceptance") &&
      is(record, "job_id", id) && number(record, "job_revision") == revision &&
      number(record, "proposal_version") == number(json_object_get(record, "execution_reference"), "proposal_version") &&
      (is(record, "decision", "accepted") || is(record, "decision", "rejected")) &&
      text(record, "evidence_ref", 1024) ? SALTS_OK : SALTS_EPROTO;
}
static void diagnose(turbo_agent_compile_diagnostic_t *diagnostic, int rc) {
  if (!diagnostic || !rc || diagnostic->status != TURBO_AGENT_COMPILE_OK) return;
  diagnostic->status = rc == SALTS_ENOMEM ? TURBO_AGENT_COMPILE_OUT_OF_MEMORY : TURBO_AGENT_COMPILE_SOURCE_INVALID;
  (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
      "Job acceptance rejected or unacknowledged (status %d); no retry was attempted", rc);
}

int turbo_agent_harness_record_job_acceptance_json(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    const json_value_t *reference, turbo_agent_job_acceptance_decision_t decision,
    const char *evidence_ref, turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  turbo_agent_job_proof_t proof = {0};
  json_value_t *record = NULL;
  size_t evidence_size = 0;
  int rc;
  if (out) *out = NULL;
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
  if (evidence_ref) while (evidence_size <= 1024 && evidence_ref[evidence_size]) ++evidence_size;
  if (!out || !harness || !id_valid(id, 64) || revision > TURBO_AGENT_JOB_MAX_REVISIONS ||
      !config_valid(config) || !reference || !evidence_size || evidence_size > 1024 ||
      (decision != TURBO_AGENT_JOB_ACCEPTED && decision != TURBO_AGENT_JOB_REJECTED)) return SALTS_EINVAL;
  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) return rc;
  rc = turbo_agent_harness_prepare_job_proof(harness, id, revision, 1, config, diagnostic, &proof);
  if (rc != SALTS_OK) goto done;
  rc = validate_reference(&proof, reference, decision, evidence_ref, diagnostic);
  if (rc != SALTS_OK) goto done;
  record = json_create_object();
  rc = SALTS_ENOMEM;
  if (!record || turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(record, "kind", "host_job_acceptance") ||
      turbo_runtime_json_object_set_string(record, "job_id", id) ||
      turbo_runtime_json_object_set_int64(record, "job_revision", revision) ||
      turbo_runtime_json_object_set_int64(record, "proposal_version", number(reference, "proposal_version")) ||
      turbo_runtime_json_object_set_string(record, "decision", decision == TURBO_AGENT_JOB_ACCEPTED ? "accepted" : "rejected") ||
      turbo_runtime_json_object_set_string(record, "evidence_ref", evidence_ref) ||
      turbo_runtime_json_object_set_clone(record, "execution_reference", reference)) goto done;
  rc = current(&proof);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_harness_require_job_active(proof.runtime, proof.snapshot, diagnostic);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_runtime_store_create_json_if_absent(proof.runtime, ACCEPTANCE, id, record);
  if (rc != SALTS_OK) goto done;
  rc = current(&proof);
  if (rc == SALTS_OK) rc = turbo_agent_harness_require_job_active(proof.runtime, proof.snapshot, diagnostic);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
done:
  json_free(record);
  diagnose(diagnostic, rc);
  turbo_agent_harness_job_proof_destroy(&proof);
  turbo_agent_harness_end_submit(harness);
  return rc;
}
int turbo_agent_harness_load_job_acceptance(const turbo_agent_job_proof_t *proof,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  const char *id = json_get_string(proof->snapshot, "id");
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(proof->runtime, ACCEPTANCE, id, &record);
  *out = NULL;
  if (rc == SALTS_OK) rc = record_shape(record, id, (uint32_t)number(proof->snapshot, "revision"));
  if (rc == SALTS_OK) rc = validate_reference(proof, json_object_get(record, "execution_reference"),
      is(record, "decision", "accepted") ? TURBO_AGENT_JOB_ACCEPTED : TURBO_AGENT_JOB_REJECTED,
      text(record, "evidence_ref", 1024), diagnostic);
  if (rc == SALTS_OK) rc = current(proof);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
  json_free(record);
  return rc;
}

int turbo_agent_harness_get_job_acceptance_json(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  turbo_agent_job_proof_t proof = {0};
  json_value_t *record = NULL;
  int rc;
  if (out) *out = NULL;
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
  if (!out || !harness || !id_valid(id, 64) || revision > TURBO_AGENT_JOB_MAX_REVISIONS || !config_valid(config))
    return SALTS_EINVAL;
  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) return rc;
  rc = turbo_agent_harness_prepare_job_proof(harness, id, revision, 0, config, diagnostic, &proof);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_harness_load_job_acceptance(&proof, diagnostic, &record);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
done:
  json_free(record);
  diagnose(diagnostic, rc);
  turbo_agent_harness_job_proof_destroy(&proof);
  turbo_agent_harness_end_submit(harness);
  return rc;
}
