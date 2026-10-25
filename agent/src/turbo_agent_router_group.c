#include "turbo_agent_router_internal.h"
#include "turbo_agent_execution_internal.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_agent_state_core_internal.h"
#include <salts/clock.h>
#include <stdlib.h>
#include <string.h>

static int group_terminal(const turbo_agent_router_group_t *group) {
  size_t i;
  for (i = 0; i < group->count; ++i)
    if (turbo_agent_execution_wait(group->tasks[i]->execution, 0) != SALTS_OK) return 0;
  return 1;
}

int turbo_agent_router_group_adopt(const char *id, turbo_agent_router_task_t **tasks,
    size_t count, turbo_agent_router_group_t **out_group) {
  turbo_agent_router_group_t *group;
  size_t i, j;
  if (!out_group) return SALTS_EINVAL;
  *out_group = NULL;
  if (!turbo_agent_router_id_valid_internal(id) || !tasks || count < 2 ||
      count > TURBO_AGENT_ROUTER_MAX_ENDPOINTS) return SALTS_EINVAL;
  for (i = 0; i < count; ++i) {
    if (!tasks[i] || tasks[i]->max_model_turns || !tasks[i]->parent_run_id ||
        strlen(tasks[i]->parent_run_id) > 128) return SALTS_EINVAL;
    if (tasks[i]->group) return SALTS_EBUSY;
    if (tasks[i]->taken) return SALTS_EALREADY;
    if (tasks[i]->router != tasks[0]->router ||
        strcmp(tasks[i]->parent_run_id, tasks[0]->parent_run_id)) return SALTS_EINVAL;
    for (j = 0; j < i; ++j)
      if (tasks[i] == tasks[j] || !strcmp(tasks[i]->thread_id, tasks[j]->thread_id))
        return SALTS_EINVAL;
  }
  group = calloc(1, sizeof(*group));
  if (!group) return SALTS_ENOMEM;
  group->id = tstr_dup(id);
  if (!group->id) { free(group); return SALTS_ENOMEM; }
  group->count = count;
  for (i = 0; i < count; ++i) {
    group->tasks[i] = tasks[i];
    if (tasks[i]->observed_ms > group->observed_ms) group->observed_ms = tasks[i]->observed_ms;
    tasks[i]->group = group;
    tasks[i] = NULL;
  }
  *out_group = group;
  return SALTS_OK;
}

const char *turbo_agent_router_group_task_id(const turbo_agent_router_group_t *group, size_t index) {
  return group && index < group->count ? turbo_agent_router_task_id(group->tasks[index]) : NULL;
}

