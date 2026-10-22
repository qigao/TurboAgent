#include "turbo_agent_router.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_execution_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_workflow_graph_internal.h"
#include "turbo_tool_registry.h"

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
  int tool_lane_busy;
  router_endpoint_t endpoints[TURBO_AGENT_ROUTER_MAX_ENDPOINTS];
};

struct turbo_agent_router_task_s {
  turbo_agent_router_t *router;
  router_endpoint_t *endpoint;
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution, *identity_execution;
  tstr review_checkpoint;
  uint64_t deadline_ms;
  unsigned max_model_turns, model_turns, review_resumes;
  int pending_review, review_cancelled;
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

static int router_set(json_value_t *object, const char *key, json_value_t *value);

static int router_loop_record(turbo_agent_router_task_t *task, json_value_t *state,
                               const char *reason) {
  json_value_t *record = json_create_object();
  if (!record ||
      turbo_runtime_json_object_set_int64(record, "schema_version", 1) ||
      turbo_runtime_json_object_set_int64(record, "model_turns", task->model_turns) ||
      turbo_runtime_json_object_set_int64(record, "max_model_turns", task->max_model_turns) ||
      turbo_runtime_json_object_set_string(record, "reason", reason)) {
    json_free(record);
    return SALTS_ENOMEM;
  }
  return router_set(state, "router_tool_loop", record);
}

/* Interrupt, rather than let the model reinterpret a rejected/unknown effect.
 * The checkpoint is evidence, not permission to automatically retry it. */
static int router_loop_halt(turbo_graph_exec_ctx_t *ctx, turbo_agent_router_task_t *task,
                             const char *reason) {
  if (turbo_agent_state_set_failure(ctx->state, "router_tool_loop", reason) ||
      router_loop_record(task, ctx->state, reason)) return SALTS_ENOMEM;
  turbo_graph_ctx_set_next(ctx, ctx->current_node);
  turbo_graph_ctx_stop(ctx);
  return SALTS_OK;
}

static int router_model_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_router_task_t *task = user_data;
  int rc;
  if (!router_authority_current(task)) return SALTS_EPERM;
  if (task->max_model_turns && task->model_turns >= task->max_model_turns)
    return router_loop_halt(ctx, task, "turn_limit");
  if (task->max_model_turns) ++task->model_turns;
  rc = turbo_agent_model_node(ctx, task->endpoint->config.agent);
  if (rc || !task->max_model_turns || ctx->stop) return rc;
  if (turbo_agent_state_pending_tool_calls(ctx->state)) {
    /* Reserve the last model turn for consuming the previous tool result. */
    if (task->model_turns >= task->max_model_turns)
      return router_loop_halt(ctx, task, "turn_limit");
    if (router_loop_record(task, ctx->state, "tool_proposed")) return SALTS_ENOMEM;
    turbo_graph_ctx_set_next(ctx, "tools");
    return SALTS_OK;
  }
  return router_loop_record(task, ctx->state, "completed");
}

