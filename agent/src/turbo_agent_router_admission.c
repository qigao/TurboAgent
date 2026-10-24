#include "turbo_agent_router_internal.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_runtime_segment.h"

#include <limits.h>
#include <string.h>

#define ADMISSION_MAX_INTEGER INT64_C(9007199254740991)

static int admission_nullable(json_value_t *record, const char *key, const char *text) {
  json_value_t *value = text ? json_create_string(text) : json_create_null();
  if (!value) return SALTS_ENOMEM;
  if (turbo_runtime_json_object_set(record, key, value)) {
    json_free(value);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int admission_text(const json_value_t *record, const char *key, size_t limit, int nullable) {
  const json_value_t *value = json_object_get(record, key);
  const char *text;
  if (nullable && value && json_type(value) == JSON_NULL) return SALTS_OK;
  if (!value || json_type(value) != JSON_STRING || !(text = json_string(value)) ||
      !text[0] || strlen(text) != json_string_len(value)) return SALTS_EPROTO;
  return json_string_len(value) > limit ? SALTS_ENOBUFS : SALTS_OK;
}

static int64_t admission_integer(const json_value_t *record, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(record, key), INT64_MIN);
}

static int admission_validate(const json_value_t *record, const char *id) {
  static const char *fields[] = {
    "schema_version", "admission_id", "semantics", "clock_domain", "recorded_at",
    "task_id", "endpoint", "model", "provider", "role", "thread_id", "parent_run_id",
    "parent_tool_call_id", "parent_tool_name", "parent_graph_run_id", "call_frame_id",
    "difficulty", "required_model_capabilities", "input_tokens_estimate", "output_tokens_estimate",
    "input_price_micros_per_million", "output_price_micros_per_million",
    "estimated_cost_micros", "budget_micros", "max_model_turns", "heartbeat_timeout_ms",
    "started_mono_ms", "deadline_mono_ms"
  };
  static const char *ids[] = {"admission_id", "task_id", "endpoint", "thread_id"};
  static const char *parent_ids[] = {"parent_tool_call_id", "parent_graph_run_id", "call_frame_id"};
  static const char *numbers[] = {"difficulty", "required_model_capabilities",
    "input_tokens_estimate", "output_tokens_estimate", "input_price_micros_per_million",
    "output_price_micros_per_million", "estimated_cost_micros", "budget_micros",
    "max_model_turns", "heartbeat_timeout_ms", "started_mono_ms", "deadline_mono_ms"};
  uint64_t seen = 0, expected;
  int64_t turns, started, deadline, heartbeat, capabilities;
  int rc;
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
    if ((rc = admission_text(record, ids[i], 64, 0))) return rc;
    if (!turbo_agent_router_id_valid_internal(json_get_string(record, ids[i]))) return SALTS_EPROTO;
  }
  for (size_t i = 0; i < sizeof(parent_ids) / sizeof(parent_ids[0]); ++i)
    if ((rc = admission_text(record, parent_ids[i], 128, 1))) return rc;
  if ((rc = admission_text(record, "parent_run_id", 128, 0)) ||
      (rc = admission_text(record, "parent_tool_name", 1024, 1)) ||
      (rc = admission_text(record, "model", 1024, 0)) ||
      (rc = admission_text(record, "provider", 1024, 0)) ||
      (rc = admission_text(record, "recorded_at", 31, 0)) ||
      (rc = admission_text(record, "semantics", 64, 0)) ||
      (rc = admission_text(record, "clock_domain", 64, 0)) ||
      (rc = admission_text(record, "role", 16, 0))) return rc;
  if (admission_integer(record, "schema_version") != 1 ||
      strcmp(json_get_string(record, "admission_id"), id) ||
      strcmp(json_get_string(record, "semantics"), "pre_dispatch_intent") ||
      strcmp(json_get_string(record, "clock_domain"), "origin_process_monotonic") ||
      (strcmp(json_get_string(record, "role"), "plan") &&
       strcmp(json_get_string(record, "role"), "execute"))) return SALTS_EPROTO;
  for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); ++i) {
    int64_t n = admission_integer(record, numbers[i]);
    if (n < 0 || n > ADMISSION_MAX_INTEGER) return SALTS_EPROTO;
  }
  turns = admission_integer(record, "max_model_turns");
  capabilities = admission_integer(record, "required_model_capabilities");
  started = admission_integer(record, "started_mono_ms");
  deadline = admission_integer(record, "deadline_mono_ms");
  heartbeat = admission_integer(record, "heartbeat_timeout_ms");
  if ((turns && (turns < 2 || turns > TURBO_AGENT_ROUTER_MAX_MODEL_TURNS)) ||
      !capabilities || (capabilities & ~INT64_C(7)) ||
      (turns && (strcmp(json_get_string(record, "role"), "execute") ||
                 !(capabilities & TURBO_AGENT_ROUTER_TOOL_PROPOSALS))) ||
      admission_integer(record, "difficulty") > 100 ||
      deadline <= started || deadline - started > 86400000 ||
      heartbeat <= 0 || heartbeat > deadline - started ||
      admission_integer(record, "input_tokens_estimate") < 1 ||
      admission_integer(record, "input_tokens_estimate") > 1000000 ||
      admission_integer(record, "output_tokens_estimate") < 1 ||
      admission_integer(record, "output_tokens_estimate") > 1000000 ||
      admission_integer(record, "input_price_micros_per_million") > 1000000000 ||
      admission_integer(record, "output_price_micros_per_million") > 1000000000)
    return SALTS_EPROTO;
  expected = ((uint64_t)admission_integer(record, "input_tokens_estimate") *
      (uint64_t)admission_integer(record, "input_price_micros_per_million") + 999999) / 1000000;
  expected += ((uint64_t)admission_integer(record, "output_tokens_estimate") *
      (uint64_t)admission_integer(record, "output_price_micros_per_million") + 999999) / 1000000;
  expected *= turns ? (uint64_t)turns : 1;
  if (expected != (uint64_t)admission_integer(record, "estimated_cost_micros") ||
      expected > (uint64_t)admission_integer(record, "budget_micros")) return SALTS_EPROTO;
  return SALTS_OK;
}

