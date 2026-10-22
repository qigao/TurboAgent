#include "turbo_agent_router_internal.h"
#include "turbo_agent_execution_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_segment.h"
#include <limits.h>
#include <string.h>

#define OBSERVATION_MAX_INTEGER INT64_C(9007199254740991)

static int observation_set(json_value_t *record, const char *key, json_value_t *value) {
  if (!value) return SALTS_ENOMEM;
  if (turbo_runtime_json_object_set(record, key, value)) {
    json_free(value); return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int observation_copy(json_value_t *record, const char *key, const json_value_t *value) {
  return observation_set(record, key, value ? json_clone(value) : json_create_null());
}

static int observation_text(const json_value_t *record, const char *key, size_t limit, int nullable) {
  const json_value_t *value = json_object_get(record, key);
  const char *text;
  if (value && nullable && json_type(value) == JSON_NULL) return SALTS_OK;
  if (!value || json_type(value) != JSON_STRING || !(text = json_string(value)) ||
      !text[0] || strlen(text) != json_string_len(value)) return SALTS_EPROTO;
  return json_string_len(value) > limit ? SALTS_ENOBUFS : SALTS_OK;
}

static int64_t observation_integer(const json_value_t *record, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(record, key), INT64_MIN);
}

static int observation_validate(const json_value_t *record, const char *id) {
  static const char *fields[] = {
    "schema_version", "observation_id", "semantics", "clock_domain", "recorded_at",
    "task_id", "execution_id", "endpoint", "model", "provider", "role", "thread_id", "parent_run_id",
    "execution_status", "runtime_result_code", "runtime_run_id", "runtime_checkpoint_id", "runtime_status",
    "failure_kind", "unresponsive", "authority_current", "approval_pending", "max_model_turns", "review_resumes",
    "started_mono_ms", "observed_mono_ms", "heartbeat_mono_ms", "deadline_mono_ms",
    "estimated_cost_micros", "budget_micros", "actual_cost_micros"
  };
  static const char *ids[] = {"observation_id", "task_id", "execution_id", "endpoint", "thread_id"};
  static const char *nullable[] = {"runtime_run_id", "runtime_checkpoint_id", "runtime_status", "failure_kind"};
  static const char *booleans[] = {"unresponsive", "authority_current", "approval_pending"};
  static const char *numbers[] = {"started_mono_ms", "observed_mono_ms", "heartbeat_mono_ms",
    "deadline_mono_ms", "estimated_cost_micros", "budget_micros"};
  static const char *terminal_status[] = {"completed", "interrupted", "cancelled", "timed_out"};
  uint64_t seen = 0;
  int rc;
  int64_t status, turns, resumes, operation;
  const char *runtime_status;
  if (!record || json_type(record) != JSON_OBJECT ||
      json_object_size(record) != sizeof(fields) / sizeof(fields[0])) return SALTS_EPROTO;
  for (size_t i = 0; i < json_object_size(record); ++i) {
    const char *key = json_object_key(record, i);
    size_t j;
    if (!key || strlen(key) != json_object_key_len(record, i)) return SALTS_EPROTO;
    for (j = 0; j < sizeof(fields) / sizeof(fields[0]); ++j)
      if (!strcmp(key, fields[j])) break;
    if (j == sizeof(fields) / sizeof(fields[0]) || (seen & (UINT64_C(1) << j))) return SALTS_EPROTO;
    seen |= UINT64_C(1) << j;
  }
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
    rc = observation_text(record, ids[i], 64, 0);
    if (rc) return rc;
    if (!turbo_agent_router_id_valid_internal(json_get_string(record, ids[i]))) return SALTS_EPROTO;
  }
  if ((rc = observation_text(record, "parent_run_id", 128, 0)) ||
      (rc = observation_text(record, "model", 1024, 0)) ||
      (rc = observation_text(record, "provider", 1024, 0)) ||
      (rc = observation_text(record, "recorded_at", 31, 0)) ||
      (rc = observation_text(record, "semantics", 64, 0)) ||
      (rc = observation_text(record, "clock_domain", 64, 0)) ||
      (rc = observation_text(record, "role", 16, 0))) return rc;
  for (size_t i = 0; i < sizeof(nullable) / sizeof(nullable[0]); ++i)
    if ((rc = observation_text(record, nullable[i], 128, 1))) return rc;
  if (observation_integer(record, "schema_version") != 1 ||
      strcmp(json_get_string(record, "observation_id"), id) ||
      strcmp(json_get_string(record, "semantics"), "historical_host_observation") ||
      strcmp(json_get_string(record, "clock_domain"), "origin_process_monotonic") ||
      (strcmp(json_get_string(record, "role"), "plan") && strcmp(json_get_string(record, "role"), "execute")) ||
      json_type(json_object_get(record, "actual_cost_micros")) != JSON_NULL) return SALTS_EPROTO;
  for (size_t i = 0; i < sizeof(booleans) / sizeof(booleans[0]); ++i)
    if (json_type(json_object_get(record, booleans[i])) != JSON_BOOL) return SALTS_EPROTO;
  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
    int64_t number = observation_integer(record, numbers[i]);
    if (number < 0 || number > OBSERVATION_MAX_INTEGER) return SALTS_EPROTO;
  }
  if (observation_integer(record, "observed_mono_ms") < observation_integer(record, "started_mono_ms") ||
      observation_integer(record, "heartbeat_mono_ms") < observation_integer(record, "started_mono_ms") ||
      observation_integer(record, "heartbeat_mono_ms") > observation_integer(record, "observed_mono_ms") ||
      observation_integer(record, "deadline_mono_ms") <= observation_integer(record, "started_mono_ms") ||
      observation_integer(record, "deadline_mono_ms") - observation_integer(record, "started_mono_ms") > 86400000 ||
      observation_integer(record, "estimated_cost_micros") > observation_integer(record, "budget_micros"))
    return SALTS_EPROTO;
  status = observation_integer(record, "execution_status");
  turns = observation_integer(record, "max_model_turns");
  resumes = observation_integer(record, "review_resumes");
  if (status < TURBO_AGENT_EXECUTION_QUEUED || status > TURBO_AGENT_EXECUTION_FAILED ||
      (turns != 0 && (turns < 2 || turns > TURBO_AGENT_ROUTER_MAX_MODEL_TURNS)) ||
      resumes < 0 || resumes > TURBO_AGENT_ROUTER_MAX_REVIEW_RESUMES || (!turns && resumes)) return SALTS_EPROTO;
  if (!resumes && strcmp(json_get_string(record, "task_id"), json_get_string(record, "execution_id")))
    return SALTS_EPROTO;
  if (status < TURBO_AGENT_EXECUTION_COMPLETED) {
    if (json_type(json_object_get(record, "runtime_result_code")) != JSON_NULL ||
        json_get_bool(record, "approval_pending", 0)) return SALTS_EPROTO;
    for (size_t i = 0; i < sizeof(nullable) / sizeof(nullable[0]); ++i)
      if (json_type(json_object_get(record, nullable[i])) != JSON_NULL) return SALTS_EPROTO;
  } else {
    operation = observation_integer(record, "runtime_result_code");
    if (operation < INT_MIN || operation > INT_MAX || (operation && status != TURBO_AGENT_EXECUTION_FAILED))
      return SALTS_EPROTO;
    runtime_status = json_get_string(record, "runtime_status");
    if (!!json_get_string(record, "runtime_run_id") != !!runtime_status ||
        (!runtime_status && json_type(json_object_get(record, "runtime_checkpoint_id")) != JSON_NULL))
      return SALTS_EPROTO;
    if (!operation) {
      if (status < TURBO_AGENT_EXECUTION_FAILED &&
          (!runtime_status || strcmp(runtime_status, terminal_status[status - TURBO_AGENT_EXECUTION_COMPLETED])))
        return SALTS_EPROTO;
      if (status == TURBO_AGENT_EXECUTION_FAILED && runtime_status)
        for (size_t i = 0; i < sizeof(terminal_status) / sizeof(terminal_status[0]); ++i)
          if (!strcmp(runtime_status, terminal_status[i])) return SALTS_EPROTO;
    }
    if (json_get_bool(record, "approval_pending", 0) &&
        (status != TURBO_AGENT_EXECUTION_INTERRUPTED || !turns ||
         !json_get_string(record, "runtime_checkpoint_id"))) return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int observation_size(const json_value_t *record) {
  char *text = json_serialize(record, NULL);
  int rc;
  if (!text) return SALTS_ENOMEM;
  rc = strlen(text) > TURBO_AGENT_ROUTER_OBSERVATION_MAX_BYTES ? SALTS_ENOBUFS : SALTS_OK;
  json_serialize_free(text);
  return rc;
}

int turbo_agent_router_task_record_observation(const turbo_agent_router_task_t *task,
    turbo_agent_runtime_t *host_runtime, const char *id) {
  turbo_agent_execution_status_t status;
  const json_value_t *summary = NULL, *state = NULL;
  json_value_t *record;
  char timestamp[32];
  int rc, operation = 0, terminal;
  if (!task || !host_runtime || !turbo_agent_router_id_valid_internal(id)) return SALTS_EINVAL;
  for (size_t i = 0; i < task->router->count; ++i)
    if (host_runtime == task->router->endpoints[i].config.runtime) return SALTS_EINVAL;
  rc = turbo_agent_execution_get_status(task->execution, &status);
  if (rc) return rc;
  terminal = status >= TURBO_AGENT_EXECUTION_COMPLETED;
  /* Status only advances toward terminal. A running observation must not pick
   * up a result published after that status read and mix two moments. */
  if (terminal) {
    rc = turbo_agent_execution_peek_result_internal(task->execution, &summary, &state);
    if (rc) return rc;
    rc = turbo_agent_execution_result_code(task->execution, &operation);
    if (rc) return rc;
    if (summary && (observation_text(summary, "run_id", 128, 0) ||
        observation_text(summary, "status", 128, 0) ||
        observation_text(summary, "checkpoint_id", 128, 1) ||
        observation_text(summary, "thread_id", 64, 0) ||
        observation_text(summary, "parent_agent_run_id", 128, 0) ||
        strcmp(json_get_string(summary, "thread_id"), task->thread_id) ||
        strcmp(json_get_string(summary, "parent_agent_run_id"), task->parent_run_id))) return SALTS_EPROTO;
  }
  if (task->started_ms > OBSERVATION_MAX_INTEGER || task->observed_ms > OBSERVATION_MAX_INTEGER ||
      task->heartbeat_ms > OBSERVATION_MAX_INTEGER || task->deadline_ms > OBSERVATION_MAX_INTEGER)
    return SALTS_ENOBUFS;
  if (turbo_runtime_record_timestamp(timestamp, sizeof(timestamp))) return SALTS_EIO;
  record = json_create_object();
  if (!record) return SALTS_ENOMEM;
  rc = SALTS_ENOMEM;
  if (turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(record, "observation_id", id) ||
      turbo_runtime_json_object_set_string(record, "semantics", "historical_host_observation") ||
      turbo_runtime_json_object_set_string(record, "clock_domain", "origin_process_monotonic") ||
      turbo_runtime_json_object_set_string(record, "recorded_at", timestamp) ||
      turbo_runtime_json_object_set_string(record, "task_id", turbo_agent_router_task_id(task)) ||
      turbo_runtime_json_object_set_string(record, "execution_id", turbo_agent_execution_id(task->execution)) ||
      turbo_runtime_json_object_set_string(record, "endpoint", task->endpoint->id) ||
      turbo_runtime_json_object_set_string(record, "model", task->endpoint->config.agent->model) ||
      turbo_runtime_json_object_set_string(record, "provider", turbo_agent_provider_name(task->endpoint->config.agent)) ||
      turbo_runtime_json_object_set_string(record, "role", task->role == TURBO_AGENT_ROUTER_PLAN ? "plan" : "execute") ||
      turbo_runtime_json_object_set_string(record, "thread_id", task->thread_id) ||
      turbo_runtime_json_object_set_string(record, "parent_run_id", task->parent_run_id) ||
      turbo_runtime_json_object_set_int64(record, "execution_status", status) ||
      observation_set(record, "runtime_result_code", terminal ? json_create_int64(operation) : json_create_null()) ||
      observation_copy(record, "runtime_run_id", json_object_get(summary, "run_id")) ||
      observation_copy(record, "runtime_checkpoint_id", json_object_get(summary, "checkpoint_id")) ||
      observation_copy(record, "runtime_status", json_object_get(summary, "status")) ||
      observation_set(record, "failure_kind", state && turbo_agent_state_failure_kind(state)
          ? json_create_string(turbo_agent_state_failure_kind(state)) : json_create_null()) ||
      observation_set(record, "unresponsive", json_create_bool(task->unresponsive)) ||
      observation_set(record, "authority_current", json_create_bool(turbo_agent_router_authority_current_internal(task))) ||
      observation_set(record, "approval_pending", json_create_bool(terminal && status == TURBO_AGENT_EXECUTION_INTERRUPTED &&
          task->pending_review && !task->review_cancelled)) ||
      turbo_runtime_json_object_set_int64(record, "max_model_turns", task->max_model_turns) ||
      turbo_runtime_json_object_set_int64(record, "review_resumes", task->review_resumes) ||
      turbo_runtime_json_object_set_int64(record, "started_mono_ms", (int64_t)task->started_ms) ||
      turbo_runtime_json_object_set_int64(record, "observed_mono_ms", (int64_t)task->observed_ms) ||
      turbo_runtime_json_object_set_int64(record, "heartbeat_mono_ms", (int64_t)task->heartbeat_ms) ||
      turbo_runtime_json_object_set_int64(record, "deadline_mono_ms", (int64_t)task->deadline_ms) ||
      turbo_runtime_json_object_set_int64(record, "estimated_cost_micros", (int64_t)task->estimated_micros) ||
      turbo_runtime_json_object_set_int64(record, "budget_micros", (int64_t)task->budget_micros) ||
      observation_set(record, "actual_cost_micros", json_create_null())) goto done;
  rc = observation_validate(record, id);
  if (!rc) rc = observation_size(record);
  if (!rc) rc = turbo_agent_runtime_store_create_json_if_absent(host_runtime,
      TURBO_AGENT_ROUTER_OBSERVATION_COLLECTION, id, record);
done:
  json_free(record);
  return rc;
}

int turbo_agent_router_load_observation(turbo_agent_runtime_t *host_runtime,
    const char *id, json_value_t **out_record) {
  json_value_t *record = NULL;
  int rc;
  if (!out_record) return SALTS_EINVAL;
  *out_record = NULL;
  if (!host_runtime || !turbo_agent_router_id_valid_internal(id)) return SALTS_EINVAL;
  rc = turbo_agent_runtime_store_get_json_status(host_runtime,
      TURBO_AGENT_ROUTER_OBSERVATION_COLLECTION, id, &record);
  if (!rc) rc = observation_validate(record, id);
  if (!rc) rc = observation_size(record);
  if (!rc) { *out_record = record; record = NULL; }
  json_free(record);
  return rc;
}