static int router_tools_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  turbo_agent_router_task_t *task = user_data;
  turbo_agent_t *child = task->endpoint->config.agent;
  const json_value_t *calls = turbo_agent_last_model_tool_calls(ctx->state);
  const char *call_id, *name, *arguments, *canonical, *schema_json;
  const json_value_t *schema;
  turbo_tool_effect_flags_t effects = 0;
  int strict = 0, outcome, rc;
  if (!router_authority_current(task)) return SALTS_EPERM;
  if (task->review_cancelled) return router_loop_halt(ctx, task, "review_rejected");
  if (!calls || json_array_size(calls) != 1 ||
      !turbo_agent_tool_call_record_fields(json_array_get(calls, 0),
                                           &call_id, &name, &arguments))
    return router_loop_halt(ctx, task, "single_tool_required");
  if (turbo_tool_registry_resolve_name(child->tool_registry, name, &canonical) != TURBO_TOOL_OK ||
      turbo_agent_policy_check_tool(&child->tool_policy, child->tool_registry, canonical, NULL) !=
          TURBO_AGENT_POLICY_ALLOW)
    return router_loop_halt(ctx, task, "child_tool_denied");
  if (turbo_tool_registry_get_result_contract(child->tool_registry, canonical,
          &schema_json, &schema, &strict) != TURBO_TOOL_OK || !schema || !strict ||
      turbo_tool_registry_get_effects(child->tool_registry, canonical, &effects) != TURBO_TOOL_OK ||
      !effects || (effects & TURBO_TOOL_EFFECT_UNKNOWN))
    return router_loop_halt(ctx, task, "tool_contract_required");
  rc = turbo_agent_tool_node_checked(ctx, task->router->authority_agent, &outcome);
  if (rc) return rc;
  if (ctx->stop && turbo_agent_state_review_required(ctx->state))
    return router_loop_record(task, ctx->state, "approval_pending");
  if (outcome != TURBO_AGENT_TOOL_CHECK_OK)
    return router_loop_halt(ctx, task, outcome == TURBO_AGENT_TOOL_CHECK_INVALID_RESULT
                                      ? "invalid_tool_result" : "tool_failed");
  /* An approval is for this dispatch only, even if the model reuses a call id. */
  if (turbo_agent_state_clear_review(ctx->state) ||
      router_loop_record(task, ctx->state, "tool_completed")) return SALTS_ENOMEM;
  turbo_graph_ctx_set_next(ctx, "model");
  return SALTS_OK;
}

/* Run before Execution publishes terminal status. Retain only bounded derived
 * identity, never a second copy of the Runtime result or checkpoint state. */