static int admission_size(const json_value_t *record) {
  char *text = json_serialize(record, NULL);
  int rc;
  if (!text) return SALTS_ENOMEM;
  rc = strlen(text) > TURBO_AGENT_ROUTER_ADMISSION_MAX_BYTES ? SALTS_ENOBUFS : SALTS_OK;
  json_serialize_free(text);
  return rc;
}

int turbo_agent_router_admission_write_internal(void *user_data, const char *execution_id) {
  const router_admission_t *admission = user_data;
  const turbo_agent_router_task_t *task = admission->task;
  const turbo_agent_router_endpoint_t *endpoint = &task->endpoint->config;
  const turbo_agent_runtime_parent_link_t *parent = admission->parent_link;
  json_value_t *record;
  char timestamp[32];
  int rc = SALTS_ENOMEM;
  if (task->started_ms > ADMISSION_MAX_INTEGER || task->deadline_ms > ADMISSION_MAX_INTEGER)
    return SALTS_ENOBUFS;
  if (turbo_runtime_record_timestamp(timestamp, sizeof(timestamp))) return SALTS_EIO;
  record = json_create_object();
  if (!record) return SALTS_ENOMEM;
  if (turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(record, "admission_id", task->admission_id) ||
      turbo_runtime_json_object_set_string(record, "semantics", "pre_dispatch_intent") ||
      turbo_runtime_json_object_set_string(record, "clock_domain", "origin_process_monotonic") ||
      turbo_runtime_json_object_set_string(record, "recorded_at", timestamp) ||
      turbo_runtime_json_object_set_string(record, "task_id", execution_id) ||
      turbo_runtime_json_object_set_string(record, "endpoint", task->endpoint->id) ||
      turbo_runtime_json_object_set_string(record, "model", endpoint->agent->model) ||
      turbo_runtime_json_object_set_string(record, "provider", turbo_agent_provider_name(endpoint->agent)) ||
      turbo_runtime_json_object_set_string(record, "role", task->role == TURBO_AGENT_ROUTER_PLAN ? "plan" : "execute") ||
      turbo_runtime_json_object_set_string(record, "thread_id", task->thread_id) ||
      turbo_runtime_json_object_set_string(record, "parent_run_id", task->parent_run_id) ||
      admission_nullable(record, "parent_tool_call_id", parent->parent_tool_call_id) ||
      admission_nullable(record, "parent_tool_name", parent->parent_tool_name) ||
      admission_nullable(record, "parent_graph_run_id", parent->parent_graph_run_id) ||
      admission_nullable(record, "call_frame_id", parent->call_frame_id) ||
      turbo_runtime_json_object_set_int64(record, "difficulty", task->difficulty) ||
      turbo_runtime_json_object_set_int64(record, "required_model_capabilities", task->required_capabilities) ||
      turbo_runtime_json_object_set_int64(record, "input_tokens_estimate", task->input_tokens) ||
      turbo_runtime_json_object_set_int64(record, "output_tokens_estimate", task->output_tokens) ||
      turbo_runtime_json_object_set_int64(record, "input_price_micros_per_million", (int64_t)endpoint->input_price_micros) ||
      turbo_runtime_json_object_set_int64(record, "output_price_micros_per_million", (int64_t)endpoint->output_price_micros) ||
      turbo_runtime_json_object_set_int64(record, "estimated_cost_micros", (int64_t)task->estimated_micros) ||
      turbo_runtime_json_object_set_int64(record, "budget_micros", (int64_t)task->budget_micros) ||
      turbo_runtime_json_object_set_int64(record, "max_model_turns", task->max_model_turns) ||
      turbo_runtime_json_object_set_int64(record, "heartbeat_timeout_ms", task->heartbeat_timeout_ms) ||
      turbo_runtime_json_object_set_int64(record, "started_mono_ms", (int64_t)task->started_ms) ||
      turbo_runtime_json_object_set_int64(record, "deadline_mono_ms", (int64_t)task->deadline_ms)) goto done;
  rc = admission_validate(record, task->admission_id);
  if (!rc) rc = admission_size(record);
  if (!rc) rc = turbo_agent_runtime_store_create_json_if_absent(admission->host_runtime,
      TURBO_AGENT_ROUTER_ADMISSION_COLLECTION, task->admission_id, record);
done:
  json_free(record);
  return rc;
}

int turbo_agent_router_load_intent(turbo_agent_runtime_t *host_runtime,
    const char *admission_id, json_value_t **out_record) {
  json_value_t *record = NULL;
  int rc;
  if (!out_record) return SALTS_EINVAL;
  *out_record = NULL;
  if (!host_runtime || !turbo_agent_router_id_valid_internal(admission_id)) return SALTS_EINVAL;
  rc = turbo_agent_runtime_store_get_json_status(host_runtime,
      TURBO_AGENT_ROUTER_ADMISSION_COLLECTION, admission_id, &record);
  if (!rc) rc = admission_validate(record, admission_id);
  if (!rc) rc = admission_size(record);
  if (!rc) { *out_record = record; record = NULL; }
  json_free(record);
  return rc;
}
