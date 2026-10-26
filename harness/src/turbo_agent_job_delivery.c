#include "turbo_agent_job_delivery.h"
#include "turbo_agent_job.h"
#include "turbo_agent_harness_internal.h"

#include <stdio.h>
#include <string.h>

#define INTENTS "agent_job_delivery_intents"
#define RECEIPTS "agent_job_delivery_receipts"

typedef enum delivery_operation_e { PREPARE, READ, RECEIPT } delivery_operation_t;

static int bounded(const char *s, size_t max) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= max; ++i) if (!s[i]) return 1;
  return 0;
}
static int id_valid(const char *s, size_t max) {
  size_t i;
  if (!bounded(s, max)) return 0;
  for (i = 0; s[i]; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 1;
}
static const char *text(const json_value_t *r, const char *key, size_t max) {
  const json_value_t *v = json_object_get(r, key);
  const char *s;
  if (!v || json_type(v) != JSON_STRING || !json_string_len(v) || json_string_len(v) > max) return NULL;
  s = json_string(v);
  return s && strlen(s) == json_string_len(v) ? s : NULL;
}
static int is(const json_value_t *r, const char *key, const char *expected) {
  const char *s = expected ? text(r, key, strlen(expected)) : NULL;
  return s && !strcmp(s, expected);
}
static int64_t number(const json_value_t *r, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(r, key), -1);
}
static int schema(const json_value_t *r) {
  int64_t version;
  if (!r || json_type(r) != JSON_OBJECT) return SALTS_EPROTO;
  version = number(r, "schema_version");
  return version == 1 ? SALTS_OK : version > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
}
static int current(const turbo_agent_job_proof_t *proof) {
  return turbo_agent_tool_authority_snapshot_is_current(proof->agent, &proof->authority) == 1 ?
      SALTS_OK : SALTS_EPROTO;
}
static int intent_valid(const json_value_t *r, const json_value_t *acceptance) {
  const char *job = text(acceptance, "job_id", 64);
  const char *delivery = text(r, "delivery_id", 64);
  char key[130];
  int rc = schema(r);
  if (rc != SALTS_OK) return rc;
  if (!job || !id_valid(delivery, 64)) return SALTS_EPROTO;
  (void)snprintf(key, sizeof(key), "%s:%s", job, delivery);
  return json_object_size(r) == 9 && is(r, "kind", "job_delivery_intent") &&
      is(r, "job_id", job) && number(r, "job_revision") == number(acceptance, "job_revision") &&
      number(r, "proposal_version") == number(acceptance, "proposal_version") &&
      is(r, "idempotency_key", key) && text(r, "destination_ref", 1024) && text(r, "payload_ref", 1024) ?
      SALTS_OK : SALTS_EPROTO;
}
static int receipt_valid(const json_value_t *r, const json_value_t *intent) {
  int rc = schema(r);
  if (rc != SALTS_OK) return rc;
  return json_object_size(r) == 7 && is(r, "kind", "host_job_delivery_receipt") &&
      is(r, "job_id", text(intent, "job_id", 64)) &&
      number(r, "job_revision") == number(intent, "job_revision") &&
      is(r, "delivery_id", text(intent, "delivery_id", 64)) &&
      is(r, "idempotency_key", text(intent, "idempotency_key", 129)) && text(r, "receipt_ref", 1024) ?
      SALTS_OK : SALTS_EPROTO;
}
/* A receipt cannot repair a missing intent. Preserve independent backend errors
 * and fail closed on orphan records before any new publication. */
