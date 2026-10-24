#include "tinytest.h"
#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include "../src/turbo_agent_workflow_graph_internal.h"
#include "../src/turbo_agent_runtime_internal.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

typedef struct loop_fixture_s {
  atomic_int model_calls, tool_calls, entered, open;
  atomic_int before_calls, after_calls, thread_calls[2];
  int chat, proposals, multi, malformed, callback_fail, invalid_result;
  int approval, deny, rewrite_args, rewrite_result, host_hooks, context_ok, approved;
  int saw_result, bad_model, fail_followup;
  turbo_agent_t *host;
  turbo_agent_runtime_store_t store;
  turbo_agent_runtime_t *runtime;
  int fail_commit;
} loop_fixture_t;

static char *loop_copy(const char *text) {
  char *copy = malloc(strlen(text) + 1);
  if (copy) memcpy(copy, text, strlen(text) + 1);
  return copy;
}

static int loop_transport(const char *request, char **out, void *user) {
  loop_fixture_t *f = user;
  int turn = atomic_fetch_add(&f->model_calls, 1);
  json_value_t *body = json_parse(request, strlen(request));
  const char *model = body ? json_get_string(body, "model") : NULL;
  const char *response;
  *out = NULL;
  if (!model || strcmp(model, f->chat ? "fast" : "strong")) f->bad_model = 1;
  json_free(body);
  if (turn > 0 && f->fail_followup) return -77;
  if (turn > 0 && strstr(request, "function_call_output")) f->saw_result = 1;
  if (turn > 0 && f->chat && strstr(request, "\"tool_call_id\"")) f->saw_result = 1;
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  uint64_t end = cmeta_monotonic_ms() + 3000;
  while (!atomic_load_explicit(&f->open, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
  if (turn < f->proposals) {
    if (f->multi) response = "{\"id\":\"r\",\"output\":["
        "{\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"loop_effect\",\"arguments\":\"{\\\"value\\\":7}\"},"
        "{\"type\":\"function_call\",\"call_id\":\"call-2\",\"name\":\"loop_effect\",\"arguments\":\"{\\\"value\\\":7}\"}]}";
    else if (f->malformed) response = "{\"id\":\"r\",\"output\":[{\"type\":\"function_call\","
        "\"call_id\":\"call-1\",\"name\":\"loop_effect\",\"arguments\":\"{\\\"value\\\":\\\"bad\\\"}\"}]}";
    else if (f->chat) response = "{\"id\":\"r\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call-1\",\"type\":\"function\","
        "\"function\":{\"name\":\"loop_effect\",\"arguments\":\"{\\\"value\\\":7}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    else response = "{\"id\":\"r\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call-1\","
        "\"name\":\"loop_effect\",\"arguments\":\"{\\\"value\\\":7}\"}]}";
  } else if (f->chat) response = "{\"id\":\"r\",\"choices\":[{\"message\":{\"role\":\"assistant\","
      "\"content\":\"verified result\"},\"finish_reason\":\"stop\"}]}";
  else response = "{\"id\":\"r\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"verified result\"}]}]}";
  *out = loop_copy(response);
  return *out ? 0 : -1;
}

static turbo_tool_status_t loop_tool(const char *args, const turbo_tool_execution_context_t *ctx,
                                     char **out, void *user) {
  loop_fixture_t *f = user;
  json_value_t *value = json_parse(args, strlen(args));
  atomic_fetch_add(&f->tool_calls, 1);
  f->context_ok = ctx && ctx->cancel_token && ctx->deadline_mono_ms && ctx->run_id &&
      ctx->thread_id && (!strcmp(ctx->thread_id, "loop-thread") ||
                        !strcmp(ctx->thread_id, "loop-other")) && ctx->tool_call_id &&
      strcmp(ctx->tool_call_id, "call-1") == 0 && value &&
      turbo_runtime_json_value_as_int64(json_object_get(value, "value"), 0) ==
          (f->rewrite_args ? 8 : 7);
  json_free(value);
  f->approved = turbo_agent_current_tool_approval_granted();
  if (ctx && ctx->thread_id)
    atomic_fetch_add(&f->thread_calls[!strcmp(ctx->thread_id, "loop-other")], 1);
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  uint64_t end = cmeta_monotonic_ms() + 3000;
  while (!atomic_load_explicit(&f->open, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
  *out = loop_copy(f->invalid_result ? "{\"ok\":\"wrong\"}" : "{\"ok\":true}");
  return f->callback_fail ? TURBO_TOOL_ERROR : TURBO_TOOL_OK;
}

static int loop_guard(turbo_agent_t *agent, const json_value_t *state, const char *id,
    const char *name, const char *args, const char **reason, void *user) {
  loop_fixture_t *f = user;
  if (agent == f->host && strcmp(name, "loop.effect") == 0) ++f->host_hooks;
  if (f->deny) { *reason = "host_denied"; return -1; }
  if (f->approval && !turbo_agent_tool_approval_review_matches(state, id, name, args)) {
    *reason = "approval_required";
    return -1;
  }
  return 0;
}

static int loop_before_tool(turbo_agent_t *agent, json_value_t *state, const char *id,
    const char *name, char **args, void *user) {
  loop_fixture_t *f = user;
  (void)agent; (void)state; (void)id; (void)name;
  atomic_fetch_add(&f->before_calls, 1);
  if (f->rewrite_args) {
    tstr replacement = tstr_dup("{\"value\":8}");
    if (!replacement) return -1;
    tstr_free(*args); *args = replacement;
  }
  return 0;
}

static int loop_after_tool(turbo_agent_t *agent, json_value_t *state, const char *id,
    const char *name, const char *args, char **out, turbo_tool_status_t status, void *user) {
  loop_fixture_t *f = user;
  (void)agent; (void)state; (void)id; (void)name; (void)args; (void)status;
  atomic_fetch_add(&f->after_calls, 1);
  if (f->rewrite_result) {
    char *replacement = loop_copy("not json");
    if (!replacement) return -1;
    free(*out); *out = replacement;
  }
  return 0;
}

static int loop_store_put(void *user, const char *collection, const char *id, const char *text) {
  loop_fixture_t *f = user;
  json_value_t *record = json_parse(text, strlen(text));
  const char *phase = record ? json_get_string(record, "phase") : NULL;
  int fail = f->fail_commit && !strcmp(collection, "agent_tool_journal") && phase &&
      !strcmp(phase, "committed");
  json_free(record);
  return fail ? -1 : f->store.put(f->store.user_data, collection, id, text);
}
static int loop_store_get(void *user, const char *collection, const char *id, char **out) {
  loop_fixture_t *f = user;
  return f->store.get(f->store.user_data, collection, id, out);
}
static int loop_store_list(void *user, const char *collection, const char *key,
                           const char *value, char **out) {
  loop_fixture_t *f = user;
  return f->store.list(f->store.user_data, collection, key, value, out);
}
static void loop_store_free(void *user) {
  loop_fixture_t *f = user;
  f->store.user_data_free(f->store.user_data);
}

static void loop_block(void *user) {
  loop_fixture_t *f = user;
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  uint64_t end = cmeta_monotonic_ms() + 3000;
  while (!atomic_load_explicit(&f->open, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
}
static void loop_entered(loop_fixture_t *f) {
  uint64_t end = cmeta_monotonic_ms() + 2000;
  while (!atomic_load_explicit(&f->entered, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
  check_true(atomic_load_explicit(&f->entered, memory_order_acquire));
}
static json_value_t *loop_journal(loop_fixture_t *f) {
  json_value_t *records = NULL;
  check_equal(turbo_agent_runtime_store_list_json(f->runtime, "agent_tool_journal",
                                                  NULL, NULL, &records), 0);
  check_not_null(records);
  return records;
}

static void loop_wait_host_lane(turbo_agent_router_task_t *task) {
  uint64_t end = cmeta_monotonic_ms() + 2000;
  int waiting = 0;
  do {
    json_value_t *view = NULL;
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    waiting = json_get_bool(view, "host_tool_waiting", 0);
    json_free(view);
    if (!waiting) cmeta_thread_yield();
  } while (!waiting && cmeta_monotonic_ms() < end);
  check_true(waiting);
}

static cmeta_threadpool_t *loop_parallel_pool(cmeta_threadpool_t *old) {
  cmeta_threadpool_config_t config = {.num_threads = 2, .queue_capacity = 2};
  cmeta_threadpool_destroy(old);
  cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&config);
  check_not_null(pool);
  return pool;
}

spec("supervised tiered Router tool loop") {
  static loop_fixture_t f[3];
  static turbo_agent_t *agents[3];
  static turbo_agent_runtime_t *runtimes[2];
  static turbo_agent_router_endpoint_t endpoints[2];
  static turbo_agent_router_config_t config;
  static turbo_agent_router_request_t request;
  static turbo_agent_runtime_parent_link_t parent;
  static turbo_agent_policy_t grant;
  static turbo_tool_registry_t *tools;
  static turbo_agent_router_t *router;
  static turbo_agent_router_task_t *task, *other;
  static cmeta_threadpool_t *pool;
  static json_value_t *input, *summary, *state, *view;
  static turbo_agent_runtime_t *audit_runtime;

  before_each() {
    static const char *capabilities[] = {"custom_tools"};
    turbo_tool_definition_v6_t tool = {0};
    turbo_agent_guardrail_t guard = {0};
    turbo_agent_middleware_t middleware = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    for (int i = 0; i < 3; ++i) {
      memset(&f[i], 0, sizeof(f[i]));
      atomic_init(&f[i].model_calls, 0); atomic_init(&f[i].tool_calls, 0);
      atomic_init(&f[i].entered, 0); atomic_init(&f[i].open, 1);
      atomic_init(&f[i].before_calls, 0); atomic_init(&f[i].after_calls, 0);
      atomic_init(&f[i].thread_calls[0], 0); atomic_init(&f[i].thread_calls[1], 0);
      f[i].chat = i == 2;
      f[i].proposals = 1;
    }
    tools = turbo_tool_registry_create();
    check_not_null(tools);
    tool.struct_size = sizeof(tool); tool.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
    tool.definition.name = "loop.effect"; tool.definition.description = "Offline bounded effect";
    tool.definition.parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},\"required\":[\"value\"]}";
    tool.definition.strict = 1; tool.definition.user_data = &f[0];
    tool.context_handler = loop_tool; tool.required_capabilities = capabilities;
    tool.required_capability_count = 1;
    tool.result_schema_json = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"]}";
    tool.strict_result = 1; tool.effect_flags = TURBO_TOOL_EFFECT_WRITE;
    check_equal(turbo_tool_registry_add_v6(tools, &tool), TURBO_TOOL_OK);
    grant = turbo_agent_policy_default(); grant.allow_custom_tools = 1; grant.allow_delegate = 1;
    for (int i = 0; i < 3; ++i) {
      turbo_agent_config_t agent_config = {0};
      agent_config.model = i == 2 ? "fast" : "strong";
      agent_config.provider = i == 2 ? turbo_model_provider_openai_compatible_chat_completions()
                                    : turbo_model_provider_openai_responses();
      agent_config.tool_registry = tools;
      agent_config.transport_fn = loop_transport; agent_config.transport_user_data = &f[i];
      agents[i] = turbo_agent_create(&agent_config);
      check_not_null(agents[i]);
      check_equal(turbo_agent_set_tool_policy(agents[i], &grant), 0);
    }
    f[0].host = agents[0];
    guard.before_tool = loop_guard; guard.user_data = &f[0];
    check_equal(turbo_agent_add_guardrail(agents[0], &guard), 0);
    middleware.before_tool = loop_before_tool; middleware.after_tool = loop_after_tool;
    middleware.user_data = &f[0];
    check_equal(turbo_agent_add_middleware(agents[0], &middleware), 0);
    for (int i = 0; i < 2; ++i) {
      turbo_agent_runtime_store_t store = {0};
      f[i+1].store = turbo_agent_runtime_store_memory_create();
      store.put = loop_store_put; store.get = loop_store_get; store.list = loop_store_list;
      store.user_data_free = loop_store_free; store.user_data = &f[i+1];
      runtimes[i] = turbo_agent_runtime_create(&store);
      check_not_null(runtimes[i]);
      f[i+1].runtime = runtimes[i];
      endpoints[i] = (turbo_agent_router_endpoint_t){0};
      endpoints[i].id = i == 0 ? "strong" : "fast";
      endpoints[i].agent = agents[i+1]; endpoints[i].runtime = runtimes[i];
      endpoints[i].roles = 3; endpoints[i].capabilities = 7; endpoints[i].max_difficulty = 100;
      endpoints[i].max_input_tokens = endpoints[i].max_output_tokens = 10000;
      endpoints[i].input_price_micros = endpoints[i].output_price_micros = i == 0 ? 10000 : 1000;
    }
    turbo_agent_router_config_init(&config); config.authority_agent = agents[0];
    config.endpoints = endpoints; config.endpoint_count = 2;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    turbo_agent_router_request_init(&request); request.role = TURBO_AGENT_ROUTER_EXECUTE;
    request.required_capabilities = TURBO_AGENT_ROUTER_TEXT | TURBO_AGENT_ROUTER_TOOL_PROPOSALS;
    request.input_tokens = request.output_tokens = 1000; request.budget_micros = 100;
    request.timeout_ms = 10000; request.heartbeat_timeout_ms = 5000;
    request.thread_id = "loop-thread";
    parent = (turbo_agent_runtime_parent_link_t){0}; parent.parent_agent_run_id = "host-run";
    request.parent_link = &parent;
    pool = cmeta_threadpool_create_with_config(&pool_config); check_not_null(pool);
    input = turbo_agent_session_create_input_state_json_value("Execute one approved Host operation");
    check_not_null(input);
    task = other = NULL; summary = state = view = NULL;
    audit_runtime = NULL;
  }
  after_each() {
    for (int i = 0; i < 3; ++i) atomic_store_explicit(&f[i].open, 1, memory_order_release);
    if (other) {
      turbo_agent_router_task_cancel(other, TURBO_CANCEL_SHUTDOWN);
      check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_release(other), SALTS_OK);
    }
    if (task) {
      turbo_agent_router_task_cancel(task, TURBO_CANCEL_SHUTDOWN);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_release(task), SALTS_OK);
    }
    cmeta_threadpool_destroy(pool);
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    for (int i = 0; i < 2; ++i) turbo_agent_runtime_destroy(runtimes[i]);
    for (int i = 0; i < 3; ++i) turbo_agent_destroy(agents[i]);
    turbo_tool_registry_destroy(tools);
    json_free(input); json_free(summary); json_free(state); json_free(view);
    turbo_agent_runtime_destroy(audit_runtime);
  }

  for (int provider = 0; provider < 2; ++provider) {
    it("executes through Host authority and validates canonical results, provider=%d", provider) {
      if (!provider) {
        check_equal(turbo_agent_router_destroy(router), SALTS_OK);
        endpoints[1].roles = TURBO_AGENT_ROUTER_PLAN;
        check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
      }
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
      check_equal(json_get_string(summary, "status"), "completed");
      check_equal(json_get_string(summary, "parent_agent_run_id"), "host-run");
      check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "completed");
      check_equal(atomic_load(&f[provider ? 2 : 1].model_calls), 2);
      check_equal(atomic_load(&f[0].tool_calls), 1);
      check_equal(f[0].host_hooks, 1); check_true(f[0].context_ok);
      check_true(f[provider ? 2 : 1].saw_result); check_false(f[provider ? 2 : 1].bad_model);
      json_value_t *records = loop_journal(&f[provider ? 2 : 1]);
      check_equal(json_array_size(records), 1u);
      check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
      json_free(records);
      check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
      check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "estimated_cost_micros"), 0), provider ? 4 : 40);
    }
  }

  for (int invalid = 0; invalid < 5; ++invalid) {
    it("rejects invalid loop admission before model or effect, case=%d", invalid) {
      unsigned turns = 2;
      int expected = SALTS_EINVAL;
      if (invalid == 0) turns = 0;
      if (invalid == 1) turns = 17;
      if (invalid == 2) request.role = TURBO_AGENT_ROUTER_PLAN;
      if (invalid == 3) request.required_capabilities = TURBO_AGENT_ROUTER_TEXT;
      if (invalid == 4) { request.budget_micros = 3; expected = SALTS_ENOENT; }
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, turns, &task), expected);
      check_null(task); check_equal(atomic_load(&f[0].tool_calls), 0);
      check_equal(atomic_load(&f[1].model_calls) + atomic_load(&f[2].model_calls), 0);
    }
  }

  it("pauses and resumes exactly the canonical approval after caller takes and mutates its copy") {
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    tstr id = tstr_dup(turbo_agent_router_task_id(task)); check_not_null(id);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "interrupted");
    tstr run = tstr_dup(json_get_string(summary, "run_id")); check_not_null(run);
    check_equal(atomic_load(&f[0].tool_calls), 0);
    check_equal(turbo_agent_state_request_review(state, "caller tampered note"), 0);
    check_equal(turbo_agent_state_set_review_approved(state, 1), 0);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    json_free(summary); json_free(state); summary = state = NULL;
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(json_get_string(summary, "run_id"), run);
    check_equal(turbo_agent_router_task_id(task), id);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    check_true(strcmp(json_get_string(view, "execution_id"), id) != 0);
    check_equal(atomic_load(&f[0].tool_calls), 1); check_true(f[0].approved);
    check_false(turbo_agent_state_review_required(state));
    json_value_t *s2 = NULL, *v2 = NULL;
    check_equal(turbo_agent_router_task_take_result(task, &s2, &v2), SALTS_EALREADY);
    check_null(s2); check_null(v2);
    tstr_free(id); tstr_free(run);
  }

  it("persists approval and resumed segment identities without making a stored observation an execution grant") {
    turbo_agent_runtime_store_t audit_store = turbo_agent_runtime_store_memory_create();
    audit_runtime = turbo_agent_runtime_create(&audit_store); check_not_null(audit_runtime);
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_record_observation(task, audit_runtime, "approval"), SALTS_OK);
    check_equal(turbo_agent_router_load_observation(audit_runtime, "approval", &view), SALTS_OK);
    check_true(json_get_bool(view, "approval_pending", 0));
    check_equal(json_get_string(view, "runtime_status"), "interrupted");
    check_equal(json_get_string(view, "task_id"), json_get_string(view, "execution_id"));
    tstr run = tstr_dup(json_get_string(view, "runtime_run_id")); check_not_null(run);
    check_equal(turbo_runtime_json_object_set(view, "approval_pending", json_create_bool(0)), 0);
    check_equal(atomic_load(&f[0].tool_calls), 0); check_equal(atomic_load(&f[2].model_calls), 1);
    json_free(view); view = NULL;
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_record_observation(task, audit_runtime, "resumed"), SALTS_OK);
    check_equal(turbo_agent_router_load_observation(audit_runtime, "resumed", &view), SALTS_OK);
    check_false(json_get_bool(view, "approval_pending", 1));
    check_equal(json_get_string(view, "task_id"), turbo_agent_router_task_id(task));
    check_true(strcmp(json_get_string(view, "task_id"), json_get_string(view, "execution_id")) != 0);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "review_resumes"), -1), 1);
    check_equal(json_get_string(view, "runtime_run_id"), run);
    check_equal(json_get_string(view, "runtime_status"), "completed");
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_type(json_object_get(view, "runtime_checkpoint_id")), json_type(json_object_get(summary, "checkpoint_id")));
    if (json_get_string(summary, "checkpoint_id"))
      check_equal(json_get_string(view, "runtime_checkpoint_id"), json_get_string(summary, "checkpoint_id"));
    else check_true(json_is_null(json_object_get(view, "runtime_checkpoint_id")));
    check_equal(atomic_load(&f[0].tool_calls), 1); check_equal(atomic_load(&f[2].model_calls), 2);
    tstr_free(run);
  }

  it("does not report an old approval after a resumed segment fails before terminal metadata capture") {
    f[0].approval = 1; f[2].fail_followup = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "execution_status"), -1),
                TURBO_AGENT_EXECUTION_FAILED);
    check_false(json_get_bool(view, "approval_pending", 1));
    check_equal(turbo_agent_router_task_cancel(task, TURBO_CANCEL_USER), SALTS_EALREADY);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_EINVAL);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_null(summary); check_not_null(turbo_agent_state_model_error_detail(state));
    check_equal(atomic_load(&f[0].tool_calls), 1);
    check_equal(atomic_load(&f[2].model_calls), 2);
  }

  it("rejects a review without effects or another model turn") {
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_review(task, pool, 0), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "review_rejected");
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_ECANCELED);
    check_equal(atomic_load(&f[0].tool_calls), 0); check_equal(atomic_load(&f[2].model_calls), 1);
  }

  it("requires fresh approval when a later model turn reuses the same call id and arguments") {
    f[0].approval = 1; f[2].proposals = 2;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 3, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "approval_pending");
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 2); check_equal(atomic_load(&f[2].model_calls), 3);
  }

  it("binds approval to post-middleware arguments and asks again after a quiescent change") {
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    f[0].rewrite_args = 1;
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 0);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1); check_true(f[0].context_ok);
  }

  for (int child = 0; child < 2; ++child) {
    it("denies review after quiescent same-grant generation replacement, child=%d", child) {
      f[0].approval = 1;
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_set_tool_policy(agents[child ? 2 : 0], &grant), 0);
      check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_EPERM);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_EPERM);
      check_null(summary); check_null(state); check_equal(atomic_load(&f[0].tool_calls), 0);
    }
  }

  for (int child = 0; child < 2; ++child) {
    it("withholds completed tool-loop delivery after quiescent generation invalidation, child=%d", child) {
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(atomic_load(&f[0].tool_calls), 1);
      check_equal(turbo_agent_set_tool_policy(agents[child ? 2 : 0], &grant), 0);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_EPERM);
      check_null(summary); check_null(state);
      check_equal(atomic_load(&f[0].tool_calls), 1);
    }
  }

  for (int failure = 0; failure < 5; ++failure) {
    it("halts before another model call on actual Host failure or invalid output, case=%d", failure) {
      if (failure == 0) f[0].deny = 1;
      if (failure == 1) f[0].callback_fail = 1;
      if (failure == 2) f[0].invalid_result = 1;
      if (failure == 3) f[0].rewrite_result = 1;
      if (failure == 4) f[2].fail_commit = 1;
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
      check_equal(json_get_string(summary, "status"), "interrupted");
      check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"),
                  failure == 2 || failure == 3 ? "invalid_tool_result" : "tool_failed");
      check_equal(atomic_load(&f[2].model_calls), 1);
      check_equal(atomic_load(&f[0].tool_calls), failure == 0 ? 0 : 1);
      check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_EINVAL);
      if (failure >= 2) {
        json_value_t *records = loop_journal(&f[2]);
        check_equal(json_array_size(records), 1u);
        check_equal(json_get_string(json_array_get(records, 0), "phase"), failure == 4 ? "started" : "committed");
        json_free(records);
      }
    }
  }

  it("checks child tool capability even when Host is authorized") {
    turbo_agent_policy_t child = turbo_agent_policy_default();
    check_equal(turbo_agent_set_tool_policy(agents[2], &child), 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "child_tool_denied");
    check_equal(atomic_load(&f[0].tool_calls), 0); check_equal(f[0].host_hooks, 0);
  }

  it("rejects a multiple-call batch before its first effect") {
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    endpoints[1].roles = TURBO_AGENT_ROUTER_PLAN;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    f[1].multi = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "single_tool_required");
    check_equal(atomic_load(&f[0].tool_calls), 0);
  }

  it("validates arguments before Journal or callback dispatch") {
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    endpoints[1].roles = TURBO_AGENT_ROUTER_PLAN;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    f[1].malformed = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 0);
    check_equal(atomic_load(&f[1].model_calls), 1);
    json_value_t *records = loop_journal(&f[1]); check_equal(json_array_size(records), 0u); json_free(records);
  }

  it("stops bounded repeated proposals before an extra effect") {
    f[2].proposals = 99;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "turn_limit");
    check_equal(atomic_load(&f[2].model_calls), 2); check_equal(atomic_load(&f[0].tool_calls), 1);
  }

  it("excludes other Router tasks through approval and terminal retention") {
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    turbo_agent_router_task_t *other = NULL;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &other), SALTS_EBUSY);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_EBUSY);
    check_null(other);
    check_equal(turbo_agent_router_task_cancel(task, TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_ECANCELED);
    check_equal(turbo_agent_router_task_release(task), SALTS_OK); task = NULL;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
  }

  it("configures bounded tool-task progress only before retaining any task") {
    check_equal(turbo_agent_router_set_tool_task_limit(NULL, 2), SALTS_EINVAL);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 0), SALTS_EINVAL);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 17), SALTS_EINVAL);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 1), SALTS_EBUSY);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_EBUSY);
    check_equal(turbo_agent_router_task_release(task), SALTS_OK); task = NULL;
    check_equal(turbo_agent_router_set_tool_task_limit(router, 1), SALTS_OK);
  }

  it("runs both model owners concurrently before synchronized Host dispatch") {
    pool = loop_parallel_pool(pool);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
    atomic_store(&f[1].open, 0); atomic_store(&f[2].open, 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    request.thread_id = "loop-other";
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
    loop_entered(&f[1]); loop_entered(&f[2]);
    check_equal(atomic_load(&f[0].tool_calls), 0);
    atomic_store(&f[1].open, 1); atomic_store(&f[2].open, 1);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 2);
    check_equal(atomic_load(&f[0].thread_calls[0]), 1);
    check_equal(atomic_load(&f[0].thread_calls[1]), 1);
    check_true(f[0].context_ok);
    for (int i = 1; i < 3; ++i) {
      json_value_t *records = loop_journal(&f[i]);
      check_equal(json_array_size(records), 1u);
      check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
      check_equal(json_get_string(json_array_get(records, 0), "thread_id"),
                  i == 1 ? "loop-other" : "loop-thread");
      json_free(records);
    }
  }

  it("keeps identical call ids and arguments in independent canonical approvals") {
    pool = loop_parallel_pool(pool);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    request.thread_id = "loop-other";
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 0);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(turbo_runtime_json_object_set(state, "review_approved", json_create_bool(1)), 0);
    /* Approve the second owner first. Mutating the first result is no grant. */
    check_equal(turbo_agent_router_task_review(other, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].thread_calls[0]), 0);
    check_equal(atomic_load(&f[0].thread_calls[1]), 1);
    check_true(f[0].approved);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    check_true(json_get_bool(view, "approval_pending", 0));
    check_equal(turbo_agent_router_task_review(task, pool, 0), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1);
    json_value_t *records = loop_journal(&f[2]);
    check_equal(json_array_size(records), 0u); json_free(records);
    check_equal(turbo_agent_router_task_release(other), SALTS_OK); other = NULL;
    /* A retained rejected owner does not prevent using the released endpoint. */
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &other), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1);
  }

  for (int stopped = 0; stopped < 3; ++stopped) {
    it("stops a queued Host dispatch before hooks and Journal admission, cause=%d", stopped) {
      pool = loop_parallel_pool(pool);
      check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
      atomic_store(&f[0].open, 0);
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      loop_entered(&f[0]);
      request.thread_id = "loop-other";
      if (stopped == 1) request.timeout_ms = request.heartbeat_timeout_ms = 300;
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
      loop_wait_host_lane(other);
      check_equal(atomic_load(&f[0].before_calls), 1);
      check_equal(atomic_load(&f[0].after_calls), 0);
      if (stopped == 0)
        check_equal(turbo_agent_router_task_cancel(other, TURBO_CANCEL_USER), SALTS_OK);
      if (stopped == 2)
        check_equal(turbo_agent_router_task_poll(other, cmeta_monotonic_ms() + 5000), SALTS_ECANCELED);
      check_equal(turbo_agent_router_task_wait(other, 1500), SALTS_OK);
      check_equal(turbo_agent_router_task_inspect(other, &view), SALTS_OK);
      check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "execution_status"), -1),
                  stopped == 1 ? TURBO_AGENT_EXECUTION_TIMED_OUT : TURBO_AGENT_EXECUTION_CANCELLED);
      check_false(json_get_bool(view, "host_tool_waiting", 1));
      check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "runtime_result_code"), -1), SALTS_OK);
      json_value_t *stopped_summary = NULL, *stopped_state = NULL;
      check_equal(turbo_agent_router_task_take_result(other, &stopped_summary, &stopped_state),
                  stopped == 2 ? SALTS_ECANCELED : SALTS_OK);
      if (stopped != 2) {
        check_equal(json_get_string(stopped_summary, "status"), stopped == 1 ? "timed_out" : "cancelled");
        check_equal(json_get_string(json_object_get(stopped_state, "router_tool_loop"), "reason"),
                    stopped == 1 ? "host_tool_deadline" : "host_tool_cancelled");
      }
      json_free(stopped_summary); json_free(stopped_state);
      check_equal(atomic_load(&f[0].tool_calls), 1);
      check_equal(atomic_load(&f[0].before_calls), 1);
      json_value_t *records = loop_journal(&f[1]);
      check_equal(json_array_size(records), 0u); json_free(records);
      check_equal(turbo_agent_router_task_release(other), SALTS_OK); other = NULL;
      /* Even a blocked callback does not prevent an independent model task. */
      request.timeout_ms = 10000; request.heartbeat_timeout_ms = 5000;
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &other), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(other, 1000), SALTS_OK);
      check_equal(atomic_load(&f[0].after_calls), 0);
      atomic_store(&f[0].open, 1);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(atomic_load(&f[0].after_calls), 1);
    }
  }

  it("holds Host hooks and output processing in the same dispatch lane as the callback") {
    pool = loop_parallel_pool(pool);
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
    atomic_store(&f[0].open, 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    loop_entered(&f[0]);
    request.thread_id = "loop-other";
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
    loop_wait_host_lane(other);
    check_equal(atomic_load(&f[0].before_calls), 1);
    check_equal(atomic_load(&f[0].tool_calls), 1);
    check_equal(turbo_agent_router_task_release(task), SALTS_EBUSY);
    check_equal(turbo_agent_router_destroy(router), SALTS_EBUSY);
    atomic_store(&f[0].open, 1);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].before_calls), 2);
    check_equal(atomic_load(&f[0].after_calls), 2);
    check_equal(atomic_load(&f[0].tool_calls), 2);
  }

  for (int invalidated = 0; invalidated < 2; ++invalidated) {
    it("rechecks each authority after independent approval pauses, parent=%d", invalidated) {
      pool = loop_parallel_pool(pool);
      check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
      f[0].approval = 1;
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      request.thread_id = "loop-other";
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
      /* Both segments are terminal: policy replacement is actually quiescent. */
      check_equal(turbo_agent_set_tool_policy(agents[invalidated ? 0 : 2], &grant), 0);
      check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_EPERM);
      check_equal(turbo_agent_router_task_review(other, pool, 1), invalidated ? SALTS_EPERM : SALTS_OK);
      check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
      check_equal(atomic_load(&f[0].thread_calls[0]), 0);
      check_equal(atomic_load(&f[0].thread_calls[1]), invalidated ? 0 : 1);
    }
  }

  it("rolls back only rejected endpoint admission without leaking the tool-task limit") {
    check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
    atomic_store(&f[0].open, 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    loop_entered(&f[0]);
    check_equal(cmeta_threadpool_submit(pool, loop_block, &f[1]), SALTS_OK);
    request.thread_id = "loop-other";
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_ENOBUFS);
    check_null(other); check_equal(atomic_load(&f[1].model_calls), 0);
    atomic_store(&f[0].open, 1);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    loop_entered(&f[1]);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 2);
  }

  for (int failure = 0; failure < 3; ++failure) {
    it("releases dispatch ownership after a Host failure, case=%d", failure) {
      pool = loop_parallel_pool(pool);
      check_equal(turbo_agent_router_set_tool_task_limit(router, 2), SALTS_OK);
      if (failure == 0) f[0].deny = 1;
      if (failure == 1) f[0].callback_fail = 1;
      if (failure == 2) f[0].rewrite_result = 1;
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      request.thread_id = "loop-other";
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &other), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(other, 5000), SALTS_OK);
      check_equal(atomic_load(&f[0].before_calls), 2);
      check_equal(atomic_load(&f[0].tool_calls), failure ? 2 : 0);
      check_equal(turbo_agent_router_task_take_result(other, &summary, &state), SALTS_OK);
      check_equal(json_get_string(summary, "status"), "interrupted");
      check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"),
                  failure == 2 ? "invalid_tool_result" : "tool_failed");
    }
  }

  for (int contract = 0; contract < 2; ++contract) {
    it("rejects undeclared results or unknown effects before callback, case=%d", contract) {
      turbo_tool_definition_v6_t tool = {0};
      tool.struct_size = sizeof(tool); tool.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
      tool.definition.name = "loop.effect"; tool.definition.description = "Unqualified effect";
      tool.definition.parameters_json = "{\"type\":\"object\"}";
      tool.definition.user_data = &f[0]; tool.context_handler = loop_tool;
      tool.result_schema_json = contract ? "{\"type\":\"object\"}" : NULL;
      tool.strict_result = contract;
      tool.effect_flags = contract ? TURBO_TOOL_EFFECT_UNKNOWN : TURBO_TOOL_EFFECT_WRITE;
      /* Registry mutation is quiescent, before any Router admission. */
      check_equal(turbo_tool_registry_remove(tools, "loop.effect"), TURBO_TOOL_OK);
      check_equal(turbo_tool_registry_add_v6(tools, &tool), TURBO_TOOL_OK);
      check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
      check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "tool_contract_required");
      check_equal(atomic_load(&f[0].tool_calls), 0);
    }
  }

  it("requires a Host before-tool guardrail and the identical canonical registry") {
    turbo_agent_config_t other_config = {0};
    turbo_agent_t *other;
    turbo_agent_router_task_t *rejected = NULL;
    other_config.model = "unconfigured";
    other_config.transport_fn = loop_transport;
    other_config.transport_user_data = &f[0];
    other_config.tool_registry = tools;
    other = turbo_agent_create(&other_config); check_not_null(other);
    check_equal(turbo_agent_set_tool_policy(other, &grant), 0);
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    config.authority_agent = other;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &rejected), SALTS_EPERM);
    check_null(rejected);
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    config.authority_agent = agents[0];
    turbo_agent_destroy(other);
    other_config.tool_registry = NULL;
    other = turbo_agent_create(&other_config); check_not_null(other);
    check_equal(turbo_agent_set_tool_policy(other, &grant), 0);
    endpoints[1].agent = other;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    request.budget_micros = 4;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &rejected), SALTS_ENOENT);
    check_null(rejected);
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    endpoints[1].agent = agents[2];
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    turbo_agent_destroy(other);
  }

  it("validates Journal replay results and rechecks Host permission before replay delivery") {
    turbo_agent_execution_context_t saved = {0}, context = {0};
    turbo_graph_exec_ctx_t ctx = {0};
    turbo_graph_t *graph;
    json_value_t *command = json_create_object(), *approved = NULL, *replay;
    int outcome = 0;
    f[0].approval = 1; f[0].invalid_result = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    /* Release Router ownership before explicitly testing the underlying checked
     * ToolNode against the same canonical checkpoint and Journal identity. */
    check_equal(turbo_agent_router_task_release(task), SALTS_OK); task = NULL;
    check_equal(turbo_runtime_json_object_set_string(command, "kind", "approve_review"), 0);
    check_equal(turbo_agent_runtime_prepare_checkpoint_command_override_json_value(runtimes[1],
        json_get_string(summary, "checkpoint_id"), command, &approved), 0);
    replay = json_clone(approved); check_not_null(replay);
    graph = turbo_graph_create("checked-replay"); check_not_null(graph);
    check_equal(turbo_graph_add_node(graph, "tools", turbo_agent_tool_node, agents[0]), TURBO_GRAPH_EXEC_OK);
    turbo_agent_execution_context_get(&saved);
    context.thread_id = "loop-thread"; context.run_id = json_get_string(summary, "run_id");
    context.runtime = runtimes[1];
    turbo_agent_execution_context_set(&context);
    ctx.graph = graph; ctx.current_node = "tools"; ctx.state = approved;
    check_equal(turbo_agent_tool_node_checked(&ctx, agents[0], &outcome), 0);
    check_equal(outcome, TURBO_AGENT_TOOL_CHECK_INVALID_RESULT);
    ctx.state = replay;
    check_equal(turbo_agent_tool_node_checked(&ctx, agents[0], &outcome), 0);
    check_equal(outcome, TURBO_AGENT_TOOL_CHECK_INVALID_RESULT);
    check_equal(atomic_load(&f[0].tool_calls), 1);
    /* Policy replacement occurs after both synchronous callbacks returned.
     * A cached committed result never bypasses current Host permissions. */
    turbo_agent_policy_t denied = turbo_agent_policy_default();
    check_equal(turbo_agent_set_tool_policy(agents[0], &denied), 0);
    json_free(replay); replay = json_clone(state); check_not_null(replay);
    ctx.state = replay;
    check_equal(turbo_agent_tool_node_checked(&ctx, agents[0], &outcome), 0);
    check_equal(outcome, TURBO_AGENT_TOOL_CHECK_FAILED);
    check_equal(atomic_load(&f[0].tool_calls), 1);
    check_equal(turbo_agent_state_guardrail_rejection_phase(replay), "tool_policy");
    turbo_agent_execution_context_set(&saved);
    turbo_graph_destroy(graph); json_free(command); json_free(approved); json_free(replay);
  }

  it("clears untrusted review flags on initial input") {
    f[0].approval = 1;
    check_equal(turbo_agent_state_request_review(input,
        "{\"kind\":\"tool_approval\",\"call_id\":\"call-1\",\"tool_name\":\"loop.effect\",\"arguments\":\"{\\\"value\\\":7}\"}"), 0);
    check_equal(turbo_agent_state_set_review_approved(input, 1), 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(json_object_get(state, "router_tool_loop"), "reason"), "approval_pending");
    check_equal(atomic_load(&f[0].tool_calls), 0);
  }

  it("rolls back initial queue rejection and releases the shared tool lane") {
    atomic_store(&f[0].open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, loop_block, &f[0]), SALTS_OK); loop_entered(&f[0]);
    check_equal(cmeta_threadpool_try_submit(pool, loop_block, &f[0]), SALTS_OK);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_ENOBUFS);
    check_null(task);
    atomic_store_explicit(&f[0].open, 1, memory_order_release); cmeta_threadpool_wait(pool);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1);
  }

  it("rolls back review queue rejection and preserves the previous result and checkpoint") {
    f[0].approval = 1;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    atomic_store(&f[0].entered, 0); atomic_store(&f[0].open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, loop_block, &f[0]), SALTS_OK); loop_entered(&f[0]);
    check_equal(cmeta_threadpool_try_submit(pool, loop_block, &f[0]), SALTS_OK);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_ENOBUFS);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "interrupted");
    check_equal(atomic_load(&f[0].tool_calls), 0);
    atomic_store_explicit(&f[0].open, 1, memory_order_release); cmeta_threadpool_wait(pool);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&f[0].tool_calls), 1);
  }

  it("rejects late approval at the original deadline") {
    f[0].approval = 1; request.timeout_ms = request.heartbeat_timeout_ms = 200;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    uint64_t end = cmeta_monotonic_ms() + 250;
    while (cmeta_monotonic_ms() < end) cmeta_thread_yield();
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_ETIMEDOUT);
    check_equal(atomic_load(&f[0].tool_calls), 0);
  }

  it("never revives an unresponsive segment or dispatches its pending effect") {
    atomic_store(&f[2].open, 0);
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &task), SALTS_OK);
    loop_entered(&f[2]);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    uint64_t heartbeat = (uint64_t)turbo_runtime_json_value_as_int64(json_object_get(view, "heartbeat_mono_ms"), 0);
    check_equal(turbo_agent_router_task_poll(task, heartbeat + request.heartbeat_timeout_ms), SALTS_ECANCELED);
    atomic_store_explicit(&f[2].open, 1, memory_order_release);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_ECANCELED);
    check_equal(turbo_agent_router_task_review(task, pool, 1), SALTS_ECANCELED);
    check_equal(atomic_load(&f[0].tool_calls), 0);
  }
}
