#include "turbo_agent_router.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime_internal.h"

#include <salts/clock.h>
#include <tstr.h>
#include <stdlib.h>
#include <string.h>

typedef struct router_endpoint_s {
  turbo_agent_router_endpoint_t config;
  tstr id;
  int busy;
} router_endpoint_t;

struct turbo_agent_router_s {
  turbo_agent_t *authority_agent;
  size_t count;
  router_endpoint_t endpoints[TURBO_AGENT_ROUTER_MAX_ENDPOINTS];
};

struct turbo_agent_router_task_s {
  turbo_agent_router_t *router;
  router_endpoint_t *endpoint;
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution;
  turbo_agent_tool_authority_snapshot_t parent_authority, child_authority;
  tstr thread_id;
  uint64_t started_ms, observed_ms, heartbeat_ms, estimated_micros;
  uint64_t budget_micros;
  unsigned heartbeat_timeout_ms, difficulty, input_tokens, output_tokens, required_capabilities;
  turbo_agent_router_role_t role;
  int unresponsive, taken;
};

static int router_id_valid(const char *text) {
  size_t i, n = text ? strlen(text) : 0;
  if (!n || n > 64) return 0;
  for (i = 0; i < n; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
  }
  return 1;
}

void turbo_agent_router_config_init(turbo_agent_router_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_ROUTER_ABI_VERSION;
}

void turbo_agent_router_request_init(turbo_agent_router_request_t *request) {
  if (!request) return;
  memset(request, 0, sizeof(*request));
  request->struct_size = sizeof(*request);
  request->abi_version = TURBO_AGENT_ROUTER_ABI_VERSION;
  request->required_capabilities = TURBO_AGENT_ROUTER_TEXT;
}

int turbo_agent_router_destroy(turbo_agent_router_t *router) {
  size_t i;
  if (!router) return SALTS_OK;
  for (i = 0; i < router->count; ++i)
    if (router->endpoints[i].busy) return SALTS_EBUSY;
  for (i = 0; i < router->count; ++i) tstr_free(router->endpoints[i].id);
  free(router);
  return SALTS_OK;
}

int turbo_agent_router_create(const turbo_agent_router_config_t *config,
                               turbo_agent_router_t **out_router) {
  turbo_agent_router_t *router;
  size_t i, j;
  if (!out_router) return SALTS_EINVAL;
  *out_router = NULL;
  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_ROUTER_ABI_VERSION || !config->authority_agent ||
      !config->endpoints || !config->endpoint_count ||
      config->endpoint_count > TURBO_AGENT_ROUTER_MAX_ENDPOINTS) return SALTS_EINVAL;
  for (i = 0; i < config->endpoint_count; ++i) {
    const turbo_agent_router_endpoint_t *e = &config->endpoints[i];
    if (!router_id_valid(e->id) || !e->agent || !e->runtime ||
        e->agent == config->authority_agent || !e->roles || (e->roles & ~3u) ||
        !e->capabilities || (e->capabilities & ~7u) ||
        e->max_difficulty > 100 || !e->max_input_tokens || e->max_input_tokens > 1000000 ||
        !e->max_output_tokens || e->max_output_tokens > 1000000 ||
        e->input_price_micros > UINT64_C(1000000000) ||
        e->output_price_micros > UINT64_C(1000000000)) return SALTS_EINVAL;
    for (j = 0; j < i; ++j) {
      const turbo_agent_router_endpoint_t *previous = &config->endpoints[j];
      if (strcmp(e->id, previous->id) == 0 || e->agent == previous->agent ||
          e->runtime == previous->runtime) return SALTS_EINVAL;
    }
  }
  router = calloc(1, sizeof(*router));
  if (!router) return SALTS_ENOMEM;
  router->authority_agent = config->authority_agent;
  router->count = config->endpoint_count;
  for (i = 0; i < router->count; ++i) {
    router->endpoints[i].config = config->endpoints[i];
    router->endpoints[i].id = tstr_dup(config->endpoints[i].id);
    if (!router->endpoints[i].id) {
      turbo_agent_router_destroy(router);
      return SALTS_ENOMEM;
    }
    router->endpoints[i].config.id = router->endpoints[i].id;
  }
  *out_router = router;
  return SALTS_OK;
}

