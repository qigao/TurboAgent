#include "turbo_agent_job_observation_internal.h"
#include "turbo_agent_job.h"
#include "turbo_agent_harness_internal.h"

#include <cmeta_uuid.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define OBSERVATIONS "agent_job_observations"
#define MAX_OBSERVATION_BYTES 16384U

static int identifier(const char *s) {
  size_t i;
  if (!s || !s[0]) return 0;
  for (i = 0; i <= 64; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!c) return 1;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 0;
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
  return turbo_runtime_json_value_as_int64(json_object_get(r, key), INT64_MIN);
}
static int status_number(const json_value_t *r, const char *key) {
  int64_t n = number(r, key);
  return n >= INT32_MIN && n <= INT32_MAX;
}
static const char *state_name(turbo_agent_execution_status_t state) {
  switch (state) {
    case TURBO_AGENT_EXECUTION_COMPLETED: return "completed";
    case TURBO_AGENT_EXECUTION_CANCELLED: return "cancelled";
    case TURBO_AGENT_EXECUTION_TIMED_OUT: return "timed_out";
    case TURBO_AGENT_EXECUTION_FAILED: return "failed";
    default: return NULL;
  }
}
static int snapshot_read(turbo_agent_runtime_t *runtime, const char *id, json_value_t **out) {
  turbo_agent_job_t *job = NULL;
  int rc = turbo_agent_job_open(runtime, id, &job);
  *out = NULL;
  if (rc == SALTS_OK) rc = turbo_agent_job_get_snapshot(job, out);
  turbo_agent_job_destroy(job);
  if (rc == SALTS_OK && !is(*out, "state", "attempt_bound")) {
    json_free(*out); *out = NULL; rc = SALTS_EBUSY;
  }
  return rc;
}
static int reference_valid(const json_value_t *r, const json_value_t *snapshot) {
  const json_value_t *binding = json_object_get(snapshot, "attempt");
  const char *thread = json_get_string(binding, "thread_id");
  const char *attempt = json_get_string(binding, "attempt_id");
  char key[160];
  int rc = turbo_agent_harness_job_reference_shape(r);
  if (rc != SALTS_OK) return rc;
  (void)snprintf(key, sizeof(key), "%03u_%s_%s", (unsigned)strlen(thread), thread, attempt);
  return is(r, "job_id", json_get_string(snapshot, "id")) &&
      number(r, "job_revision") == number(snapshot, "revision") &&
      number(r, "proposal_version") == number(json_object_get(snapshot, "proposal"), "revision") &&
      is(r, "thread_id", thread) && is(r, "attempt_id", attempt) &&
      is(r, "attempt_record_id", key) ? SALTS_OK : SALTS_EPROTO;
}
static int record_valid(const json_value_t *r, const char *id, const char *observation,
    const json_value_t *snapshot) {
  const json_value_t *request, *reference, *binding = json_object_get(snapshot, "attempt");
  const char *supervisor;
  cmeta_uuid_t uuid;
  char canonical[SALTS_UUID_STRING_SIZE];
  int64_t op, tool, version;
  int requested, has_reference, dispatched, rc;
  char *serialized;
  if (!r || json_type(r) != JSON_OBJECT) return SALTS_EPROTO;
  version = number(r, "schema_version");
  if (version != 1) return version > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
  if (json_object_size(r) != 17 || !is(r, "kind", "job_execution_observation") ||
      !is(r, "observation_id", observation) || !is(r, "job_id", id) ||
      number(r, "job_revision") != number(snapshot, "revision") ||
      number(r, "proposal_version") != number(json_object_get(snapshot, "proposal"), "revision") ||
      !is(r, "thread_id", json_get_string(binding, "thread_id")) ||
      !is(r, "attempt_id", json_get_string(binding, "attempt_id")) ||
      !status_number(r, "operation_status") || !status_number(r, "cancellation_status") ||
      number(r, "tool_status") < TURBO_TOOL_TRAPPED || number(r, "tool_status") > TURBO_TOOL_OK ||
      json_type(json_object_get(r, "cancellation_requested")) != JSON_BOOL) return SALTS_EPROTO;
  supervisor = text(r, "supervisor_id", 36);
  if (!supervisor || strlen(supervisor) != 36 || cmeta_uuid_parse(supervisor, &uuid) != SALTS_OK ||
      cmeta_uuid_format(&uuid, canonical, sizeof(canonical)) != SALTS_OK || strcmp(supervisor, canonical))
    return SALTS_EPROTO;
  request = json_object_get(r, "cancellation_request");
  reference = json_object_get(r, "execution_reference");
  if (!request || !reference) return SALTS_EPROTO;
  requested = json_get_bool(r, "cancellation_requested", 0);
  if (requested) {
    if (json_type(request) != JSON_OBJECT || json_object_size(request) != 2 ||
        !identifier(text(request, "request_id", 64)) || !text(request, "reason_ref", 1024)) return SALTS_EPROTO;
  } else if (json_type(request) != JSON_NULL || number(r, "cancellation_status") != SALTS_ENOENT)
    return SALTS_EPROTO;
  dispatched = is(r, "dispatch", "inspect");
  if (!dispatched && !is(r, "dispatch", "pool_cancelled")) return SALTS_EPROTO;
  has_reference = json_type(reference) != JSON_NULL;
  op = number(r, "operation_status"); tool = number(r, "tool_status");
  if ((op == SALTS_OK) != has_reference) return SALTS_EPROTO;
  if (!dispatched && (op != SALTS_ECANCELED || tool != TURBO_TOOL_CANCELLED)) return SALTS_EPROTO;
  if (op == SALTS_OK) {
    if (tool != TURBO_TOOL_OK || (!is(r, "state", "completed") && !is(r, "state", "cancelled") &&
        !is(r, "state", "timed_out")) || (is(r, "state", "completed") && requested) ||
        (is(r, "state", "cancelled") && !requested)) return SALTS_EPROTO;
  } else if (op == SALTS_ECANCELED) {
    if (!is(r, "state", "cancelled") || tool != TURBO_TOOL_CANCELLED) return SALTS_EPROTO;
  } else if (op == SALTS_ETIMEDOUT) {
    if (!is(r, "state", "timed_out") || tool != TURBO_TOOL_DEADLINE_EXCEEDED) return SALTS_EPROTO;
  } else if (!is(r, "state", "failed")) return SALTS_EPROTO;
  if (has_reference && (rc = reference_valid(reference, snapshot)) != SALTS_OK) return rc;
  serialized = json_serialize(r, NULL);
  if (!serialized) return SALTS_ENOMEM;
  rc = strlen(serialized) <= MAX_OBSERVATION_BYTES ? SALTS_OK : SALTS_EFBIG;
  json_serialize_free(serialized);
  return rc;
}

