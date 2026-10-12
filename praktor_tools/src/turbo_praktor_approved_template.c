#include "turbo_praktor_approved_template.h"

#include "turbo_agent_praktor_lowering.h"
#include "turbo_runtime_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRAKTOR_APPROVAL_COLLECTION "praktor_template_approvals"
#define PRAKTOR_ATTEMPT_COLLECTION "praktor_template_attempts"
#define PRAKTOR_APPROVED_MAX_RECORD_BYTES (1024u * 1024u)
#define PRAKTOR_APPROVED_MAX_YAML_BYTES (256u * 1024u)

static int approved_key_component(const char *text, size_t max_length) {
  size_t i;
  if (!text || !text[0]) return 0;
  for (i = 0; text[i]; ++i) {
    char c = text[i];
    if (i >= max_length ||
        !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_'))
      return 0;
  }
  return i != 0;
}

static int approved_identity(const char *thread_id, const char *attempt_id,
                             const char *approval_id, const turbo_agent_template_plan_t *plan,
                             char key[192], char hash[17]) {
  int width;
  uint64_t plan_hash;
  if (!key || !hash ||
      !approved_key_component(thread_id, 80) ||
      !approved_key_component(attempt_id, 64) ||
      !approved_key_component(approval_id, 64) ||
      !plan || !turbo_agent_template_praktor_execution_preflight(plan) ||
      !(plan_hash = turbo_agent_template_plan_hash(plan)))
    return 0;
  width = snprintf(key, 192, "%03u_%s_%s", (unsigned)strlen(thread_id),
                   thread_id, attempt_id);
  if (width <= 0 || width >= 192) return 0;
  snprintf(hash, 17, "%016llx", (unsigned long long)plan_hash);
  return 1;
}

static int approved_record_equals(const json_value_t *record, const char *field,
                                  const char *expected) {
  const char *value = record ? json_get_string(record, field) : NULL;
  return value && expected && strcmp(value, expected) == 0;
}

static int approved_record_matches(const json_value_t *record,
                                   const char *key, const char *thread_id,
                                   const char *attempt_id, const char *approval_id,
                                   const char *hash) {
  return record && json_type(record) == JSON_OBJECT &&
         turbo_runtime_json_value_as_int64(
             json_object_get(record, "schema_version"), -1) == 1 &&
         approved_record_equals(record, "id", key) &&
         approved_record_equals(record, "thread_id", thread_id) &&
         approved_record_equals(record, "attempt_id", attempt_id) &&
         approved_record_equals(record, "approval_id", approval_id) &&
         approved_record_equals(record, "plan_hash", hash);
}

/* One bounded durable record; no callback pointer or raw Runtime state is
 * serialized. Only frozen logical identities and canonical JSON results. */
static int approved_store_record(
    turbo_agent_runtime_t *runtime, const char *collection,
    const char *key, const char *thread_id, const char *attempt_id,
    const char *approval_id, const char *hash, const char *phase,
    turbo_tool_status_t status, turbo_agent_template_outcome_t outcome,
    const json_value_t *result) {
  json_value_t *record = json_create_object();
  char *encoded = NULL;
  size_t bytes = 0;
  int ok = 0;
  if (!record) return 0;
  if (turbo_runtime_json_object_set_int64(record, "schema_version", 1) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "id", key) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "thread_id", thread_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "attempt_id", attempt_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "approval_id", approval_id) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "plan_hash", hash) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_string(record, "phase", phase) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "tool_status", status) !=
          TURBO_RUNTIME_JSON_OK ||
      turbo_runtime_json_object_set_int64(record, "outcome", outcome) !=
          TURBO_RUNTIME_JSON_OK ||
      (result && turbo_runtime_json_object_set_clone(
          record, "result", result) != TURBO_RUNTIME_JSON_OK))
    goto done;
  encoded = json_serialize(record, &bytes);
  if (!encoded || bytes > PRAKTOR_APPROVED_MAX_RECORD_BYTES) goto done;
  ok = turbo_agent_runtime_store_put_json(runtime, collection, key, record) == 0;
done:
  if (encoded) json_serialize_free(encoded);
  turbo_runtime_json_destroy(record);
  return ok;
}

