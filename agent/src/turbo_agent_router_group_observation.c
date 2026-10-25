#include "turbo_agent_router_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_runtime_segment.h"
#include <limits.h>
#include <string.h>

static int group_text(const json_value_t *record, const char *key, size_t limit) {
  const json_value_t *value = json_object_get(record, key);
  const char *text = value ? json_string(value) : NULL;
  if (!value || json_type(value) != JSON_STRING || !text || !text[0] ||
      strlen(text) != json_string_len(value)) return SALTS_EPROTO;
  return strlen(text) > limit ? SALTS_ENOBUFS : SALTS_OK;
}

static int64_t group_integer(const json_value_t *record, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(record, key), INT64_MIN);
}

static int group_boolean(json_value_t *record, const char *key, int boolean) {
  json_value_t *value = json_create_bool(boolean);
  if (!value) return SALTS_ENOMEM;
  if (turbo_runtime_json_object_set(record, key, value)) { json_free(value); return SALTS_ENOMEM; }
  return SALTS_OK;
}

static int group_observation_validate(const json_value_t *record, const char *id) {
  static const char *fields[] = {"schema_version", "observation_id", "semantics",
      "clock_domain", "recorded_at", "group_id", "parent_run_id", "observed_mono_ms",
      "failure_code", "input_built", "handoff_kind", "members"};
  static const char *unique[] = {"task_id", "execution_id", "thread_id", "endpoint"};
  const json_value_t *members;
  const char *kind;
  uint64_t seen = 0;
  int64_t observed, failure;
  int rc, aggregation;
  if (!record || json_type(record) != JSON_OBJECT ||
      json_object_size(record) != sizeof(fields)/sizeof(fields[0])) return SALTS_EPROTO;
  for (size_t i = 0; i < json_object_size(record); ++i) {
    const char *key = json_object_key(record, i);
    size_t j;
    if (!key || strlen(key) != json_object_key_len(record, i)) return SALTS_EPROTO;
    for (j = 0; j < sizeof(fields)/sizeof(fields[0]); ++j)
      if (!strcmp(key, fields[j])) break;
    if (j == sizeof(fields)/sizeof(fields[0]) || (seen & (UINT64_C(1) << j))) return SALTS_EPROTO;
    seen |= UINT64_C(1) << j;
  }
  if ((rc = group_text(record, "observation_id", 64)) ||
      (rc = group_text(record, "group_id", 64)) ||
      (rc = group_text(record, "parent_run_id", 128)) ||
      (rc = group_text(record, "recorded_at", 31)) ||
      (rc = group_text(record, "semantics", 64)) ||
      (rc = group_text(record, "clock_domain", 64)) ||
      (rc = group_text(record, "handoff_kind", 32))) return rc;
  kind = json_get_string(record, "handoff_kind");
  aggregation = !strcmp(kind, "aggregation_input");
  observed = group_integer(record, "observed_mono_ms");
  failure = group_integer(record, "failure_code");
  members = json_object_get(record, "members");
  if (group_integer(record, "schema_version") != 1 ||
      strcmp(json_get_string(record, "observation_id"), id) ||
      !turbo_agent_router_id_valid_internal(json_get_string(record, "observation_id")) ||
      !turbo_agent_router_id_valid_internal(json_get_string(record, "group_id")) ||
      strcmp(json_get_string(record, "semantics"), "pre_group_handoff_observation") ||
      strcmp(json_get_string(record, "clock_domain"), "origin_process_monotonic") ||
      (!aggregation && strcmp(kind, "result_collection")) ||
      observed < 0 || observed > INT64_C(9007199254740991) ||
      failure < INT_MIN || failure > 0 ||
      json_type(json_object_get(record, "input_built")) != JSON_BOOL ||
      !members || json_type(members) != JSON_ARRAY || json_array_size(members) < 2 ||
      json_array_size(members) > TURBO_AGENT_ROUTER_MAX_ENDPOINTS ||
      (aggregation && (failure || json_get_bool(record, "input_built", 1)))) return SALTS_EPROTO;
  for (size_t i = 0; i < json_array_size(members); ++i) {
    const json_value_t *member = json_array_get(members, i);
    rc = turbo_agent_router_observation_validate_internal(member, id);
    if (rc) return rc;
    if (group_integer(member, "execution_status") < TURBO_AGENT_EXECUTION_COMPLETED ||
        group_integer(member, "max_model_turns") || group_integer(member, "review_resumes") ||
        group_integer(member, "observed_mono_ms") > observed ||
        strcmp(json_get_string(member, "parent_run_id"), json_get_string(record, "parent_run_id")) ||
        !json_get_bool(member, "authority_current", 0) || json_get_bool(member, "unresponsive", 1) ||
        json_get_bool(member, "approval_pending", 1)) return SALTS_EPROTO;
    if (aggregation && (group_integer(member, "execution_status") != TURBO_AGENT_EXECUTION_COMPLETED ||
        group_integer(member, "runtime_result_code") ||
        !json_is_null(json_object_get(member, "failure_kind")))) return SALTS_EPROTO;
    for (size_t j = 0; j < i; ++j)
      for (size_t k = 0; k < sizeof(unique)/sizeof(unique[0]); ++k)
        if (!strcmp(json_get_string(member, unique[k]),
            json_get_string(json_array_get(members, j), unique[k]))) return SALTS_EPROTO;
  }
  char *serialized = json_serialize(record, NULL);
  if (!serialized) return SALTS_ENOMEM;
  rc = strlen(serialized) > TURBO_AGENT_ROUTER_GROUP_OBSERVATION_MAX_BYTES ? SALTS_ENOBUFS : SALTS_OK;
  json_serialize_free(serialized);
  return rc;
}