int turbo_agent_job_observation_write(turbo_agent_runtime_t *runtime, const char *id,
    uint32_t revision, const char *observation, const char *supervisor, int dispatched,
    const turbo_agent_job_execution_status_t *status, const char *request_id,
    const char *reason_ref, const json_value_t *reference) {
  json_value_t *snapshot = NULL, *r = NULL, *request = NULL, *null_value = NULL;
  const json_value_t *binding;
  const char *state = status ? state_name(status->state) : NULL;
  char key[130];
  int rc;
  if (!runtime || !identifier(id) || !identifier(observation) || !supervisor || !state ||
      (dispatched != 0 && dispatched != 1)) return SALTS_EINVAL;
  rc = snapshot_read(runtime, id, &snapshot);
  if (rc != SALTS_OK) return rc;
  if (number(snapshot, "revision") != revision) { rc = SALTS_EBUSY; goto done; }
  binding = json_object_get(snapshot, "attempt");
  r = json_create_object();
  null_value = json_create_null();
  request = status->cancellation_requested ? json_create_object() : json_create_null();
  rc = SALTS_ENOMEM;
  if (!r || !request || !null_value ||
      (status->cancellation_requested &&
        (turbo_runtime_json_object_set_string(request, "request_id", request_id) ||
         turbo_runtime_json_object_set_string(request, "reason_ref", reason_ref))) ||
      turbo_runtime_json_object_set_int64(r, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(r, "kind", "job_execution_observation") ||
      turbo_runtime_json_object_set_string(r, "observation_id", observation) ||
      turbo_runtime_json_object_set_string(r, "supervisor_id", supervisor) ||
      turbo_runtime_json_object_set_string(r, "job_id", id) ||
      turbo_runtime_json_object_set_int64(r, "job_revision", revision) ||
      turbo_runtime_json_object_set_int64(r, "proposal_version", number(json_object_get(snapshot, "proposal"), "revision")) ||
      turbo_runtime_json_object_set_string(r, "thread_id", json_get_string(binding, "thread_id")) ||
      turbo_runtime_json_object_set_string(r, "attempt_id", json_get_string(binding, "attempt_id")) ||
      turbo_runtime_json_object_set_string(r, "dispatch", dispatched ? "inspect" : "pool_cancelled") ||
      turbo_runtime_json_object_set_string(r, "state", state) ||
      turbo_runtime_json_object_set_int64(r, "operation_status", status->operation_status) ||
      turbo_runtime_json_object_set_int64(r, "tool_status", status->tool_status) ||
      turbo_runtime_json_object_set_int64(r, "cancellation_status", status->cancellation_status) ||
      turbo_runtime_json_object_set_clone(r, "cancellation_request", request) ||
      turbo_runtime_json_object_set_clone(r, "execution_reference", reference ? reference : null_value)) goto done;
  {
    json_value_t *boolean = json_create_bool(status->cancellation_requested != 0);
    if (!boolean) goto done;
    if (turbo_runtime_json_object_set(r, "cancellation_requested", boolean)) { json_free(boolean); goto done; }
  }
  rc = record_valid(r, id, observation, snapshot);
  if (rc != SALTS_OK) goto done;
  (void)snprintf(key, sizeof(key), "%s:%s", id, observation);
  rc = turbo_agent_runtime_store_create_json_if_absent(runtime, OBSERVATIONS, key, r);
done:
  json_free(snapshot); json_free(r); json_free(request); json_free(null_value);
  return rc;
}

int turbo_agent_job_execution_observation_get(turbo_agent_runtime_t *runtime, const char *id,
    const char *observation, json_value_t **out) {
  json_value_t *snapshot = NULL, *record = NULL;
  char key[130];
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!runtime || !identifier(id) || !identifier(observation)) return SALTS_EINVAL;
  rc = snapshot_read(runtime, id, &snapshot);
  if (rc != SALTS_OK) return rc;
  (void)snprintf(key, sizeof(key), "%s:%s", id, observation);
  rc = turbo_agent_runtime_store_get_json_status(runtime, OBSERVATIONS, key, &record);
  if (rc == SALTS_OK) rc = record_valid(record, id, observation, snapshot);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
  json_free(snapshot); json_free(record);
  return rc;
}