int turbo_agent_router_group_wait(turbo_agent_router_group_t *group, uint64_t timeout_ms) {
  uint64_t started, remaining, elapsed;
  size_t i;
  int rc;
  if (!group) return SALTS_EINVAL;
  started = cmeta_monotonic_ms();
  for (i = 0; i < group->count; ++i) {
    elapsed = cmeta_monotonic_ms() - started;
    remaining = timeout_ms == TURBO_RUNTIME_WAIT_INFINITE ? timeout_ms
        : elapsed < timeout_ms ? timeout_ms - elapsed : 0;
    rc = turbo_agent_execution_wait(group->tasks[i]->execution, remaining);
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

static int group_halt(turbo_agent_router_group_t *group, int reason,
                       turbo_cancel_reason_t cancel_reason) {
  size_t i;
  if (!group->failure_code) group->failure_code = reason;
  for (i = 0; i < group->count; ++i)
    (void)turbo_agent_execution_cancel(group->tasks[i]->execution, cancel_reason);
  return group->failure_code;
}

/* No policy fields are mutated/read here; current checks only compare captured
 * generations. Terminal JSON is borrowed from the sole Execution owner. */
static int group_member_failure(turbo_agent_router_task_t *task) {
  turbo_agent_execution_status_t status;
  const json_value_t *summary = NULL, *state = NULL;
  const char *kind;
  int rc;
  if (!turbo_agent_router_authority_current_internal(task)) return SALTS_EPERM;
  if (task->unresponsive) return SALTS_ECANCELED;
  if (turbo_agent_execution_wait(task->execution, 0) != SALTS_OK) return SALTS_OK;
  rc = turbo_agent_execution_get_status(task->execution, &status);
  if (rc) return rc;
  if (status == TURBO_AGENT_EXECUTION_TIMED_OUT) return SALTS_ETIMEDOUT;
  if (status == TURBO_AGENT_EXECUTION_CANCELLED) return SALTS_ECANCELED;
  if (status == TURBO_AGENT_EXECUTION_FAILED) return SALTS_EIO;
  if (status != TURBO_AGENT_EXECUTION_COMPLETED) return SALTS_EPROTO;
  if (task->taken) return SALTS_OK;
  rc = turbo_agent_execution_peek_result_internal(task->execution, &summary, &state);
  if (rc) return rc;
  kind = state ? turbo_agent_state_failure_kind(state) : NULL;
  if (!summary || !state || turbo_agent_state_pending_tool_calls(state) || (kind && kind[0]))
    return SALTS_EPROTO;
  return SALTS_OK;
}

int turbo_agent_router_group_poll(turbo_agent_router_group_t *group, uint64_t now_ms) {
  size_t i;
  int first = SALTS_OK, rc;
  if (!group || now_ms < group->observed_ms || now_ms > UINT64_C(9007199254740991))
    return SALTS_EINVAL;
  if (group->results_taken) return SALTS_EALREADY;
  group->observed_ms = now_ms;
  /* Observe everyone before cancellation, so one lost member does not hide
   * another member's elapsed heartbeat/loss evidence. */
  for (i = 0; i < group->count; ++i) {
    rc = turbo_agent_router_task_poll_internal(group->tasks[i], now_ms);
    if (!first && rc) first = rc;
    rc = group_member_failure(group->tasks[i]);
    if (!first && rc) first = rc;
  }
  if (first || group->failure_code)
    return group_halt(group, first, TURBO_CANCEL_PARENT);
  return SALTS_OK;
}

int turbo_agent_router_group_heartbeat(turbo_agent_router_group_t *group,
    size_t index, uint64_t now_ms) {
  int rc;
  if (!group || index >= group->count) return SALTS_EINVAL;
  rc = turbo_agent_router_group_poll(group, now_ms);
  if (rc) return rc;
  if (turbo_agent_execution_wait(group->tasks[index]->execution, 0) == SALTS_OK)
    return SALTS_EALREADY;
  group->tasks[index]->heartbeat_ms = now_ms;
  return SALTS_OK;
}

int turbo_agent_router_group_cancel(turbo_agent_router_group_t *group,
    turbo_cancel_reason_t reason) {
  if (!group || reason < TURBO_CANCEL_USER || reason > TURBO_CANCEL_PARENT) return SALTS_EINVAL;
  if (group->failure_code || group->results_taken) return SALTS_EALREADY;
  group_halt(group, SALTS_ECANCELED, reason);
  return SALTS_OK;
}

static int group_set(json_value_t *object, const char *key, json_value_t *value) {
  if (!value || turbo_runtime_json_object_set(object, key, value) != 0) {
    json_free(value);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

int turbo_agent_router_group_inspect(const turbo_agent_router_group_t *group,
    json_value_t **out_view) {
  json_value_t *view = NULL, *members = NULL, *member = NULL;
  uint64_t estimate = 0;
  size_t i;
  if (!out_view) return SALTS_EINVAL;
  *out_view = NULL;
  if (!group) return SALTS_EINVAL;
  view = json_create_object(); members = json_create_array();
  if (!view || !members) goto failed;
  for (i = 0; i < group->count; ++i) {
    if (turbo_agent_router_task_inspect(group->tasks[i], &member) ||
        turbo_runtime_json_array_append(members, member)) goto failed;
    member = NULL;
    /* Each admitted per-call estimate <= 2e9; at most 16 members. */
    estimate += group->tasks[i]->estimated_micros;
  }
  if (turbo_runtime_json_object_set_string(view, "group_id", group->id) ||
      turbo_runtime_json_object_set_string(view, "parent_agent_run_id", group->tasks[0]->parent_run_id) ||
      turbo_runtime_json_object_set_int64(view, "observed_mono_ms", (int64_t)group->observed_ms) ||
      turbo_runtime_json_object_set_int64(view, "failure_code", group->failure_code) ||
      turbo_runtime_json_object_set_int64(view, "estimated_cost_micros", (int64_t)estimate) ||
      group_set(view, "all_terminal", json_create_bool(group_terminal(group))) ||
      group_set(view, "input_built", json_create_bool(group->input_built)) ||
      group_set(view, "results_taken", json_create_bool(group->results_taken))) goto failed;
  if (turbo_runtime_json_object_set(view, "members", members)) goto failed;
  members = NULL;
  *out_view = view;
  return SALTS_OK;
failed:
  json_free(member); json_free(members); json_free(view);
  return SALTS_ENOMEM;
}

static int group_delivery_preflight(turbo_agent_router_group_t *group) {
  size_t i;
  if (group->results_taken) return SALTS_EALREADY;
  if (!group_terminal(group)) return SALTS_EBUSY;
  for (i = 0; i < group->count; ++i) {
    if (group->tasks[i]->unresponsive) return SALTS_ECANCELED;
    if (!turbo_agent_router_authority_current_internal(group->tasks[i])) return SALTS_EPERM;
    if (group->tasks[i]->taken) return SALTS_EALREADY;
  }
  return SALTS_OK;
}

static int group_build_input(turbo_agent_router_group_t *group, const char *host_task,
    turbo_agent_runtime_t *host_runtime, const char *id, json_value_t **out_input) {
  const json_value_t *summary = NULL, *state = NULL, *text_value;
  json_value_t *bundle = NULL, *members = NULL, *member = NULL, *candidate = NULL;
  const char *text, *run, *parent;
  char *serialized = NULL;
  size_t i;
  int rc;
  if (!out_input) return SALTS_EINVAL;
  *out_input = NULL;
  if (!group || !host_task || !host_task[0]) return SALTS_EINVAL;
  if (host_runtime && (rc = turbo_agent_router_group_observation_destination_internal(group, host_runtime, id))) return rc;
  if (group->input_built || group->results_taken) return SALTS_EALREADY;
  rc = group_delivery_preflight(group);
  if (rc) return rc;
  if (group->failure_code) return group->failure_code;
  if (strlen(host_task) > TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES) return SALTS_ENOBUFS;
  bundle = json_create_object(); members = json_create_array();
  rc = SALTS_ENOMEM;
  if (!bundle || !members) goto done;
  for (i = 0; i < group->count; ++i) {
    turbo_agent_router_task_t *task = group->tasks[i];
    rc = group_member_failure(task);
    if (rc) { group_halt(group, rc, TURBO_CANCEL_PARENT); goto done; }
    rc = turbo_agent_execution_peek_result_internal(task->execution, &summary, &state);
    if (rc) goto done;
    text_value = json_object_get(turbo_agent_state_last_event_of_kind_impl(state, "model"), "output_text");
    text = turbo_agent_state_last_output_text(state);
    run = json_get_string(summary, "run_id");
    parent = json_get_string(summary, "parent_agent_run_id");
    if (!text || !text[0] || json_string_len(text_value) != strlen(text) ||
        !run || !run[0] || strlen(run) > 128 ||
        !parent || strcmp(parent, task->parent_run_id)) { rc = SALTS_EPROTO; goto done; }
    if (strlen(text) > TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES) { rc = SALTS_ENOBUFS; goto done; }
    rc = SALTS_ENOMEM;
    member = json_create_object();
    if (!member ||
        turbo_runtime_json_object_set_string(member, "task_id", turbo_agent_router_task_id(task)) ||
        turbo_runtime_json_object_set_string(member, "endpoint", task->endpoint->id) ||
        turbo_runtime_json_object_set_string(member, "thread_id", task->thread_id) ||
        turbo_runtime_json_object_set_string(member, "run_id", run) ||
        turbo_runtime_json_object_set_clone(member, "checkpoint_id", json_object_get(summary, "checkpoint_id")) ||
        turbo_runtime_json_object_set_string(member, "parent_agent_run_id", parent) ||
        turbo_runtime_json_object_set_string(member, "output_text", text) ||
        turbo_runtime_json_array_append(members, member)) goto done;
    member = NULL;
  }
  if (turbo_runtime_json_object_set_int64(bundle, "schema_version", 1) ||
      turbo_runtime_json_object_set_string(bundle, "group_id", group->id) ||
      turbo_runtime_json_object_set_string(bundle, "host_task", host_task)) goto done;
  if (turbo_runtime_json_object_set(bundle, "worker_results", members)) goto done;
  members = NULL;
  serialized = json_serialize(bundle, NULL);
  if (!serialized) goto done;
  if (strlen(serialized) > TURBO_AGENT_ROUTER_GROUP_MAX_INPUT_BYTES) { rc = SALTS_ENOBUFS; goto done; }
  rc = SALTS_ENOMEM;
  candidate = turbo_agent_session_create_input_state_json_value(serialized);
  if (!candidate) goto done;
  if (host_runtime && (rc = turbo_agent_router_group_observation_write_internal(group,
          host_runtime, id, "aggregation_input"))) goto done;
  *out_input = candidate; candidate = NULL;
  group->input_built = 1;
  rc = SALTS_OK;
done:
  json_serialize_free(serialized);
  json_free(member); json_free(members); json_free(bundle); json_free(candidate);
  return rc;
}

int turbo_agent_router_group_build_input(turbo_agent_router_group_t *group,
    const char *host_task, json_value_t **out_input) {
  return group_build_input(group, host_task, NULL, NULL, out_input);
}

int turbo_agent_router_group_build_input_with_observation(turbo_agent_router_group_t *group,
    const char *host_task, turbo_agent_runtime_t *host_runtime, const char *id, json_value_t **out_input) {
  if (!host_runtime || !turbo_agent_router_id_valid_internal(id)) {
    if (out_input) *out_input = NULL;
    return SALTS_EINVAL;
  }
  return group_build_input(group, host_task, host_runtime, id, out_input);
}

static int group_take_results(turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id,
    turbo_agent_router_group_result_t *results, size_t capacity, size_t *out_count) {
  size_t i;
  int rc;
  if (!out_count) return SALTS_EINVAL;
  *out_count = 0;
  if (!group || !results || capacity < group->count) return SALTS_EINVAL;
  memset(results, 0, sizeof(*results) * group->count);
  if (host_runtime && (rc = turbo_agent_router_group_observation_destination_internal(group, host_runtime, id))) return rc;
  rc = group_delivery_preflight(group);
  if (rc) return rc;
  if (host_runtime && (rc = turbo_agent_router_group_observation_write_internal(group,
          host_runtime, id, "result_collection"))) return rc;
  /* No allocations follow these checks. Exclusive group ownership prevents
   * any other result taker between preflight and the individual moves. */
  for (i = 0; i < group->count; ++i) {
    rc = turbo_agent_execution_take_result(group->tasks[i]->execution,
        &results[i].summary, &results[i].state);
    if (rc) return rc;
    group->tasks[i]->taken = 1;
    group->results_taken = 1;
    ++*out_count;
  }
  return SALTS_OK;
}

int turbo_agent_router_group_take_results(turbo_agent_router_group_t *group,
    turbo_agent_router_group_result_t *results, size_t capacity, size_t *out_count) {
  return group_take_results(group, NULL, NULL, results, capacity, out_count);
}

int turbo_agent_router_group_take_results_with_observation(turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id,
    turbo_agent_router_group_result_t *results, size_t capacity, size_t *out_count) {
  if (!host_runtime || !turbo_agent_router_id_valid_internal(id)) {
    if (out_count) *out_count = 0;
    return SALTS_EINVAL;
  }
  return group_take_results(group, host_runtime, id, results, capacity, out_count);
}

int turbo_agent_router_group_release(turbo_agent_router_group_t *group) {
  size_t i;
  if (!group) return SALTS_OK;
  if (!group_terminal(group)) return SALTS_EBUSY;
  for (i = 0; i < group->count; ++i) {
    group->tasks[i]->group = NULL;
    (void)turbo_agent_router_task_release(group->tasks[i]);
  }
  tstr_free(group->id);
  free(group);
  return SALTS_OK;
}
