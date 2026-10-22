#include "tinytest.h"
#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

typedef struct group_transport_s {
  atomic_int calls, entered, open;
  const char *model;
  int chat, fail, proposal, saw_workers;
  size_t text_bytes;
} group_transport_t;

static int group_tool_calls;
static int group_effect(const json_value_t *args, json_value_t **out, void *user) {
  (void)args; (void)user;
  ++group_tool_calls;
  *out = json_create_bool(1);
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int group_transport(const char *request, char **out, void *user) {
  group_transport_t *f = user;
  json_value_t *body = json_parse(request, strlen(request));
  const char *model = body ? json_get_string(body, "model") : NULL;
  const char *response;
  *out = NULL;
  if (!model || strcmp(model, f->model)) { json_free(body); return -1; }
  if (strstr(request, "worker_results") && strstr(request, "worker-a") &&
      strstr(request, "worker-b") && strstr(request, "host-run") &&
      strstr(request, "run_id") && strstr(request, "checkpoint_id")) f->saw_workers = 1;
  json_free(body);
  atomic_fetch_add(&f->calls, 1);
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  uint64_t end = cmeta_monotonic_ms() + 5000;
  while (!atomic_load_explicit(&f->open, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
  if (f->fail || !atomic_load(&f->open)) return -77;
  if (f->text_bytes) {
    /* A transport boundary owns malloc text. Keep even the oversize fixture
     * bounded; only the model text exceeds the aggregation projection cap. */
    size_t n = f->text_bytes;
    tstr text = tstr_new_len(NULL, n);
    if (!text) return -1;
    memset(text, '\\', n);
    json_value_t *root = json_parse("{\"id\":\"long\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":\"stop\"}]}",
                                  strlen("{\"id\":\"long\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":\"stop\"}]}"));
    if (!root) { tstr_free(text); return -1; }
    json_value_t *message = json_object_get(json_array_get(json_object_get(root, "choices"), 0), "message");
    int rc = turbo_runtime_json_object_set_string(message, "content", text);
    if (!rc) *out = json_serialize(root, NULL);
    json_free(root); tstr_free(text);
    return *out ? 0 : -1;
  }
  if (f->proposal) response = "{\"id\":\"proposal\",\"output\":[{\"type\":\"function_call\","
      "\"call_id\":\"call-1\",\"name\":\"effect\",\"arguments\":\"{}\"}]}";
  else if (f->chat) response = "{\"id\":\"chat\",\"choices\":[{\"message\":{\"role\":\"assistant\","
      "\"content\":\"worker-b draft\"},\"finish_reason\":\"stop\"}]}";
  else response = "{\"id\":\"response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"worker-a draft\"}]}]}";
  *out = malloc(strlen(response) + 1);
  if (!*out) return -1;
  memcpy(*out, response, strlen(response) + 1);
  return 0;
}

static int group_allow_tool(turbo_agent_t *agent, const json_value_t *state, const char *id,
    const char *name, const char *args, const char **reason, void *user) {
  (void)agent; (void)state; (void)id; (void)name; (void)args; (void)reason; (void)user;
  return 0;
}

static void group_entered(group_transport_t *f) {
  uint64_t end = cmeta_monotonic_ms() + 3000;
  while (!atomic_load_explicit(&f->entered, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
  check_true(atomic_load_explicit(&f->entered, memory_order_acquire));
}
static void group_block(void *user) {
  group_transport_t *f = user;
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  uint64_t end = cmeta_monotonic_ms() + 5000;
  while (!atomic_load_explicit(&f->open, memory_order_acquire) && cmeta_monotonic_ms() < end)
    cmeta_thread_yield();
}
static int group_controlled(const char *request, const turbo_cancel_token_t *token,
    unsigned timeout_ms, unsigned connect_ms, turbo_agent_transport_response_t *response, void *user) {
  group_transport_t *f = user;
  (void)request; (void)timeout_ms; (void)connect_ms;
  atomic_fetch_add(&f->calls, 1);
  atomic_store_explicit(&f->entered, 1, memory_order_release);
  if (!token || turbo_cancel_token_wait(token, 5000) != TURBO_CANCEL_WAIT_SIGNALED) return -77;
  response->transport_status = turbo_cancel_token_check(token);
  return response->transport_status;
}

static void group_submit_pair(turbo_agent_router_t *router, cmeta_threadpool_t *pool,
    turbo_agent_router_request_t *request, const json_value_t *input, turbo_agent_router_task_t **tasks) {
  static const char *threads[] = {"worker-a", "worker-b"};
  for (size_t i = 0; i < 2; ++i) {
    request->thread_id = threads[i];
    check_equal(turbo_agent_router_submit(router, pool, request, input, &tasks[i]), SALTS_OK);
  }
}

spec("bounded Router worker group supervision and aggregation") {
  static group_transport_t f[4];
  static turbo_agent_t *agents[4];
  static turbo_agent_runtime_t *runtimes[3];
  static turbo_agent_router_endpoint_t endpoints[3];
  static turbo_agent_router_config_t config;
  static turbo_agent_router_request_t request;
  static turbo_agent_runtime_parent_link_t parent;
  static turbo_agent_policy_t grant;
  static turbo_agent_router_t *router;
  static turbo_agent_router_group_t *group;
  static turbo_agent_router_task_t *tasks[3];
  static turbo_agent_router_group_result_t results[3];
  static cmeta_threadpool_t *pool;
  static turbo_tool_registry_t *tools;
  static json_value_t *input, *aggregate_input, *summary, *state, *view;
  static size_t count;

  before_each() {
    cmeta_threadpool_config_t pool_config = {.num_threads = 2, .queue_capacity = 2};
    turbo_tool_definition_t tool = {0};
    tools = turbo_tool_registry_create(); check_not_null(tools);
    tool.name = "effect"; tool.description = "Never dispatched by model workers";
    tool.parameters_json = "{\"type\":\"object\"}"; tool.json_value_handler = group_effect;
    check_equal(turbo_tool_registry_add(tools, &tool), TURBO_TOOL_OK);
    group_tool_calls = 0;
    grant = turbo_agent_policy_default(); grant.allow_delegate = 1; grant.allow_custom_tools = 1;
    for (int i = 0; i < 4; ++i) {
      turbo_agent_config_t c = {0};
      memset(&f[i], 0, sizeof(f[i]));
      atomic_init(&f[i].calls, 0); atomic_init(&f[i].entered, 0); atomic_init(&f[i].open, 1);
      f[i].model = i == 0 ? "authority" : i == 1 ? "worker-a" : i == 2 ? "worker-b" : "aggregator";
      f[i].chat = i == 2;
      c.model = f[i].model;
      c.provider = f[i].chat ? turbo_model_provider_openai_compatible_chat_completions()
                            : turbo_model_provider_openai_responses();
      c.tool_registry = tools; c.transport_fn = group_transport; c.transport_user_data = &f[i];
      agents[i] = turbo_agent_create(&c); check_not_null(agents[i]);
      check_equal(turbo_agent_set_tool_policy(agents[i], &grant), SALTS_OK);
      if (i < 3) tasks[i] = NULL;
    }
    for (int i = 0; i < 3; ++i) {
      turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
      runtimes[i] = turbo_agent_runtime_create(&store); check_not_null(runtimes[i]);
      endpoints[i] = (turbo_agent_router_endpoint_t){0};
      endpoints[i].id = f[i+1].model; endpoints[i].agent = agents[i+1]; endpoints[i].runtime = runtimes[i];
      endpoints[i].roles = i == 2 ? TURBO_AGENT_ROUTER_PLAN : TURBO_AGENT_ROUTER_EXECUTE;
      endpoints[i].capabilities = 7; endpoints[i].max_difficulty = 100;
      endpoints[i].max_input_tokens = endpoints[i].max_output_tokens = 10000;
      endpoints[i].input_price_micros = endpoints[i].output_price_micros = (i+1)*1000;
      results[i] = (turbo_agent_router_group_result_t){0};
    }
    turbo_agent_router_config_init(&config); config.authority_agent = agents[0];
    config.endpoints = endpoints; config.endpoint_count = 3;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    turbo_agent_router_request_init(&request); request.role = TURBO_AGENT_ROUTER_EXECUTE;
    request.input_tokens = request.output_tokens = 1000; request.budget_micros = 20;
    request.timeout_ms = 10000; request.heartbeat_timeout_ms = 4000; request.thread_id = "worker-a";
    parent = (turbo_agent_runtime_parent_link_t){0}; parent.parent_agent_run_id = "host-run";
    request.parent_link = &parent;
    pool = cmeta_threadpool_create_with_config(&pool_config); check_not_null(pool);
    input = turbo_agent_session_create_input_state_json_value("Analyze the Host-reviewed subtask"); check_not_null(input);
    group = NULL; aggregate_input = summary = state = view = NULL; count = 0;
  }
  after_each() {
    for (int i = 0; i < 4; ++i) atomic_store_explicit(&f[i].open, 1, memory_order_release);
    if (group) {
      turbo_agent_router_group_cancel(group, TURBO_CANCEL_SHUTDOWN);
      check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
      check_equal(turbo_agent_router_group_release(group), SALTS_OK);
    }
    for (int i = 0; i < 3; ++i) {
      if (tasks[i]) {
        turbo_agent_router_task_cancel(tasks[i], TURBO_CANCEL_SHUTDOWN);
        check_equal(turbo_agent_router_task_wait(tasks[i], 5000), SALTS_OK);
        check_equal(turbo_agent_router_task_release(tasks[i]), SALTS_OK);
      }
      json_free(results[i].summary); json_free(results[i].state);
    }
    cmeta_threadpool_destroy(pool);
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    for (int i = 0; i < 3; ++i) turbo_agent_runtime_destroy(runtimes[i]);
    for (int i = 0; i < 4; ++i) turbo_agent_destroy(agents[i]);
    turbo_tool_registry_destroy(tools);
    json_free(input); json_free(aggregate_input); json_free(summary); json_free(state); json_free(view);
  }

  it("runs two real model workers concurrently, crosses the barrier and explicitly aggregates their canonical evidence") {
    atomic_store(&f[1].open, 0); atomic_store(&f[2].open, 0);
    group_submit_pair(router, pool, &request, input, tasks);
    group_entered(&f[1]); group_entered(&f[2]);
    tstr ids[2] = {tstr_dup(turbo_agent_router_task_id(tasks[0])), tstr_dup(turbo_agent_router_task_id(tasks[1]))};
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_null(tasks[0]); check_null(tasks[1]);
    check_equal(turbo_agent_router_group_task_id(group, 0), ids[0]);
    check_equal(turbo_agent_router_group_task_id(group, 1), ids[1]);
    check_null(turbo_agent_router_group_task_id(group, 2));
    check_equal(turbo_agent_router_group_wait(group, 0), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize evidence", &aggregate_input), SALTS_EBUSY);
    check_null(aggregate_input);
    check_equal(turbo_agent_router_group_release(group), SALTS_EBUSY);
    check_equal(turbo_agent_router_destroy(router), SALTS_EBUSY);
    atomic_store_explicit(&f[2].open, 1, memory_order_release);
    atomic_store_explicit(&f[1].open, 1, memory_order_release);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_poll(group, cmeta_monotonic_ms()), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize evidence", &aggregate_input), SALTS_OK);
    check_not_null(aggregate_input);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(count, 2u);
    for (int i = 0; i < 2; ++i) {
      check_equal(json_get_string(results[i].summary, "status"), "completed");
      check_equal(json_get_string(results[i].summary, "parent_agent_run_id"), "host-run");
      check_equal(json_get_string(json_object_get(results[i].state, "model_route"), "endpoint"), i ? "worker-b" : "worker-a");
    }
    check_equal(turbo_agent_router_group_inspect(group, &view), SALTS_OK);
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(view, "estimated_cost_micros"), 0), 6);
    check_true(json_get_bool(view, "all_terminal", 0)); check_true(json_get_bool(view, "input_built", 0));
    request.role = TURBO_AGENT_ROUTER_PLAN; request.thread_id = "synthesis";
    check_equal(turbo_agent_router_submit(router, pool, &request, aggregate_input, &tasks[2]), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(tasks[2], 5000), SALTS_OK);
    check_true(f[3].saw_workers); check_equal(atomic_load(&f[3].calls), 1);
    check_equal(group_tool_calls, 0);
    tstr_free(ids[0]); tstr_free(ids[1]);
  }

  for (int invalid = 0; invalid < 5; ++invalid) {
    it("rejects invalid adoption without consuming any accepted handle, case=%d", invalid) {
      group_submit_pair(router, pool, &request, input, tasks);
      turbo_agent_router_task_t *members[2] = {tasks[0], tasks[1]};
      if (invalid == 0) members[1] = members[0];
      if (invalid == 1) members[1] = NULL;
      if (invalid == 2) {
        check_equal(turbo_agent_router_task_wait(tasks[0], 5000), SALTS_OK);
        check_equal(turbo_agent_router_task_take_result(tasks[0], &summary, &state), SALTS_OK);
      }
      check_equal(turbo_agent_router_group_adopt(invalid == 3 ? "bad/id" : "analysis", members,
          invalid == 4 ? 1 : 2, &group), invalid == 2 ? SALTS_EALREADY : SALTS_EINVAL);
      check_null(group);
      check_true(members[0] == tasks[0]);
      if (invalid != 0 && invalid != 1) check_true(members[1] == tasks[1]);
    }
  }

  for (int mismatch = 0; mismatch < 2; ++mismatch) {
    it("rejects ambiguous thread or mismatched parent identity before ownership transfer, case=%d", mismatch) {
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[0]), SALTS_OK);
      if (mismatch) { parent.parent_agent_run_id = "other-host"; request.thread_id = "worker-b"; }
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[1]), SALTS_OK);
      check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_EINVAL);
      check_not_null(tasks[0]); check_not_null(tasks[1]); check_null(group);
    }
  }

  it("rejects tasks from different Routers without consuming them") {
    turbo_agent_router_t *other = NULL;
    turbo_agent_router_config_t other_config = config;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[0]), SALTS_OK);
    other_config.endpoints = &endpoints[2]; other_config.endpoint_count = 1;
    check_equal(turbo_agent_router_create(&other_config, &other), SALTS_OK);
    request.role = TURBO_AGENT_ROUTER_PLAN; request.thread_id = "worker-b";
    check_equal(turbo_agent_router_submit(other, pool, &request, input, &tasks[1]), SALTS_OK);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_EINVAL);
    check_not_null(tasks[0]); check_not_null(tasks[1]); check_null(group);
    check_equal(turbo_agent_router_task_wait(tasks[1], 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_release(tasks[1]), SALTS_OK); tasks[1] = NULL;
    check_equal(turbo_agent_router_destroy(other), SALTS_OK);
  }

  it("keeps supervised Host tool loops outside parallel model groups") {
    turbo_agent_guardrail_t guard = {0}; guard.before_tool = group_allow_tool;
    check_equal(turbo_agent_add_guardrail(agents[0], &guard), SALTS_OK);
    request.required_capabilities = TURBO_AGENT_ROUTER_TOOL_PROPOSALS;
    check_equal(turbo_agent_router_submit_tool_loop(router, pool, &request, input, 2, &tasks[0]), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(tasks[0], 5000), SALTS_OK);
    turbo_agent_router_task_t *members[2] = {tasks[0], tasks[0]};
    check_equal(turbo_agent_router_group_adopt("analysis", members, 2, &group), SALTS_EINVAL);
    check_true(members[0] == tasks[0]); check_null(group);
    request.thread_id = "worker-b";
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[1]), SALTS_EBUSY);
    check_null(tasks[1]); check_equal(group_tool_calls, 0);
  }

  it("prevents aliases from releasing, taking or controlling a group-owned task") {
    group_submit_pair(router, pool, &request, input, tasks);
    turbo_agent_router_task_t *alias = tasks[0];
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_task_release(alias), SALTS_EBUSY);
    check_equal(turbo_agent_router_task_take_result(alias, &summary, &state), SALTS_EBUSY);
    check_equal(turbo_agent_router_task_poll(alias, cmeta_monotonic_ms()), SALTS_EBUSY);
    check_equal(turbo_agent_router_task_cancel(alias, TURBO_CANCEL_USER), SALTS_EBUSY);
    turbo_agent_router_task_t *duplicate[2] = {alias, alias};
    turbo_agent_router_group_t *another = NULL;
    check_equal(turbo_agent_router_group_adopt("other", duplicate, 2, &another), SALTS_EBUSY);
    check_null(another);
  }

  it("delivers each canonical pair once and releases endpoint reservations only with the group") {
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_take_results(group, results, 1, &count), SALTS_EINVAL);
    check_equal(count, 0u);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(count, 2u);
    turbo_agent_router_group_result_t again[2] = {0};
    check_equal(turbo_agent_router_group_take_results(group, again, 2, &count), SALTS_EALREADY);
    check_equal(count, 0u); check_null(again[0].summary); check_null(again[1].state);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_EALREADY);
    request.thread_id = "other";
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[2]), SALTS_EBUSY);
    check_null(tasks[2]);
    check_equal(turbo_agent_router_group_release(group), SALTS_OK); group = NULL;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[2]), SALTS_OK);
  }

  it("builds aggregation input once without consuming or copying the canonical result owner") {
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_OK);
    json_value_t *again = NULL;
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &again), SALTS_EALREADY);
    check_null(again);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_not_null(results[0].state); check_not_null(results[1].state);
  }

  it("halts aggregation and cancels siblings on an observed Provider failure while preserving diagnostic results") {
    f[1].fail = 1;
    check_equal(turbo_agent_transport_v2_set(agents[2], group_controlled, &f[2], NULL), SALTS_OK);
    group_submit_pair(router, pool, &request, input, tasks); group_entered(&f[2]);
    check_equal(turbo_agent_router_task_wait(tasks[0], 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_poll(group, cmeta_monotonic_ms()), SALTS_EIO);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_EIO);
    check_null(aggregate_input);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_null(results[0].summary); check_not_null(turbo_agent_state_model_error_detail(results[0].state));
    check_equal(json_get_string(results[1].summary, "status"), "cancelled");
    check_equal(atomic_load(&f[1].calls), 1); check_equal(atomic_load(&f[2].calls), 1);
    check_equal(atomic_load(&f[3].calls), 0);
  }

  it("keeps unresolved tool proposals out of aggregation without dispatching effects") {
    f[1].proposal = 1;
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_EPROTO);
    check_null(aggregate_input); check_equal(group_tool_calls, 0);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(turbo_agent_state_pending_tool_calls(results[0].state), 1u);
  }

  for (int owner = 0; owner < 3; ++owner) {
    it("rejects all fan-in delivery after quiescent same-grant replacement, owner=%d", owner) {
      group_submit_pair(router, pool, &request, input, tasks);
      check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
      check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
      check_equal(turbo_agent_set_tool_policy(agents[owner], &grant), SALTS_OK);
      check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_EPERM);
      check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_EPERM);
      check_equal(count, 0u); check_null(results[0].summary); check_null(results[1].state);
      check_equal(turbo_agent_router_group_poll(group, cmeta_monotonic_ms()), SALTS_EPERM);
      check_null(aggregate_input);
    }
  }

  it("rejects raw-result delivery after authority changes following an authorized input projection") {
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_OK);
    check_equal(turbo_agent_set_tool_policy(agents[2], &grant), SALTS_OK);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_EPERM);
    check_equal(count, 0u); check_not_null(aggregate_input);
  }

  it("retains a lost worker until actual terminal and rejects every late fan-in without retry") {
    atomic_store(&f[1].open, 0); atomic_store(&f[2].open, 0);
    group_submit_pair(router, pool, &request, input, tasks); group_entered(&f[1]); group_entered(&f[2]);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_inspect(group, &view), SALTS_OK);
    const json_value_t *members = json_object_get(view, "members");
    uint64_t now = (uint64_t)turbo_runtime_json_value_as_int64(json_object_get(view, "observed_mono_ms"), 0);
    check_equal(json_array_size(members), 2u);
    check_equal(turbo_agent_router_group_poll(group, now + 4000), SALTS_ECANCELED);
    check_equal(turbo_agent_router_group_heartbeat(group, 0, now + 4001), SALTS_ECANCELED);
    check_equal(turbo_agent_router_group_release(group), SALTS_EBUSY);
    atomic_store_explicit(&f[1].open, 1, memory_order_release); atomic_store_explicit(&f[2].open, 1, memory_order_release);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_ECANCELED);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_ECANCELED);
    check_equal(count, 0u); check_equal(atomic_load(&f[3].calls), 0);
    check_equal(atomic_load(&f[1].calls) + atomic_load(&f[2].calls), 2);
  }

  it("checks every member before heartbeat renewal and rejects backward clock input without mutation") {
    atomic_store(&f[1].open, 0); atomic_store(&f[2].open, 0);
    group_submit_pair(router, pool, &request, input, tasks); group_entered(&f[1]); group_entered(&f[2]);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_inspect(group, &view), SALTS_OK);
    uint64_t now = (uint64_t)turbo_runtime_json_value_as_int64(json_object_get(view, "observed_mono_ms"), 0);
    check_equal(turbo_agent_router_group_heartbeat(group, 0, now + 1000), SALTS_OK);
    check_equal(turbo_agent_router_group_heartbeat(group, 0, now + 1), SALTS_EINVAL);
    /* Renewing member 0 cannot hide member 1's missed interval. */
    check_equal(turbo_agent_router_group_heartbeat(group, 0, now + 4000), SALTS_ECANCELED);
  }

  it("uses one total finite wait budget and cooperative group cancellation") {
    for (int i = 1; i <= 2; ++i)
      check_equal(turbo_agent_transport_v2_set(agents[i], group_controlled, &f[i], NULL), SALTS_OK);
    group_submit_pair(router, pool, &request, input, tasks); group_entered(&f[1]); group_entered(&f[2]);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_router_group_cancel(group, TURBO_CANCEL_NONE), SALTS_EINVAL);
    check_equal(turbo_agent_router_group_cancel(group, TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_agent_router_group_cancel(group, TURBO_CANCEL_USER), SALTS_EALREADY);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_ECANCELED);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(json_get_string(results[0].summary, "status"), "cancelled");
    check_equal(json_get_string(results[1].summary, "status"), "cancelled");
  }

  it("blocks aggregation on actual Execution deadline expiry") {
    for (int i = 1; i <= 2; ++i)
      check_equal(turbo_agent_transport_v2_set(agents[i], group_controlled, &f[i], NULL), SALTS_OK);
    request.timeout_ms = request.heartbeat_timeout_ms = 300;
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_poll(group, cmeta_monotonic_ms()), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_ETIMEDOUT);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(json_get_string(results[0].summary, "status"), "timed_out");
  }

  it("rejects oversized worker text without truncation or consuming canonical results") {
    f[2].text_bytes = TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1;
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(group, "Synthesize", &aggregate_input), SALTS_ENOBUFS);
    check_null(aggregate_input);
    check_equal(turbo_agent_router_group_take_results(group, results, 3, &count), SALTS_OK);
    check_equal(strlen(turbo_agent_state_last_output_text(results[1].state)), (size_t)TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1);
  }

  it("rejects oversized Host instructions and permits a bounded retry without another model call") {
    group_submit_pair(router, pool, &request, input, tasks);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(group, 5000), SALTS_OK);
    tstr text = tstr_new_len(NULL, TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1);
    check_not_null(text); memset(text, 'x', TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1);
    check_equal(turbo_agent_router_group_build_input(group, text, &aggregate_input), SALTS_ENOBUFS);
    check_null(aggregate_input);
    check_equal(turbo_agent_router_group_build_input(group, "Bounded synthesis", &aggregate_input), SALTS_OK);
    check_equal(atomic_load(&f[1].calls), 1); check_equal(atomic_load(&f[2].calls), 1);
    tstr_free(text);
  }

  it("bounds the serialized aggregation projection across sixteen individually valid worker outputs") {
    static const char *ids[] = {"large-0", "large-1", "large-2", "large-3", "large-4", "large-5",
        "large-6", "large-7", "large-8", "large-9", "large-10", "large-11", "large-12", "large-13", "large-14", "large-15"};
    group_transport_t transport[16];
    turbo_agent_t *children[16] = {0};
    turbo_agent_runtime_t *stores[16] = {0};
    turbo_agent_router_endpoint_t routes[16] = {0};
    turbo_agent_router_task_t *members[16] = {0};
    turbo_agent_router_group_result_t pairs[16] = {0};
    turbo_agent_router_t *large_router = NULL;
    turbo_agent_router_group_t *large_group = NULL;
    cmeta_threadpool_config_t pool_config = {.num_threads = 4, .queue_capacity = 16};
    cmeta_threadpool_t *large_pool = cmeta_threadpool_create_with_config(&pool_config);
    check_not_null(large_pool);
    for (int i = 0; i < 16; ++i) {
      turbo_agent_config_t c = {0};
      turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
      memset(&transport[i], 0, sizeof(transport[i]));
      atomic_init(&transport[i].calls, 0); atomic_init(&transport[i].entered, 0); atomic_init(&transport[i].open, 1);
      transport[i].model = "large"; transport[i].chat = 1;
      transport[i].text_bytes = TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES;
      c.model = "large"; c.provider = turbo_model_provider_openai_compatible_chat_completions();
      c.transport_fn = group_transport; c.transport_user_data = &transport[i];
      children[i] = turbo_agent_create(&c); stores[i] = turbo_agent_runtime_create(&store);
      check_not_null(children[i]); check_not_null(stores[i]);
      routes[i].id = ids[i]; routes[i].agent = children[i]; routes[i].runtime = stores[i];
      routes[i].roles = TURBO_AGENT_ROUTER_EXECUTE; routes[i].capabilities = TURBO_AGENT_ROUTER_TEXT;
      routes[i].max_input_tokens = routes[i].max_output_tokens = 10000;
      routes[i].input_price_micros = routes[i].output_price_micros = 1000;
    }
    turbo_agent_router_config_t large_config;
    turbo_agent_router_config_init(&large_config); large_config.authority_agent = agents[0];
    large_config.endpoints = routes; large_config.endpoint_count = 16;
    check_equal(turbo_agent_router_create(&large_config, &large_router), SALTS_OK);
    for (int i = 0; i < 16; ++i) {
      request.thread_id = ids[i];
      check_equal(turbo_agent_router_submit(large_router, large_pool, &request, input, &members[i]), SALTS_OK);
    }
    check_equal(turbo_agent_router_group_adopt("large", members, 16, &large_group), SALTS_OK);
    check_equal(turbo_agent_router_group_wait(large_group, 10000), SALTS_OK);
    check_equal(turbo_agent_router_group_build_input(large_group, "Synthesize", &aggregate_input), SALTS_ENOBUFS);
    check_null(aggregate_input);
    check_equal(turbo_agent_router_group_take_results(large_group, pairs, 16, &count), SALTS_OK);
    check_equal(count, 16u);
    check_equal(turbo_agent_router_group_release(large_group), SALTS_OK);
    check_equal(turbo_agent_router_destroy(large_router), SALTS_OK);
    cmeta_threadpool_destroy(large_pool);
    for (int i = 0; i < 16; ++i) {
      check_equal(strlen(turbo_agent_state_last_output_text(pairs[i].state)),
                  (size_t)TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES);
      json_free(pairs[i].summary); json_free(pairs[i].state);
      turbo_agent_runtime_destroy(stores[i]); turbo_agent_destroy(children[i]);
    }
  }

  it("leaves an accepted task owned by the Host when a later bounded queue admission is rejected") {
    cmeta_threadpool_destroy(pool);
    cmeta_threadpool_config_t small = {.num_threads = 1, .queue_capacity = 1};
    pool = cmeta_threadpool_create_with_config(&small); check_not_null(pool);
    atomic_store(&f[0].open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, group_block, &f[0]), SALTS_OK); group_entered(&f[0]);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[0]), SALTS_OK);
    request.thread_id = "worker-b";
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &tasks[1]), SALTS_ENOBUFS);
    check_null(tasks[1]); check_not_null(tasks[0]);
    check_equal(turbo_agent_router_group_adopt("analysis", tasks, 2, &group), SALTS_EINVAL);
    check_not_null(tasks[0]); check_null(group);
    check_equal(turbo_agent_router_task_cancel(tasks[0], TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_agent_router_task_release(tasks[0]), SALTS_EBUSY);
    atomic_store_explicit(&f[0].open, 1, memory_order_release);
    check_equal(turbo_agent_router_task_wait(tasks[0], 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(tasks[0], &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "cancelled");
    check_equal(atomic_load(&f[1].calls) + atomic_load(&f[2].calls), 0);
  }
}