static int router_authority_current(const turbo_agent_router_task_t *task) {
  return turbo_agent_tool_authority_snapshot_is_current(
             task->router->authority_agent, &task->parent_authority) == 1 &&
         turbo_agent_tool_authority_snapshot_is_current(
             task->endpoint->config.agent, &task->child_authority) == 1;
}

static int router_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_router_task_t *task = user_data;
  if (!router_authority_current(task)) return SALTS_EPERM;
  return turbo_agent_model_node(ctx, task->endpoint->config.agent);
}

/* Bounds admitted above make each product <= 1e15 and the sum <= 2e9.
 * Round each direction up so a fractional micro-unit cannot evade admission. */
static uint64_t router_estimated_cost(const turbo_agent_router_endpoint_t *e,
                                       const turbo_agent_router_request_t *r) {
  return ((uint64_t)r->input_tokens * e->input_price_micros + 999999) / 1000000 +
         ((uint64_t)r->output_tokens * e->output_price_micros + 999999) / 1000000;
}

static int router_set(json_value_t *object, const char *key, json_value_t *value) {
  if (!value || turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    json_free(value);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static json_value_t *router_decision(const turbo_agent_router_task_t *task) {
  json_value_t *view = json_create_object();
  const turbo_agent_t *agent = task->endpoint->config.agent;
  if (!view ||
      turbo_runtime_json_object_set_string(view, "endpoint", task->endpoint->id) ||
      turbo_runtime_json_object_set_string(view, "model", agent->model) ||
      turbo_runtime_json_object_set_string(view, "provider", turbo_agent_provider_name(agent)) ||
      turbo_runtime_json_object_set_string(view, "role",
          task->role == TURBO_AGENT_ROUTER_PLAN ? "plan" : "execute") ||
      turbo_runtime_json_object_set_string(view, "reason", "lowest_estimated_cost_qualified_idle") ||
      turbo_runtime_json_object_set_int64(view, "difficulty", task->difficulty) ||
      turbo_runtime_json_object_set_int64(view, "required_model_capabilities", task->required_capabilities) ||
      turbo_runtime_json_object_set_int64(view, "input_tokens_estimate", task->input_tokens) ||
      turbo_runtime_json_object_set_int64(view, "output_tokens_estimate", task->output_tokens) ||
      turbo_runtime_json_object_set_int64(view, "input_price_micros_per_million",
                                         (int64_t)task->endpoint->config.input_price_micros) ||
      turbo_runtime_json_object_set_int64(view, "output_price_micros_per_million",
                                         (int64_t)task->endpoint->config.output_price_micros) ||
      turbo_runtime_json_object_set_int64(view, "estimated_cost_micros", (int64_t)task->estimated_micros) ||
      turbo_runtime_json_object_set_int64(view, "budget_micros", (int64_t)task->budget_micros) ||
      router_set(view, "actual_cost_micros", json_create_null())) {
    json_free(view);
    return NULL;
  }
  return view;
}

int turbo_agent_router_submit(turbo_agent_router_t *router, cmeta_threadpool_t *executor,
    const turbo_agent_router_request_t *request, const json_value_t *input,
    turbo_agent_router_task_t **out_task) {
  turbo_agent_tool_authority_snapshot_t parent = {0}, child = {0}, selected_child = {0};
  turbo_agent_router_task_t *task;
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  turbo_agent_execution_options_t options = {0};
  turbo_graph_run_options_t graph_options = {0};
  json_value_t *state = NULL, *decision = NULL;
  router_endpoint_t *selected = NULL;
  uint64_t best_cost = UINT64_MAX, now;
  size_t i;
  int busy = 0, rc;
  if (!out_task) return SALTS_EINVAL;
  *out_task = NULL;
  if (!router || !executor || !request || request->struct_size < sizeof(*request) ||
      request->abi_version != TURBO_AGENT_ROUTER_ABI_VERSION ||
      (request->role != TURBO_AGENT_ROUTER_PLAN && request->role != TURBO_AGENT_ROUTER_EXECUTE) ||
      request->difficulty > 100 || !request->input_tokens || request->input_tokens > 1000000 ||
      !request->required_capabilities || (request->required_capabilities & ~7u) ||
      !request->output_tokens || request->output_tokens > 1000000 ||
      request->budget_micros > UINT64_C(9007199254740991) ||
      !request->timeout_ms || request->timeout_ms > 86400000 ||
      !request->heartbeat_timeout_ms || request->heartbeat_timeout_ms > request->timeout_ms ||
      !router_id_valid(request->thread_id) || !request->parent_link ||
      !request->parent_link->parent_agent_run_id || !request->parent_link->parent_agent_run_id[0] ||
      !input || json_type(input) != JSON_OBJECT) return SALTS_EINVAL;
  if (turbo_agent_tool_authority_snapshot_get(router->authority_agent, &parent) != 0 ||
      !(parent.capability_mask & TURBO_AGENT_TOOL_AUTHORITY_CAPABILITY_BIT(
                                    TURBO_AGENT_POLICY_CAPABILITY_DELEGATE))) return SALTS_EPERM;
  for (i = 0; i < router->count; ++i) {
    router_endpoint_t *endpoint = &router->endpoints[i];
    const turbo_agent_router_endpoint_t *e = &endpoint->config;
    uint64_t cost;
    if (!(e->roles & (unsigned)request->role) || e->max_difficulty < request->difficulty ||
        (e->capabilities & request->required_capabilities) != request->required_capabilities ||
        e->max_input_tokens < request->input_tokens ||
        e->max_output_tokens < request->output_tokens) continue;
    cost = router_estimated_cost(e, request);
    if (cost > request->budget_micros) continue;
    /* Never read the ordinary policy fields of an occupied child. */
    if (endpoint->busy) { busy = 1; continue; }
    if (turbo_agent_tool_authority_snapshot_get(e->agent, &child) != 0 ||
        (child.capability_mask & ~parent.capability_mask)) continue;
    if (cost < best_cost) {
      selected = endpoint;
      selected_child = child;
      best_cost = cost;
    }
  }
  if (!selected) return busy ? SALTS_EBUSY : SALTS_ENOENT;
  now = cmeta_monotonic_ms();
  if (UINT64_MAX - now < request->timeout_ms) return SALTS_ERANGE;
  task = calloc(1, sizeof(*task));
  if (!task) return SALTS_ENOMEM;
  task->router = router;
  task->endpoint = selected;
  task->parent_authority = parent;
  task->child_authority = selected_child;
  task->thread_id = tstr_dup(request->thread_id);
  task->started_ms = task->observed_ms = task->heartbeat_ms = now;
  task->heartbeat_timeout_ms = request->heartbeat_timeout_ms;
  task->estimated_micros = best_cost;
  task->budget_micros = request->budget_micros;
  task->difficulty = request->difficulty;
  task->required_capabilities = request->required_capabilities;
  task->input_tokens = request->input_tokens;
  task->output_tokens = request->output_tokens;
  task->role = request->role;
  task->graph = turbo_graph_create("tiered-model-call");
  state = json_clone(input);
  decision = router_decision(task);
  rc = SALTS_ENOMEM;
  if (!task->thread_id || !task->graph || !state || !decision) goto rejected;
  if (turbo_runtime_json_object_set(state, "model_route", decision) != TURBO_RUNTIME_JSON_OK)
    goto rejected;
  decision = NULL;
  if (turbo_graph_add_node(task->graph, "model", router_model_node, task) != TURBO_GRAPH_EXEC_OK ||
      turbo_graph_set_entry(task->graph, "model") != TURBO_GRAPH_EXEC_OK) goto rejected;
  graph_options.max_steps = 2;
  runtime_options.thread_id = task->thread_id;
  runtime_options.parent_link = request->parent_link;
  options.struct_size = sizeof(options);
  options.abi_version = TURBO_AGENT_EXECUTION_ABI_VERSION;
  options.deadline_mono_ms = now + request->timeout_ms;
  selected->busy = 1;
  rc = turbo_agent_execution_start(executor, selected->config.runtime, task->graph, state,
      &graph_options, &runtime_options, &options, &task->execution);
  if (rc != SALTS_OK) { selected->busy = 0; goto rejected; }
  json_free(state);
  *out_task = task;
  return SALTS_OK;
rejected:
  json_free(state);
  json_free(decision);
  turbo_graph_destroy(task->graph);
  tstr_free(task->thread_id);
  free(task);
  return rc;
}

const char *turbo_agent_router_task_id(const turbo_agent_router_task_t *task) {
  return task ? turbo_agent_execution_id(task->execution) : NULL;
}

int turbo_agent_router_task_wait(turbo_agent_router_task_t *task, uint64_t timeout_ms) {
  return task ? turbo_agent_execution_wait(task->execution, timeout_ms) : SALTS_EINVAL;
}

int turbo_agent_router_task_cancel(turbo_agent_router_task_t *task, turbo_cancel_reason_t reason) {
  return task ? turbo_agent_execution_cancel(task->execution, reason) : SALTS_EINVAL;
}

int turbo_agent_router_task_poll(turbo_agent_router_task_t *task, uint64_t now_ms) {
  int rc;
  if (!task || now_ms < task->observed_ms || now_ms > UINT64_C(9007199254740991)) return SALTS_EINVAL;
  task->observed_ms = now_ms;
  if (task->unresponsive) return SALTS_ECANCELED;
  if (turbo_agent_execution_wait(task->execution, 0) == SALTS_OK) return SALTS_OK;
  if (now_ms - task->heartbeat_ms < task->heartbeat_timeout_ms) return SALTS_OK;
  task->unresponsive = 1;
  rc = turbo_agent_execution_cancel(task->execution, TURBO_CANCEL_PARENT);
  return rc == SALTS_OK || rc == SALTS_EALREADY ? SALTS_ECANCELED : rc;
}

int turbo_agent_router_task_heartbeat(turbo_agent_router_task_t *task, uint64_t now_ms) {
  int rc = turbo_agent_router_task_poll(task, now_ms);
  if (rc != SALTS_OK) return rc;
  if (turbo_agent_execution_wait(task->execution, 0) == SALTS_OK) return SALTS_EALREADY;
  task->heartbeat_ms = now_ms;
  return SALTS_OK;
}

int turbo_agent_router_task_inspect(const turbo_agent_router_task_t *task, json_value_t **out_view) {
  turbo_agent_execution_status_t status;
  json_value_t *view;
  int operation_rc;
  if (!out_view) return SALTS_EINVAL;
  *out_view = NULL;
  if (!task || turbo_agent_execution_get_status(task->execution, &status) != SALTS_OK)
    return SALTS_EINVAL;
  view = router_decision(task);
  if (!view ||
      turbo_runtime_json_object_set_string(view, "execution_id", turbo_agent_router_task_id(task)) ||
      turbo_runtime_json_object_set_string(view, "thread_id", task->thread_id) ||
      turbo_runtime_json_object_set_int64(view, "execution_status", status) ||
      turbo_runtime_json_object_set_int64(view, "heartbeat_mono_ms", (int64_t)task->heartbeat_ms) ||
      turbo_runtime_json_object_set_int64(view, "observed_elapsed_ms",
                                         (int64_t)(task->observed_ms - task->started_ms)) ||
      router_set(view, "unresponsive", json_create_bool(task->unresponsive)) ||
      router_set(view, "result_taken", json_create_bool(task->taken))) goto failed;
  if (turbo_agent_execution_result_code(task->execution, &operation_rc) == SALTS_OK) {
    if (turbo_runtime_json_object_set_int64(view, "runtime_result_code", operation_rc)) goto failed;
  } else if (router_set(view, "runtime_result_code", json_create_null())) goto failed;
  *out_view = view;
  return SALTS_OK;
failed:
  json_free(view);
  return SALTS_ENOMEM;
}

int turbo_agent_router_task_take_result(turbo_agent_router_task_t *task,
    json_value_t **out_summary, json_value_t **out_state) {
  int rc;
  if (out_summary) *out_summary = NULL;
  if (out_state) *out_state = NULL;
  if (!task || !out_summary || !out_state) return SALTS_EINVAL;
  if (task->taken) return SALTS_EALREADY;
  if (turbo_agent_execution_wait(task->execution, 0) != SALTS_OK) return SALTS_EBUSY;
  if (task->unresponsive) return SALTS_ECANCELED;
  if (!router_authority_current(task)) return SALTS_EPERM;
  rc = turbo_agent_execution_take_result(task->execution, out_summary, out_state);
  if (rc == SALTS_OK) task->taken = 1;
  return rc;
}

int turbo_agent_router_task_release(turbo_agent_router_task_t *task) {
  if (!task) return SALTS_OK;
  if (turbo_agent_execution_wait(task->execution, 0) != SALTS_OK) return SALTS_EBUSY;
  turbo_agent_execution_release(task->execution);
  turbo_graph_destroy(task->graph);
  task->endpoint->busy = 0;
  tstr_free(task->thread_id);
  free(task);
  return SALTS_OK;
}