static int router_loop_complete(void *user, turbo_graph_t *graph,
    const turbo_graph_run_options_t *options, turbo_cancel_token_t *token,
    json_value_t **summary, json_value_t **state) {
  turbo_agent_router_task_t *task = user;
  const char *status = *summary ? json_get_string(*summary, "status") : NULL;
  const char *checkpoint = *summary ? json_get_string(*summary, "checkpoint_id") : NULL;
  const char *reason = *state ? json_get_string(json_object_get(*state, "router_tool_loop"),
                                                "reason") : NULL;
  tstr copy = NULL;
  int pending = status && strcmp(status, "interrupted") == 0 && reason &&
      strcmp(reason, "approval_pending") == 0 && checkpoint && checkpoint[0] &&
      turbo_agent_state_review_required(*state);
  (void)graph; (void)options; (void)token;
  if (pending && !(copy = tstr_dup(checkpoint))) return SALTS_ENOMEM;
  tstr_free(task->review_checkpoint);
  task->review_checkpoint = copy;
  task->pending_review = pending;
  return SALTS_OK;
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

static int router_submit(turbo_agent_router_t *router, cmeta_threadpool_t *executor,
    const turbo_agent_router_request_t *request, const json_value_t *input,
    unsigned max_model_turns, turbo_agent_router_task_t **out_task) {
  turbo_agent_tool_authority_snapshot_t parent = {0}, child = {0}, selected_child = {0};
  turbo_agent_router_task_t *task;
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  turbo_agent_execution_options_t options = {0};
  turbo_graph_run_options_t graph_options = {0};
  json_value_t *state = NULL, *decision = NULL;
  router_endpoint_t *selected = NULL;
  uint64_t best_cost = UINT64_MAX, now;
  size_t i;
  int busy = 0, rc, guarded = 0;
  turbo_agent_execution_hooks_t hooks = {0};
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
  if (router->tool_lane_busy) return SALTS_EBUSY;
  if (max_model_turns) {
    if (max_model_turns < 2 || max_model_turns > TURBO_AGENT_ROUTER_MAX_MODEL_TURNS ||
        request->role != TURBO_AGENT_ROUTER_EXECUTE ||
        !(request->required_capabilities & TURBO_AGENT_ROUTER_TOOL_PROPOSALS)) return SALTS_EINVAL;
    for (i = 0; i < router->count; ++i)
      if (router->endpoints[i].busy) return SALTS_EBUSY;
    for (i = 0; i < router->authority_agent->guardrail_count; ++i)
      if (router->authority_agent->guardrails[i].before_tool) guarded = 1;
    if (!guarded || !router->authority_agent->tool_registry ||
        !turbo_tool_registry_count(router->authority_agent->tool_registry)) return SALTS_EPERM;
  }
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
    if (max_model_turns && e->agent->tool_registry != router->authority_agent->tool_registry)
      continue;
    cost = router_estimated_cost(e, request) * (max_model_turns ? max_model_turns : 1);
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
  task->max_model_turns = max_model_turns;
  task->deadline_ms = now + request->timeout_ms;
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
  if (max_model_turns) {
    if (turbo_agent_state_clear_review(state) || router_loop_record(task, state, "admitted") ||
        turbo_graph_add_node(task->graph, "tools", router_tools_node, task) != TURBO_GRAPH_EXEC_OK)
      goto rejected;
    hooks.complete = router_loop_complete;
    hooks.user_data = task;
  }
  graph_options.max_steps = max_model_turns ? 2 * max_model_turns +
      TURBO_AGENT_ROUTER_MAX_REVIEW_RESUMES + 2 : 2;
  runtime_options.thread_id = task->thread_id;
  runtime_options.parent_link = request->parent_link;
  options.struct_size = sizeof(options);
  options.abi_version = TURBO_AGENT_EXECUTION_ABI_VERSION;
  options.deadline_mono_ms = task->deadline_ms;
  selected->busy = 1;
  router->tool_lane_busy = max_model_turns != 0;
  rc = turbo_agent_execution_start_internal(executor, selected->config.runtime, task->graph, state,
      &graph_options, &runtime_options, &options, &hooks, &task->execution);
  if (rc != SALTS_OK) {
    selected->busy = 0;
    router->tool_lane_busy = 0;
    goto rejected;
  }
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

int turbo_agent_router_submit(turbo_agent_router_t *router, cmeta_threadpool_t *executor,
    const turbo_agent_router_request_t *request, const json_value_t *input,
    turbo_agent_router_task_t **out_task) {
  return router_submit(router, executor, request, input, 0, out_task);
}

int turbo_agent_router_submit_tool_loop(turbo_agent_router_t *router, cmeta_threadpool_t *executor,
    const turbo_agent_router_request_t *request, const json_value_t *input,
    unsigned max_model_turns, turbo_agent_router_task_t **out_task) {
  if (!max_model_turns) {
    if (out_task) *out_task = NULL;
    return SALTS_EINVAL;
  }
  return router_submit(router, executor, request, input, max_model_turns, out_task);
}

int turbo_agent_router_task_review(turbo_agent_router_task_t *task, cmeta_threadpool_t *executor,
                                   int approved) {
  turbo_agent_runtime_exec_options_t runtime_options = {0};
  turbo_agent_execution_options_t options = {0};
  turbo_graph_run_options_t graph_options = {0};
  turbo_agent_execution_hooks_t hooks = {0};
  turbo_agent_execution_t *next = NULL, *old;
  turbo_agent_execution_status_t status;
  json_value_t *command = NULL, *override = NULL;
  tstr checkpoint = NULL;
  uint64_t now;
  int rc, previous_cancelled;
  if (!task || !executor || !task->max_model_turns || (approved != 0 && approved != 1))
    return SALTS_EINVAL;
  if (turbo_agent_execution_wait(task->execution, 0) != SALTS_OK) return SALTS_EBUSY;
  if (task->unresponsive || task->review_cancelled) return SALTS_ECANCELED;
  if (!router_authority_current(task)) return SALTS_EPERM;
  turbo_agent_execution_get_status(task->execution, &status);
  if (status != TURBO_AGENT_EXECUTION_INTERRUPTED || !task->pending_review ||
      !task->review_checkpoint) return SALTS_EINVAL;
  if (task->review_resumes >= TURBO_AGENT_ROUTER_MAX_REVIEW_RESUMES) return SALTS_ERANGE;
  now = cmeta_monotonic_ms();
  if (now >= task->deadline_ms || task->observed_ms > now) return SALTS_ETIMEDOUT;
  checkpoint = tstr_dup(task->review_checkpoint);
  command = json_create_object();
  rc = SALTS_ENOMEM;
  if (!checkpoint || !command || turbo_runtime_json_object_set_string(command, "kind",
          approved ? "approve_review" : "reject_review")) goto done;
  rc = turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      task->endpoint->config.runtime, checkpoint, command, &override);
  if (rc) goto done;
  runtime_options.scope = TURBO_RUNTIME_SCOPE_CHECKPOINT;
  runtime_options.input_kind = TURBO_RUNTIME_INPUT_OVERRIDE;
  runtime_options.checkpoint_id = checkpoint;
  graph_options.max_steps = 2 * task->max_model_turns + TURBO_AGENT_ROUTER_MAX_REVIEW_RESUMES + 2;
  options.struct_size = sizeof(options);
  options.abi_version = TURBO_AGENT_EXECUTION_ABI_VERSION;
  options.deadline_mono_ms = task->deadline_ms;
  hooks.complete = router_loop_complete;
  hooks.user_data = task;
  previous_cancelled = task->review_cancelled;
  task->review_cancelled = !approved;
  rc = turbo_agent_execution_resume_internal(executor, task->endpoint->config.runtime,
      task->graph, override, &graph_options, &runtime_options, &options, &hooks, &next);
  if (rc) { task->review_cancelled = previous_cancelled; goto done; }
  old = task->execution;
  if (!task->taken) {
    json_value_t *old_summary = NULL, *old_state = NULL;
    turbo_agent_execution_take_result(old, &old_summary, &old_state);
    json_free(old_summary); json_free(old_state);
  }
  task->execution = next;
  /* Keep the first existing Execution UUID stable across reviewed segments.
   * No allocation or fallible action follows successful work admission. */
  if (!task->identity_execution) task->identity_execution = old;
  else turbo_agent_execution_release(old);
  ++task->review_resumes;
  task->taken = 0;
  task->heartbeat_ms = task->observed_ms = now;
done:
  tstr_free(checkpoint);
  json_free(command); json_free(override);
  return rc;
}

const char *turbo_agent_router_task_id(const turbo_agent_router_task_t *task) {
  return task ? turbo_agent_execution_id(task->identity_execution
      ? task->identity_execution : task->execution) : NULL;
}

int turbo_agent_router_task_wait(turbo_agent_router_task_t *task, uint64_t timeout_ms) {
  return task ? turbo_agent_execution_wait(task->execution, timeout_ms) : SALTS_EINVAL;
}

int turbo_agent_router_task_cancel(turbo_agent_router_task_t *task, turbo_cancel_reason_t reason) {
  if (!task) return SALTS_EINVAL;
  if (task->max_model_turns && turbo_agent_execution_wait(task->execution, 0) == SALTS_OK &&
      task->pending_review) {
    task->review_cancelled = 1;
    return SALTS_OK;
  }
  return turbo_agent_execution_cancel(task->execution, reason);
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
      turbo_runtime_json_object_set_string(view, "task_id", turbo_agent_router_task_id(task)) ||
      turbo_runtime_json_object_set_string(view, "execution_id", turbo_agent_execution_id(task->execution)) ||
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
  if (task->max_model_turns) {
    if (turbo_runtime_json_object_set_int64(view, "max_model_turns", task->max_model_turns) ||
        turbo_runtime_json_object_set_int64(view, "review_resumes", task->review_resumes)) goto failed;
    if (turbo_agent_execution_wait(task->execution, 0) == SALTS_OK &&
        router_set(view, "approval_pending", json_create_bool(task->pending_review &&
                                                             !task->review_cancelled))) goto failed;
  }
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
  turbo_agent_execution_release(task->identity_execution);
  turbo_graph_destroy(task->graph);
  task->endpoint->busy = 0;
  if (task->max_model_turns) task->router->tool_lane_busy = 0;
  tstr_free(task->review_checkpoint);
  tstr_free(task->thread_id);
  free(task);
  return SALTS_OK;
}
