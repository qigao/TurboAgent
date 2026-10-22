#ifndef SINGLE_AGENT_REPORT_EXAMPLE_H
#define SINGLE_AGENT_REPORT_EXAMPLE_H

#include "document_render_tool.h"
#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include <salts/clock.h>

/* Example-only synchronous Host composition, not an installed API. The caller
 * exclusively owns the Router, pool, agents, stores and immutable registry.
 * Every accepted handle is drained before return, including partial admission.
 * A noncooperative backend can therefore block cleanup: cancellation is a
 * request, never permission to destroy live dependencies. */
typedef int (*single_agent_report_review_fn)(const json_value_t *summary,
    const json_value_t *state, void *user); /* 1 approve, 0 reject, negative abort */

typedef struct single_agent_report_config_s {
  turbo_agent_router_t *router;
  turbo_agent_router_t *tool_router; /* Separate reviewed endpoint pool, same Host. */
  cmeta_threadpool_t *pool;
  turbo_agent_t *authority;
  const turbo_tool_registry_t *tools;
  const char *parent_run_id; /* Host-supplied lineage, not a fabricated Runtime run */
  const char *goal;
  const char *brief;
  turbo_agent_router_request_t worker, aggregate, execute;
  uint64_t reserved_budget_micros;
  unsigned timeout_ms;
  single_agent_report_review_fn review;
  void *review_user;
} single_agent_report_config_t;

/* A presence-only guard is insufficient. This example permits exactly the
 * reviewed renderer, and requires an exact canonical approval for every call. */
static int single_agent_report_guard(turbo_agent_t *agent, const json_value_t *state,
    const char *id, const char *name, const char *args, const char **reason, void *user) {
  (void)agent; (void)user;
  if (!name || strcmp(name, "document_render")) { *reason = "report_tool_denied"; return -1; }
  if (!turbo_agent_tool_approval_review_matches(state, id, name, args)) {
    *reason = "approval_required";
    return -1;
  }
  return 0;
}

static int single_agent_report_input(const json_value_t *data, json_value_t **out) {
  char *text = json_serialize(data, NULL);
  int rc = SALTS_ENOMEM;
  *out = NULL;
  if (text) {
    if (strlen(text) > TURBO_AGENT_ROUTER_GROUP_MAX_INPUT_BYTES) rc = SALTS_ENOBUFS;
    else {
      *out = turbo_agent_session_create_input_state_json_value(text);
      if (*out) rc = SALTS_OK;
    }
  }
  json_serialize_free(text);
  return rc;
}

static int single_agent_report_request(turbo_agent_router_request_t *request,
    uint64_t deadline, const char *thread, turbo_agent_runtime_parent_link_t *parent) {
  uint64_t now = cmeta_monotonic_ms();
  if (now >= deadline) return SALTS_ETIMEDOUT;
  request->timeout_ms = (unsigned)(deadline - now);
  /* This local synchronous example uses its single absolute deadline as the
   * liveness bound. It does not invent backend heartbeat evidence. */
  request->heartbeat_timeout_ms = request->timeout_ms;
  request->thread_id = thread;
  request->parent_link = parent;
  return SALTS_OK;
}

static int single_agent_report_wait(turbo_agent_router_task_t *task, uint64_t deadline) {
  int rc;
  for (;;) {
    uint64_t now = cmeta_monotonic_ms();
    if (now >= deadline) return SALTS_ETIMEDOUT;
    rc = turbo_agent_router_task_poll(task, now);
    if (rc) return rc;
    rc = turbo_agent_router_task_wait(task, 10);
    if (rc != SALTS_ETIMEDOUT) return rc;
  }
}

/* Transfer success alone is not completion. Check Execution and canonical state
 * separately; a failed Runtime may have no summary or committed checkpoint. */
