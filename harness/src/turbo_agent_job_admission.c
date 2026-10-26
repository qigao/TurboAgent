#include "turbo_agent_job_admission_internal.h"
#include "turbo_agent_job.h"
#include "turbo_agent_job_internal.h"

#include <cmeta_uuid.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define ADMISSIONS "agent_job_admissions"

static int identifier(const char *s) {
  if (!s || !s[0]) return 0;
  for (size_t i = 0; i <= 64; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!c) return 1;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
  }
  return 0;
}
static int is(const json_value_t *r, const char *key, const char *expected) {
  const json_value_t *v = json_object_get(r, key);
  return expected && v && json_type(v) == JSON_STRING &&
      json_string_len(v) == strlen(expected) && !memcmp(json_string(v), expected, strlen(expected));
}
static int64_t number(const json_value_t *r, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(r, key), INT64_MIN);
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
static int record_valid(const json_value_t *r, const char *id, const char *admission,
    const json_value_t *snapshot) {
  const json_value_t *supervisor, *binding = json_object_get(snapshot, "attempt");
  cmeta_uuid_t uuid;
  char canonical[SALTS_UUID_STRING_SIZE];
  int64_t schema;
  if (!r || json_type(r) != JSON_OBJECT) return SALTS_EPROTO;
  schema = number(r, "schema_version");
  if (schema != 1) return schema > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
  if (json_object_size(r) != 10 || !is(r, "kind", "job_inspect_admission_intent") ||
      !is(r, "admission_id", admission) || !is(r, "observation_id", admission) ||
      !is(r, "job_id", id) || number(r, "job_revision") != number(snapshot, "revision") ||
      number(r, "proposal_version") != number(json_object_get(snapshot, "proposal"), "revision") ||
      !is(r, "thread_id", json_get_string(binding, "thread_id")) ||
      !is(r, "attempt_id", json_get_string(binding, "attempt_id"))) return SALTS_EPROTO;
  supervisor = json_object_get(r, "supervisor_id");
  if (!supervisor || json_type(supervisor) != JSON_STRING || json_string_len(supervisor) != 36 ||
      strlen(json_string(supervisor)) != 36 || cmeta_uuid_parse(json_string(supervisor), &uuid) != SALTS_OK ||
      cmeta_uuid_format(&uuid, canonical, sizeof(canonical)) != SALTS_OK ||
      strcmp(json_string(supervisor), canonical)) return SALTS_EPROTO;
  return SALTS_OK;
}

int turbo_agent_job_admission_write(turbo_agent_runtime_t *runtime, const char *id,
    uint32_t revision, const char *admission, const char *supervisor) {
  json_value_t *snapshot = NULL, *r = NULL;
  const json_value_t *binding;
  char key[130];
  int rc;
  if (!runtime || !identifier(id) || !identifier(admission) || !supervisor) return SALTS_EINVAL;
  rc = snapshot_read(runtime, id, &snapshot);
  if (rc != SALTS_OK) return rc;
  if (number(snapshot, "revision") != revision) { rc = SALTS_EBUSY; goto done; }
  rc = turbo_agent_job_check_active(runtime, id, revision);
  if (rc != SALTS_OK) goto done;
  binding = json_object_get(snapshot, "attempt");
  r = json_create_object();
  rc = SALTS_ENOMEM;
  if (!r || turbo_runtime_json_object_set_int64(r, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(r, "kind", "job_inspect_admission_intent") ||
      turbo_runtime_json_object_set_string(r, "admission_id", admission) ||
      turbo_runtime_json_object_set_string(r, "observation_id", admission) ||
      turbo_runtime_json_object_set_string(r, "supervisor_id", supervisor) ||
      turbo_runtime_json_object_set_string(r, "job_id", id) ||
      turbo_runtime_json_object_set_int64(r, "job_revision", revision) ||
      turbo_runtime_json_object_set_int64(r, "proposal_version", number(json_object_get(snapshot, "proposal"), "revision")) ||
      turbo_runtime_json_object_set_string(r, "thread_id", json_get_string(binding, "thread_id")) ||
      turbo_runtime_json_object_set_string(r, "attempt_id", json_get_string(binding, "attempt_id"))) goto done;
  rc = record_valid(r, id, admission, snapshot);
  if (rc != SALTS_OK) goto done;
  (void)snprintf(key, sizeof(key), "%s:%s", id, admission);
  rc = turbo_agent_runtime_store_create_json_if_absent(runtime, ADMISSIONS, key, r);
done:
  json_free(snapshot); json_free(r);
  return rc;
}

int turbo_agent_job_admission_get(turbo_agent_runtime_t *runtime, const char *id,
    const char *admission, json_value_t **out) {
  json_value_t *snapshot = NULL, *record = NULL;
  char key[130];
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!runtime || !identifier(id) || !identifier(admission)) return SALTS_EINVAL;
  rc = snapshot_read(runtime, id, &snapshot);
  if (rc != SALTS_OK) return rc;
  (void)snprintf(key, sizeof(key), "%s:%s", id, admission);
  rc = turbo_agent_runtime_store_get_json_status(runtime, ADMISSIONS, key, &record);
  if (rc == SALTS_OK) rc = record_valid(record, id, admission, snapshot);
  if (rc == SALTS_OK) { *out = record; record = NULL; }
  json_free(snapshot); json_free(record);
  return rc;
}