/* Trusted host alone calls this after reviewing the same plan certificate;
 * this is an explicit approval grant, not a capability inferred from effects. */
turbo_tool_status_t turbo_praktor_template_approval_commit(
    turbo_agent_runtime_t *runtime, const turbo_agent_template_plan_t *plan,
    const char *thread_id, const char *attempt_id, const char *approval_id) {
  char key[192], hash[17];
  json_value_t *existing = NULL;
  int rc;
  if (!runtime ||
      !approved_identity(thread_id, attempt_id, approval_id, plan, key, hash))
    return TURBO_TOOL_INVALID_ARGUMENT;
  rc = turbo_agent_runtime_store_get_json_status(
      runtime, PRAKTOR_APPROVAL_COLLECTION, key, &existing);
  if (rc == SALTS_OK) {
    const int same = approved_record_matches(
        existing, key, thread_id, attempt_id, approval_id, hash) &&
        approved_record_equals(existing, "phase", "approved");
    turbo_runtime_json_destroy(existing);
    return same ? TURBO_TOOL_OK : TURBO_TOOL_INVALID_ARGUMENT;
  }
  if (rc != SALTS_ENOENT) return TURBO_TOOL_ERROR;
  return approved_store_record(
      runtime, PRAKTOR_APPROVAL_COLLECTION, key, thread_id, attempt_id,
      approval_id, hash, "approved", TURBO_TOOL_OK,
      TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT, NULL)
      ? TURBO_TOOL_OK : TURBO_TOOL_ERROR;
}