static int single_agent_report_take(turbo_agent_router_task_t *task, const char *parent,
    json_value_t **summary, json_value_t **state) {
  json_value_t *view = NULL;
  const char *status, *lineage, *failure;
  int rc = turbo_agent_router_task_inspect(task, &view);
  if (rc) return rc;
  if (turbo_runtime_json_value_as_int64(json_object_get(view, "execution_status"), -1) !=
          TURBO_AGENT_EXECUTION_COMPLETED ||
      turbo_runtime_json_value_as_int64(json_object_get(view, "runtime_result_code"), -1) != 0)
    rc = SALTS_EIO;
  json_free(view);
  if (rc) return rc;
  rc = turbo_agent_router_task_take_result(task, summary, state);
  if (rc) return rc;
  status = *summary ? json_get_string(*summary, "status") : NULL;
  lineage = *summary ? json_get_string(*summary, "parent_agent_run_id") : NULL;
  failure = *state ? turbo_agent_state_failure_kind(*state) : NULL;
  return status && !strcmp(status, "completed") && lineage && !strcmp(lineage, parent) &&
      *state && !turbo_agent_state_pending_tool_calls(*state) && (!failure || !failure[0])
      ? SALTS_OK : SALTS_EPROTO;
}

static int single_agent_report_deliver(const turbo_tool_registry_t *tools,
    const json_value_t *state, json_value_t **out) {
  const json_value_t *outputs = turbo_agent_state_tool_results_outputs(
      turbo_agent_state_latest_tool_results_event(state));
  const char *reason = json_get_string(json_object_get(state, "router_tool_loop"), "reason");
  const char *text;
  json_value_t *result;
  /* One tool and two model turns: a fluent answer without a tool receipt fails.
   * The renderer checks reference identities, not factual claim support. */
  if (!reason || strcmp(reason, "completed") || json_array_size(outputs) != 1)
    return SALTS_EPROTO;
  const json_value_t *payload = json_object_get(json_array_get(outputs, 0), "output");
  text = json_string(payload);
  if (!text || !text[0] || strlen(text) != json_string_len(payload) ||
      strlen(text) > DOCUMENT_RENDER_MAX_OUTPUT_BYTES * 6u + 512u)
    return SALTS_EPROTO;
  result = json_parse(text, strlen(text));
  if (!result || turbo_tool_schema_validate_result_json_value(tools, "document_render", result,
          NULL, 0) != TURBO_TOOL_SCHEMA_VALID ||
      !document_render_text_valid(json_object_get(result, "markdown"),
          DOCUMENT_RENDER_MAX_OUTPUT_BYTES, 1)) {
    json_free(result);
    return SALTS_EPROTO;
  }
  *out = result;
  return SALTS_OK;
}

static const json_value_t *single_agent_report_model_text(const json_value_t *state) {
  size_t count = turbo_agent_state_event_count(state);
  while (count) {
    const json_value_t *event = turbo_agent_state_event_at(state, --count);
    const char *kind = json_get_string(event, "kind");
    if (kind && !strcmp(kind, "model")) return json_object_get(event, "output_text");
  }
  return NULL;
}