int turbo_agent_router_group_observation_destination_internal(const turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id) {
  if (!group || !host_runtime || !turbo_agent_router_id_valid_internal(id)) return SALTS_EINVAL;
  for (size_t i = 0; i < group->tasks[0]->router->count; ++i)
    if (host_runtime == group->tasks[0]->router->endpoints[i].config.runtime) return SALTS_EINVAL;
  return SALTS_OK;
}

int turbo_agent_router_group_observation_write_internal(const turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id, const char *kind) {
  json_value_t *record = NULL, *members = NULL, *member = NULL;
  char timestamp[32];
  int rc = turbo_agent_router_group_observation_destination_internal(group, host_runtime, id);
  if (rc) return rc;
  if (turbo_runtime_record_timestamp(timestamp, sizeof(timestamp))) return SALTS_EIO;
  record = json_create_object(); members = json_create_array();
  rc = SALTS_ENOMEM;
  if (!record || !members) goto done;
  for (size_t i = 0; i < group->count; ++i) {
    rc = turbo_agent_router_task_observation_internal(group->tasks[i], id, &member);
    if (rc) goto done;
    rc = SALTS_ENOMEM;
    if (turbo_runtime_json_array_append(members, member)) goto done;
    member = NULL;
  }
  if (turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(record, "observation_id", id) ||
      turbo_runtime_json_object_set_string(record, "semantics", "pre_group_handoff_observation") ||
      turbo_runtime_json_object_set_string(record, "clock_domain", "origin_process_monotonic") ||
      turbo_runtime_json_object_set_string(record, "recorded_at", timestamp) ||
      turbo_runtime_json_object_set_string(record, "group_id", group->id) ||
      turbo_runtime_json_object_set_string(record, "parent_run_id", group->tasks[0]->parent_run_id) ||
      turbo_runtime_json_object_set_int64(record, "observed_mono_ms", (int64_t)group->observed_ms) ||
      turbo_runtime_json_object_set_int64(record, "failure_code", group->failure_code) ||
      group_boolean(record, "input_built", group->input_built) ||
      turbo_runtime_json_object_set_string(record, "handoff_kind", kind)) goto done;
  if (turbo_runtime_json_object_set(record, "members", members)) goto done;
  members = NULL;
  rc = group_observation_validate(record, id);
  if (!rc) rc = turbo_agent_runtime_store_create_json_if_absent(host_runtime,
      TURBO_AGENT_ROUTER_GROUP_OBSERVATION_COLLECTION, id, record);
done:
  json_free(member); json_free(members); json_free(record);
  return rc;
}

int turbo_agent_router_load_group_observation(turbo_agent_runtime_t *host_runtime,
    const char *id, json_value_t **out_record) {
  json_value_t *record = NULL;
  int rc;
  if (!out_record) return SALTS_EINVAL;
  *out_record = NULL;
  if (!host_runtime || !turbo_agent_router_id_valid_internal(id)) return SALTS_EINVAL;
  rc = turbo_agent_runtime_store_get_json_status(host_runtime,
      TURBO_AGENT_ROUTER_GROUP_OBSERVATION_COLLECTION, id, &record);
  if (!rc) rc = group_observation_validate(record, id);
  if (!rc) { *out_record = record; record = NULL; }
  json_free(record);
  return rc;
}