turbo_tool_status_t turbo_praktor_template_execute_approved(
    turbo_agent_runtime_t *runtime, const turbo_agent_template_plan_t *plan,
    const char *thread_id, const char *attempt_id, const char *approval_id,
    const turbo_tool_execution_context_t *context,
    int *out_replayed, turbo_agent_template_outcome_t *out_outcome,
    json_value_t **out_result) {
  char key[192], hash[17];
  json_value_t *receipt = NULL, *existing = NULL, *result = NULL;
  turbo_agent_praktor_inline_source_t *lowered = NULL;
  turbo_praktor_inline_plan_t *compiled = NULL;
  turbo_praktor_inline_plan_config_t config;
  turbo_tool_status_t status = TURBO_TOOL_INVALID_ARGUMENT;
  turbo_agent_template_outcome_t outcome = TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT;
  int rc;

  if (out_result) *out_result = NULL;
  if (out_replayed) *out_replayed = 0;
  if (out_outcome) *out_outcome = TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT;
  if (!runtime || !out_result || !out_replayed || !out_outcome ||
      !approved_identity(thread_id, attempt_id, approval_id, plan, key, hash))
    return TURBO_TOOL_INVALID_ARGUMENT;

  /* An approval must exist in this same durable RuntimeStore. Neither
   * tool arguments nor the Praktor backend may create it implicitly. */
  rc = turbo_agent_runtime_store_get_json_status(
      runtime, PRAKTOR_APPROVAL_COLLECTION, key, &receipt);
  if (rc != SALTS_OK) {
    status = rc == SALTS_ENOENT ? TURBO_TOOL_INVALID_ARGUMENT : TURBO_TOOL_ERROR;
    goto done;
  }
  if (!approved_record_matches(
          receipt, key, thread_id, attempt_id, approval_id, hash) ||
      !approved_record_equals(receipt, "phase", "approved")) {
    status = TURBO_TOOL_INVALID_ARGUMENT;
    goto done;
  }

  rc = turbo_agent_runtime_store_get_json_status(
      runtime, PRAKTOR_ATTEMPT_COLLECTION, key, &existing);
  if (rc == SALTS_OK) {
    const char *phase = json_get_string(existing, "phase");
    const json_value_t *saved_result = json_object_get(existing, "result");
    int64_t saved_status, saved_outcome;
    if (!approved_record_matches(
            existing, key, thread_id, attempt_id, approval_id, hash) ||
        !phase) goto done;
    if (strcmp(phase, "started") == 0) {
      status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
      goto done;
    }
    if (strcmp(phase, "committed") != 0) goto done;
    saved_status = turbo_runtime_json_value_as_int64(
        json_object_get(existing, "tool_status"), INT64_MIN);
    saved_outcome = turbo_runtime_json_value_as_int64(
        json_object_get(existing, "outcome"), INT64_MIN);
    if (saved_status < TURBO_TOOL_TRAPPED || saved_status > TURBO_TOOL_OK ||
        saved_outcome < TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED ||
        saved_outcome > TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED ||
        (saved_status == TURBO_TOOL_OK && (!saved_result ||
                                           json_type(saved_result) != JSON_OBJECT)) ||
        (saved_status != TURBO_TOOL_OK && saved_result))
      goto done;
    if (saved_result && !(*out_result = json_clone(saved_result))) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto done;
    }
    *out_replayed = 1;
    *out_outcome = (turbo_agent_template_outcome_t)saved_outcome;
    status = (turbo_tool_status_t)saved_status;
    goto done;
  }
  if (rc != SALTS_ENOENT) {
    status = TURBO_TOOL_ERROR;
    goto done;
  }

  /* Compile inline via the released ABI before committing STARTED.
   * This phase executes zero HostTool callbacks; failure creates no attempt. */
  if (turbo_agent_template_lower_praktor_inline(
          plan, &lowered, NULL) != TURBO_AGENT_PRAKTOR_LOWERING_OK || !lowered)
    goto done;
  if (turbo_agent_praktor_inline_source_plan_hash(lowered) !=
          turbo_agent_template_plan_hash(plan) ||
      !turbo_agent_praktor_inline_source_yaml_size(lowered) ||
      turbo_agent_praktor_inline_source_yaml_size(lowered) >
          PRAKTOR_APPROVED_MAX_YAML_BYTES)
    goto done;
  turbo_praktor_inline_plan_config_init(&config);
  config.source_id = turbo_agent_praktor_inline_source_id(lowered);
  config.workflow_yaml = turbo_agent_praktor_inline_source_yaml(lowered);
  config.workflow_yaml_size = turbo_agent_praktor_inline_source_yaml_size(lowered);
  config.approved_host_tools = turbo_agent_template_plan_approved_tools(plan);
  if (turbo_praktor_inline_plan_compile(&config, &compiled) != TURBO_TOOL_OK ||
      !compiled) goto done;

  if (context && context->cancel_token &&
      turbo_cancel_token_check(context->cancel_token) != SALTS_OK) {
    status = TURBO_TOOL_CANCELLED;
    goto done; /* Not started; no tool callback and no retry hazard. */
  }
  if (!approved_store_record(
          runtime, PRAKTOR_ATTEMPT_COLLECTION, key, thread_id, attempt_id,
          approval_id, hash, "started", TURBO_TOOL_UNKNOWN_SIDE_EFFECT,
          TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT, NULL)) {
    status = TURBO_TOOL_ERROR;
    goto done;
  }

  /* One admitted Praktor WorkflowPlan execution. HostTool callbacks run only
   * under its frozen RuntimeTools projection. We deliberately DO NOT retry. */
  status = turbo_praktor_inline_plan_execute(compiled, context, &result);
  if (status == TURBO_TOOL_OK && (!result || json_type(result) != JSON_OBJECT))
    status = TURBO_TOOL_ERROR;
  outcome = status == TURBO_TOOL_OK
      ? turbo_agent_template_finish_praktor_result(plan, result)
      : TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED;
  if (outcome == TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT) {
    status = TURBO_TOOL_ERROR;
    outcome = TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED;
  }
  if (!approved_store_record(
          runtime, PRAKTOR_ATTEMPT_COLLECTION, key, thread_id, attempt_id,
          approval_id, hash, "committed", status, outcome,
          status == TURBO_TOOL_OK ? result : NULL)) {
    status = TURBO_TOOL_UNKNOWN_SIDE_EFFECT;
    goto done; /* STARTED persists: never return an uncommitted output. */
  }
  *out_outcome = outcome;
  if (status == TURBO_TOOL_OK) {
    *out_result = result;
    result = NULL;
  }

done:
  turbo_runtime_json_destroy(result);
  turbo_runtime_json_destroy(existing);
  turbo_runtime_json_destroy(receipt);
  turbo_praktor_inline_plan_destroy(compiled);
  turbo_agent_praktor_inline_source_destroy(lowered);
  return status;
}