static int single_agent_report_run(const single_agent_report_config_t *config,
    json_value_t **out_delivery, const char **out_phase) {
  turbo_agent_router_task_t *workers[2] = {0}, *task = NULL;
  turbo_agent_router_group_t *group = NULL;
  turbo_agent_tool_authority_snapshot_t authority;
  turbo_agent_runtime_parent_link_t parent = {0};
  turbo_agent_router_request_t request;
  json_value_t *data = NULL, *input = NULL, *next = NULL, *summary = NULL, *state = NULL;
  uint64_t deadline, budget;
  int rc = SALTS_EINVAL;
  static const char *threads[] = {"report-worker-a", "report-worker-b"};
  if (!out_delivery || !out_phase) return rc;
  *out_delivery = NULL; *out_phase = "admission";
  if (!config || !config->router || !config->tool_router || !config->pool || !config->authority || !config->tools ||
      !config->review || !config->parent_run_id || !config->parent_run_id[0] ||
      strlen(config->parent_run_id) > 128 || !config->goal || !config->goal[0] ||
      !config->brief || !config->brief[0] || !config->timeout_ms || config->timeout_ms > 86400000)
    return rc;
  if (strlen(config->goal) > TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES ||
      strlen(config->brief) > TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES) return SALTS_ENOBUFS;
  /* Reserve request ceilings before any worker starts. Estimates are not an
   * actual charge, a provider token bound or a hard spending guarantee. */
  if (config->worker.budget_micros > UINT64_MAX / 2) return SALTS_ERANGE;
  budget = config->worker.budget_micros * 2;
  if (UINT64_MAX - budget < config->aggregate.budget_micros) return SALTS_ERANGE;
  budget += config->aggregate.budget_micros;
  if (UINT64_MAX - budget < config->execute.budget_micros) return SALTS_ERANGE;
  budget += config->execute.budget_micros;
  if (budget > config->reserved_budget_micros) return SALTS_ENOBUFS;
  rc = turbo_agent_tool_authority_snapshot_get(config->authority, &authority);
  if (rc) return rc;
  deadline = cmeta_monotonic_ms();
  if (UINT64_MAX - deadline < config->timeout_ms) return SALTS_ERANGE;
  deadline += config->timeout_ms;
  parent.parent_agent_run_id = config->parent_run_id;
  data = json_create_object();
  rc = SALTS_ENOMEM;
  if (!data || turbo_runtime_json_object_set_string(data, "host_goal", config->goal) ||
      turbo_runtime_json_object_set_string(data, "source_id", "brief") ||
      turbo_runtime_json_object_set_string(data, "source_revision", "host-v1") ||
      turbo_runtime_json_object_set_string(data, "source_text", config->brief)) goto done;
  rc = single_agent_report_input(data, &input);
  if (rc) goto done;
  *out_phase = "workers";
  for (size_t i = 0; i < 2; ++i) {
    request = config->worker; request.role = TURBO_AGENT_ROUTER_EXECUTE;
    request.required_capabilities = TURBO_AGENT_ROUTER_TEXT;
    rc = single_agent_report_request(&request, deadline, threads[i], &parent);
    if (!rc) rc = turbo_agent_router_submit(config->router, config->pool, &request, input, &workers[i]);
    if (rc) goto done; /* Earlier handles still belong to this Host. */
  }
  rc = turbo_agent_router_group_adopt("report-analysis", workers, 2, &group);
  if (rc) goto done;
  for (;;) {
    uint64_t now = cmeta_monotonic_ms();
    if (now >= deadline) { rc = SALTS_ETIMEDOUT; goto done; }
    rc = turbo_agent_router_group_poll(group, now);
    if (rc) goto done;
    rc = turbo_agent_router_group_wait(group, 10);
    if (rc != SALTS_ETIMEDOUT) break;
  }
  if (!rc) rc = turbo_agent_router_group_build_input(group, config->goal, &next);
  if (rc) goto done;
  rc = turbo_agent_router_group_release(group);
  if (rc) goto done;
  group = NULL;
  json_free(input); input = next; next = NULL;
  *out_phase = "aggregation";
  if (turbo_agent_tool_authority_snapshot_is_current(config->authority, &authority) != 1) {
    rc = SALTS_EPERM; goto done;
  }
  request = config->aggregate; request.role = TURBO_AGENT_ROUTER_PLAN;
  request.required_capabilities = TURBO_AGENT_ROUTER_TEXT;
  rc = single_agent_report_request(&request, deadline, "report-aggregation", &parent);
  if (!rc) rc = turbo_agent_router_submit(config->router, config->pool, &request, input, &task);
  if (!rc) rc = single_agent_report_wait(task, deadline);
  if (!rc) rc = single_agent_report_take(task, config->parent_run_id, &summary, &state);
  if (rc) goto done;
  const char *draft = turbo_agent_state_last_output_text(state);
  const char *run = json_get_string(summary, "run_id");
  if (!draft || !draft[0] || !run || !run[0] ||
      json_string_len(single_agent_report_model_text(state)) != strlen(draft)) {
    rc = SALTS_EPROTO; goto done;
  }
  if (strlen(draft) > TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES) { rc = SALTS_ENOBUFS; goto done; }
  /* Preserve Host context; aggregation text cannot become instructions, an
   * approval flag, a model selection or the Host source catalog. */
  rc = SALTS_ENOMEM;
  if (turbo_runtime_json_object_set_string(data, "aggregation_run_id", run) ||
      turbo_runtime_json_object_set_string(data, "aggregation_text", draft)) goto done;
  rc = single_agent_report_input(data, &next);
  if (rc) goto done;
  rc = turbo_agent_router_task_release(task);
  if (rc) goto done;
  task = NULL;
  json_free(summary); json_free(state); summary = state = NULL;
  json_free(input); input = next; next = NULL;
  *out_phase = "execution";
  if (turbo_agent_tool_authority_snapshot_is_current(config->authority, &authority) != 1) {
    rc = SALTS_EPERM; goto done;
  }
  request = config->execute; request.role = TURBO_AGENT_ROUTER_EXECUTE;
  request.required_capabilities = TURBO_AGENT_ROUTER_TEXT | TURBO_AGENT_ROUTER_TOOL_PROPOSALS;
  rc = single_agent_report_request(&request, deadline, "report-delivery", &parent);
  if (!rc) rc = turbo_agent_router_submit_tool_loop(config->tool_router, config->pool, &request, input, 2, &task);
  if (!rc) rc = single_agent_report_wait(task, deadline);
  if (rc) goto done;
  json_value_t *view = NULL;
  rc = turbo_agent_router_task_inspect(task, &view);
  int pending = view && json_get_bool(view, "approval_pending", 0);
  if (!rc) {
    int64_t status = turbo_runtime_json_value_as_int64(json_object_get(view, "execution_status"), -1);
    if (status != TURBO_AGENT_EXECUTION_COMPLETED && status != TURBO_AGENT_EXECUTION_INTERRUPTED)
      rc = status == TURBO_AGENT_EXECUTION_TIMED_OUT ? SALTS_ETIMEDOUT
          : status == TURBO_AGENT_EXECUTION_CANCELLED ? SALTS_ECANCELED : SALTS_EIO;
  }
  json_free(view);
  if (rc) goto done;
  if (!pending) { rc = SALTS_EPROTO; goto done; }
  if (pending) {
    *out_phase = "approval";
    rc = turbo_agent_router_task_take_result(task, &summary, &state);
    if (rc) goto done;
    /* The callback runs on the serialized Host after every execution segment
     * is terminal. Borrowed review data expires when the callback returns. */
    int approved = config->review(summary, state, config->review_user);
    if (approved < 0) { rc = SALTS_ECANCELED; goto done; }
    if (approved != 0 && approved != 1) { rc = SALTS_EINVAL; goto done; }
    if (turbo_agent_tool_authority_snapshot_is_current(config->authority, &authority) != 1) {
      rc = SALTS_EPERM; goto done;
    }
    rc = turbo_agent_router_task_review(task, config->pool, approved);
    if (!rc) rc = single_agent_report_wait(task, deadline);
    if (rc) goto done;
    if (!approved) { rc = SALTS_ECANCELED; goto done; }
    json_free(summary); json_free(state); summary = state = NULL;
  }
  *out_phase = "delivery";
  if (turbo_agent_tool_authority_snapshot_is_current(config->authority, &authority) != 1) {
    rc = SALTS_EPERM; goto done;
  }
  rc = single_agent_report_take(task, config->parent_run_id, &summary, &state);
  if (!rc) rc = single_agent_report_deliver(config->tools, state, out_delivery);
done:
  /* Cancel, then wait for actual terminal. Never detach work on a timeout or
   * release the Router/Agent/store while a callback can still use it. */
  if (group) {
    (void)turbo_agent_router_group_cancel(group, TURBO_CANCEL_PARENT);
    (void)turbo_agent_router_group_wait(group, TURBO_RUNTIME_WAIT_INFINITE);
    (void)turbo_agent_router_group_release(group);
  }
  for (size_t i = 0; i < 2; ++i) if (workers[i]) {
    (void)turbo_agent_router_task_cancel(workers[i], TURBO_CANCEL_PARENT);
    (void)turbo_agent_router_task_wait(workers[i], TURBO_RUNTIME_WAIT_INFINITE);
    (void)turbo_agent_router_task_release(workers[i]);
  }
  if (task) {
    (void)turbo_agent_router_task_cancel(task, TURBO_CANCEL_PARENT);
    (void)turbo_agent_router_task_wait(task, TURBO_RUNTIME_WAIT_INFINITE);
    (void)turbo_agent_router_task_release(task);
  }
  json_free(data); json_free(input); json_free(next); json_free(summary); json_free(state);
  return rc;
}

#endif