static int load_delivery(const turbo_agent_job_proof_t *proof, const json_value_t *acceptance,
    json_value_t **intent, json_value_t **receipt) {
  const char *id = json_get_string(acceptance, "job_id");
  int rc = turbo_agent_runtime_store_get_json_status(proof->runtime, RECEIPTS, id, receipt);
  int intent_rc;
  if (rc != SALTS_OK && rc != SALTS_ENOENT) return rc;
  intent_rc = turbo_agent_runtime_store_get_json_status(proof->runtime, INTENTS, id, intent);
  if (intent_rc == SALTS_ENOENT) return *receipt ? SALTS_EPROTO : SALTS_ENOENT;
  if (intent_rc != SALTS_OK) return intent_rc;
  rc = intent_valid(*intent, acceptance);
  if (rc == SALTS_OK && *receipt) rc = receipt_valid(*receipt, *intent);
  return rc;
}
static json_value_t *base_record(const char *kind, const char *id, uint32_t revision,
    const char *delivery, const char *key) {
  json_value_t *r = json_create_object();
  if (!r || turbo_runtime_json_object_set_int64(r, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(r, "kind", kind) ||
      turbo_runtime_json_object_set_string(r, "job_id", id) ||
      turbo_runtime_json_object_set_int64(r, "job_revision", revision) ||
      turbo_runtime_json_object_set_string(r, "delivery_id", delivery) ||
      turbo_runtime_json_object_set_string(r, "idempotency_key", key)) {
    json_free(r); return NULL;
  }
  return r;
}
static int operate(turbo_agent_harness_t *harness, const char *id, uint32_t revision,
    const turbo_agent_compiler_config_t *config, delivery_operation_t op,
    const char *delivery, const char *destination, const char *payload, const char *receipt_ref,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  turbo_agent_job_proof_t proof = {0};
  json_value_t *acceptance = NULL, *intent = NULL, *receipt = NULL, *record = NULL;
  char key[130];
  int rc;
  if (out) *out = NULL;
  if (diagnostic) memset(diagnostic, 0, sizeof(*diagnostic));
  if (!out || !harness || !id_valid(id, 64) || revision > TURBO_AGENT_JOB_MAX_REVISIONS ||
      !config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_COMPILER_CONFIG_ABI_VERSION || !config->deny_unlisted_capabilities ||
      (config->allowed_capability_count && !config->allowed_capabilities) ||
      (op != READ && !id_valid(delivery, 64)) ||
      (op == PREPARE && (!bounded(destination, 1024) || !bounded(payload, 1024))) ||
      (op == RECEIPT && !bounded(receipt_ref, 1024))) return SALTS_EINVAL;
  rc = turbo_agent_harness_begin_submit(harness);
  if (rc != SALTS_OK) return rc;
  rc = turbo_agent_harness_prepare_job_proof(harness, id, revision, op == PREPARE, config, diagnostic, &proof);
  if (rc != SALTS_OK) goto done;
  rc = turbo_agent_harness_load_job_acceptance(&proof, diagnostic, &acceptance);
  if (rc != SALTS_OK) goto done;
  if (!is(acceptance, "decision", "accepted")) { rc = SALTS_EPERM; goto done; }
  rc = load_delivery(&proof, acceptance, &intent, &receipt);
  if (op == PREPARE) {
    if (rc == SALTS_OK) { rc = SALTS_EALREADY; goto done; }
    if (rc != SALTS_ENOENT) goto done;
    (void)snprintf(key, sizeof(key), "%s:%s", id, delivery);
    record = base_record("job_delivery_intent", id, revision, delivery, key);
    rc = SALTS_ENOMEM;
    if (!record || turbo_runtime_json_object_set_int64(record, "proposal_version", number(acceptance, "proposal_version")) ||
        turbo_runtime_json_object_set_string(record, "destination_ref", destination) ||
        turbo_runtime_json_object_set_string(record, "payload_ref", payload)) goto done;
  } else {
    if (rc != SALTS_OK) goto done;
    if (op == RECEIPT) {
      if (!is(intent, "delivery_id", delivery)) { rc = SALTS_EPROTO; goto done; }
      if (receipt) { rc = SALTS_EALREADY; goto done; }
      record = base_record("host_job_delivery_receipt", id, revision, delivery,
          text(intent, "idempotency_key", 129));
      rc = SALTS_ENOMEM;
      if (!record || turbo_runtime_json_object_set_string(record, "receipt_ref", receipt_ref)) goto done;
    } else {
      record = json_create_object();
      rc = SALTS_ENOMEM;
      if (!record || turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
          turbo_runtime_json_object_set_string(record, "kind", "job_delivery_state") ||
          turbo_runtime_json_object_set_string(record, "status", receipt ? "delivered" : "outcome_unknown") ||
          turbo_runtime_json_object_set_clone(record, "intent", intent) ||
          (receipt && turbo_runtime_json_object_set_clone(record, "receipt", receipt))) goto done;
    }
  }
  rc = current(&proof);
  if (rc != SALTS_OK) goto done;
  if (op == PREPARE) {
    rc = turbo_agent_harness_require_job_active(proof.runtime, proof.snapshot, diagnostic);
    if (rc != SALTS_OK) goto done;
  }
  if (op != READ) {
    rc = turbo_agent_runtime_store_create_json_if_absent(proof.runtime, op == PREPARE ? INTENTS : RECEIPTS, id, record);
    if (rc != SALTS_OK) goto done;
  }
  rc = current(&proof);
  if (rc == SALTS_OK && op == PREPARE)
    rc = turbo_agent_harness_require_job_active(proof.runtime, proof.snapshot, diagnostic);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
done:
  if (rc != SALTS_OK && diagnostic && diagnostic->status == TURBO_AGENT_COMPILE_OK) {
    diagnostic->status = rc == SALTS_ENOMEM ? TURBO_AGENT_COMPILE_OUT_OF_MEMORY : TURBO_AGENT_COMPILE_SOURCE_INVALID;
    (void)snprintf(diagnostic->message, sizeof(diagnostic->message),
        "Job delivery rejected or unacknowledged (status %d); reconcile explicitly", rc);
  }
  json_free(record); json_free(intent); json_free(receipt); json_free(acceptance);
  turbo_agent_harness_job_proof_destroy(&proof);
  turbo_agent_harness_end_submit(harness);
  return rc;
}

int turbo_agent_harness_prepare_job_delivery_json(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    const char *delivery, const char *destination, const char *payload,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  return operate(harness, id, revision, config, PREPARE, delivery, destination, payload, NULL, diagnostic, out);
}
int turbo_agent_harness_get_job_delivery_json(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  return operate(harness, id, revision, config, READ, NULL, NULL, NULL, NULL, diagnostic, out);
}
int turbo_agent_harness_record_job_delivery_receipt_json(turbo_agent_harness_t *harness,
    const char *id, uint32_t revision, const turbo_agent_compiler_config_t *config,
    const char *delivery, const char *receipt_ref,
    turbo_agent_compile_diagnostic_t *diagnostic, json_value_t **out) {
  return operate(harness, id, revision, config, RECEIPT, delivery, NULL, NULL, receipt_ref, diagnostic, out);
}
