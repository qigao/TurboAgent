#include "tinytest.h"
#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct router_transport_s {
  atomic_int calls, entered, open;
  int chat, fail, tool_proposal;
  const char *model;
} router_transport_t;

static int router_tool_calls;

static int router_fixture_tool(const json_value_t *args, json_value_t **out, void *user) {
  (void)args; (void)user;
  ++router_tool_calls;
  *out = json_create_bool(1);
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int router_fixture_transport(const char *request, char **out, void *user) {
  static const char responses[] =
      "{\"id\":\"router-response\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"draft\"}]}],"
      "\"usage\":{\"input_tokens\":4,\"output_tokens\":2,\"total_tokens\":6}}";
  static const char chat[] =
      "{\"id\":\"router-chat\",\"choices\":[{\"message\":{\"role\":\"assistant\","
      "\"content\":\"draft\"},\"finish_reason\":\"stop\"}],"
      "\"usage\":{\"prompt_tokens\":4,\"completion_tokens\":2,\"total_tokens\":6}}";
  static const char proposal[] =
      "{\"id\":\"router-proposal\",\"output\":[{\"type\":\"function_call\","
      "\"call_id\":\"call-1\",\"name\":\"effect\",\"arguments\":\"{}\"}]}";
  router_transport_t *fixture = user;
  json_value_t *body = json_parse(request, strlen(request));
  const char *text = fixture->tool_proposal ? proposal : fixture->chat ? chat : responses;
  uint64_t stop = cmeta_monotonic_ms() + 3000;
  *out = NULL;
  if (!body || !json_get_string(body, "model") ||
      strcmp(json_get_string(body, "model"), fixture->model) != 0) {
    json_free(body);
    return -1;
  }
  json_free(body);
  atomic_fetch_add(&fixture->calls, 1);
  atomic_store_explicit(&fixture->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&fixture->open, memory_order_acquire) &&
         cmeta_monotonic_ms() < stop) cmeta_thread_yield();
  if (!atomic_load(&fixture->open) || fixture->fail) return -77;
  *out = malloc(strlen(text) + 1);
  if (!*out) return -1;
  memcpy(*out, text, strlen(text) + 1);
  return 0;
}

static int router_fixture_controlled(const char *request, const turbo_cancel_token_t *token,
    unsigned timeout_ms, unsigned connect_ms, turbo_agent_transport_response_t *response,
    void *user) {
  router_transport_t *fixture = user;
  (void)request; (void)timeout_ms; (void)connect_ms;
  atomic_fetch_add(&fixture->calls, 1);
  atomic_store_explicit(&fixture->entered, 1, memory_order_release);
  if (!token || turbo_cancel_token_wait(token, 3000) != TURBO_CANCEL_WAIT_SIGNALED)
    return -77;
  response->transport_status = turbo_cancel_token_check(token);
  return response->transport_status;
}

static turbo_agent_t *router_fixture_agent(router_transport_t *fixture,
                                            turbo_tool_registry_t *tools) {
  turbo_agent_config_t config = {0};
  config.model = fixture->model;
  config.provider = fixture->chat ? turbo_model_provider_openai_compatible_chat_completions()
                                 : turbo_model_provider_openai_responses();
  config.transport_fn = router_fixture_transport;
  config.transport_user_data = fixture;
  config.tool_registry = tools;
  return turbo_agent_create(&config);
}

static void router_wait_entered(router_transport_t *fixture) {
  uint64_t stop = cmeta_monotonic_ms() + 2000;
  while (!atomic_load_explicit(&fixture->entered, memory_order_acquire) &&
         cmeta_monotonic_ms() < stop) cmeta_thread_yield();
  check_true(atomic_load_explicit(&fixture->entered, memory_order_acquire));
}

static void router_queue_block(void *user) {
  router_transport_t *fixture = user;
  uint64_t stop = cmeta_monotonic_ms() + 3000;
  atomic_store_explicit(&fixture->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&fixture->open, memory_order_acquire) &&
         cmeta_monotonic_ms() < stop) cmeta_thread_yield();
}

spec("bounded tiered model Router") {
  static router_transport_t transport[3];
  static turbo_agent_t *agents[3];
  static turbo_agent_runtime_t *runtimes[2];
  static turbo_agent_router_endpoint_t endpoints[2];
  static turbo_agent_router_config_t config;
  static turbo_agent_router_request_t request;
  static turbo_agent_runtime_parent_link_t parent;
  static turbo_agent_policy_t grant;
  static turbo_agent_router_t *router;
  static turbo_agent_router_task_t *task;
  static cmeta_threadpool_t *pool;
  static turbo_tool_registry_t *tools;
  static json_value_t *input, *summary, *state, *view;

  before_each() {
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_tool_definition_t tool = {0};
    for (int i = 0; i < 3; ++i) {
      memset(&transport[i], 0, sizeof(transport[i]));
      atomic_init(&transport[i].calls, 0);
      atomic_init(&transport[i].entered, 0);
      atomic_init(&transport[i].open, 1);
      transport[i].model = i == 0 ? "authority" : i == 1 ? "planner" : "worker";
      transport[i].chat = i == 2;
    }
    tools = turbo_tool_registry_create();
    check_not_null(tools);
    tool.name = "effect";
    tool.description = "Offline effect fixture; the Router must never dispatch it.";
    tool.parameters_json = "{\"type\":\"object\"}";
    tool.json_value_handler = router_fixture_tool;
    check_equal(turbo_tool_registry_add(tools, &tool), TURBO_TOOL_OK);
    router_tool_calls = 0;
    grant = turbo_agent_policy_default();
    grant.allow_delegate = 1;
    grant.allow_custom_tools = 1;
    for (int i = 0; i < 3; ++i) {
      agents[i] = router_fixture_agent(&transport[i], i == 1 ? tools : NULL);
      check_not_null(agents[i]);
      if (i == 0) check_equal(turbo_agent_set_tool_policy(agents[i], &grant), 0);
    }
    for (int i = 0; i < 2; ++i) {
      turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
      runtimes[i] = turbo_agent_runtime_create(&store);
      check_not_null(runtimes[i]);
      endpoints[i] = (turbo_agent_router_endpoint_t){0};
      endpoints[i].id = i == 0 ? "strong" : "fast";
      endpoints[i].agent = agents[i + 1];
      endpoints[i].runtime = runtimes[i];
      endpoints[i].roles = i == 0 ? 3u : TURBO_AGENT_ROUTER_EXECUTE;
      endpoints[i].capabilities = i == 0 ? 7u : TURBO_AGENT_ROUTER_TEXT;
      endpoints[i].max_difficulty = i == 0 ? 100 : 50;
      endpoints[i].max_input_tokens = endpoints[i].max_output_tokens = 10000;
      endpoints[i].input_price_micros = endpoints[i].output_price_micros = i == 0 ? 10000 : 1000;
    }
    turbo_agent_router_config_init(&config);
    config.authority_agent = agents[0];
    config.endpoints = endpoints;
    config.endpoint_count = 2;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    turbo_agent_router_request_init(&request);
    request.role = TURBO_AGENT_ROUTER_EXECUTE;
    request.difficulty = 20;
    request.input_tokens = request.output_tokens = 1000;
    request.budget_micros = 20;
    request.timeout_ms = 10000;
    request.heartbeat_timeout_ms = 5000;
    request.thread_id = "child-router";
    parent = (turbo_agent_runtime_parent_link_t){0};
    parent.parent_agent_run_id = "parent-run";
    parent.parent_tool_call_id = "parent-call";
    request.parent_link = &parent;
    pool = cmeta_threadpool_create_with_config(&pool_config);
    check_not_null(pool);
    input = turbo_agent_session_create_input_state_json_value("perform the Host task");
    check_not_null(input);
    summary = state = view = NULL;
    task = NULL;
  }

  after_each() {
    for (int i = 0; i < 3; ++i) atomic_store_explicit(&transport[i].open, 1, memory_order_release);
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
  }

  it("routes execution to the cheaper Provider and preserves canonical lineage, usage and one result owner") {
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_not_null(turbo_agent_router_task_id(task));
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(json_get_string(summary, "parent_agent_run_id"), "parent-run");
    check_equal(json_get_string(json_object_get(state, "model_route"), "endpoint"), "fast");
    check_equal(json_get_string(json_object_get(state, "model_route"), "model"), "worker");
    check_equal(json_array_size(json_object_get(state, "usage_records")), 1u);
    check_equal(atomic_load(&transport[1].calls), 0);
    check_equal(atomic_load(&transport[2].calls), 1);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    check_equal(json_type(json_object_get(view, "actual_cost_micros")), JSON_NULL);
    json_value_t *again_summary = NULL, *again_state = NULL;
    check_equal(turbo_agent_router_task_take_result(task, &again_summary, &again_state), SALTS_EALREADY);
    check_null(again_summary); check_null(again_state);
  }

  for (int reason = 0; reason < 3; ++reason) {
    it("routes planning, harder tasks and required model features explicitly, reason %d", reason) {
      if (reason == 0) request.role = TURBO_AGENT_ROUTER_PLAN;
      if (reason == 1) request.difficulty = 80;
      if (reason == 2) request.required_capabilities = TURBO_AGENT_ROUTER_JSON;
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(atomic_load(&transport[1].calls), 1);
      check_equal(atomic_load(&transport[2].calls), 0);
    }
  }

  for (int invalid = 0; invalid < 5; ++invalid) {
    it("rejects invalid or unaffordable requests without a Provider call, case %d", invalid) {
      int expected = SALTS_EINVAL;
      if (invalid == 0) { request.budget_micros = 1; expected = SALTS_ENOENT; }
      if (invalid == 1) request.difficulty = 101;
      if (invalid == 2) { request.input_tokens = 10001; expected = SALTS_ENOENT; }
      if (invalid == 3) request.timeout_ms = 0;
      if (invalid == 4) request.required_capabilities = 8;
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), expected);
      check_null(task);
      check_equal(atomic_load(&transport[1].calls) + atomic_load(&transport[2].calls), 0);
    }
  }

  it("requires delegation and never widens Host grants to match a child") {
    grant.allow_delegate = 0;
    check_equal(turbo_agent_set_tool_policy(agents[0], &grant), 0);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_EACCES);
    check_null(task);
    grant.allow_delegate = 1;
    check_equal(turbo_agent_set_tool_policy(agents[0], &grant), 0);
    turbo_agent_policy_t child = turbo_agent_policy_default();
    child.allow_shell = 1;
    check_equal(turbo_agent_set_tool_policy(agents[1], &child), 0);
    check_equal(turbo_agent_set_tool_policy(agents[2], &child), 0);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_ENOENT);
    check_null(task);
    check_equal(atomic_load(&transport[1].calls) + atomic_load(&transport[2].calls), 0);
  }

  it("supports two independently configured models on the same Provider") {
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    turbo_agent_destroy(agents[2]);
    transport[2].chat = 0;
    agents[2] = router_fixture_agent(&transport[2], NULL);
    endpoints[1].agent = agents[2];
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(atomic_load(&transport[2].calls), 1);
  }

  it("keeps an unresponsive worker owned until terminal and rejects late delivery without retry") {
    atomic_store(&transport[2].open, 0);
    request.budget_micros = 2;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    router_wait_entered(&transport[2]);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_EBUSY);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    uint64_t start = (uint64_t)turbo_runtime_json_value_as_int64(json_object_get(view, "heartbeat_mono_ms"), 0);
    json_free(view); view = NULL;
    check_equal(turbo_agent_router_task_poll(task, start + request.heartbeat_timeout_ms), SALTS_ECANCELED);
    check_equal(turbo_agent_router_task_heartbeat(task, start + request.heartbeat_timeout_ms + 1), SALTS_ECANCELED);
    check_equal(turbo_agent_router_task_release(task), SALTS_EBUSY);
    check_equal(turbo_agent_router_destroy(router), SALTS_EBUSY);
    turbo_agent_router_task_t *second = NULL;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &second), SALTS_EBUSY);
    check_null(second);
    atomic_store_explicit(&transport[2].open, 1, memory_order_release);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_ECANCELED);
    check_null(summary); check_null(state);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    check_true(json_get_bool(view, "unresponsive", 0));
    check_equal(atomic_load(&transport[2].calls), 1);
    check_equal(atomic_load(&transport[1].calls), 0);
  }

  it("renews a timely heartbeat and rejects backward clock observations") {
    atomic_store(&transport[2].open, 0);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    router_wait_entered(&transport[2]);
    check_equal(turbo_agent_router_task_inspect(task, &view), SALTS_OK);
    uint64_t start = (uint64_t)turbo_runtime_json_value_as_int64(json_object_get(view, "heartbeat_mono_ms"), 0);
    json_free(view); view = NULL;
    check_equal(turbo_agent_router_task_heartbeat(task, start + 4999), SALTS_OK);
    check_equal(turbo_agent_router_task_poll(task, start + 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_poll(task, start + 1), SALTS_EINVAL);
    atomic_store_explicit(&transport[2].open, 1, memory_order_release);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
  }

  for (int deadline = 0; deadline < 2; ++deadline) {
    it("uses existing cooperative cancellation and deadline terminal states, deadline=%d", deadline) {
      check_equal(turbo_agent_transport_v2_set(agents[2], router_fixture_controlled, &transport[2], NULL), SALTS_OK);
      if (deadline) request.timeout_ms = request.heartbeat_timeout_ms = 500;
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
      if (!deadline) {
        router_wait_entered(&transport[2]);
        check_equal(turbo_agent_router_task_cancel(task, TURBO_CANCEL_USER), SALTS_OK);
      }
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
      check_equal(json_get_string(summary, "status"), deadline ? "timed_out" : "cancelled");
      check_equal(atomic_load(&transport[1].calls), 0);
    }
  }

  it("publishes Provider failure without selecting another endpoint") {
    transport[2].fail = 1;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "failed");
    check_equal(atomic_load(&transport[2].calls), 1);
    check_equal(atomic_load(&transport[1].calls), 0);
  }

  for (int child = 0; child < 2; ++child) {
    it("invalidates delivery after quiescent same-grant policy replacement, child=%d", child) {
      check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
      check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
      turbo_agent_policy_t policy = child ? turbo_agent_policy_default() : grant;
      check_equal(turbo_agent_set_tool_policy(agents[child ? 2 : 0], &policy), 0);
      check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_EACCES);
      check_null(summary); check_null(state);
      check_equal(atomic_load(&transport[2].calls), 1);
    }
  }

  it("returns a model tool proposal without executing even an authorized registered tool") {
    transport[1].tool_proposal = 1;
    request.role = TURBO_AGENT_ROUTER_PLAN;
    request.required_capabilities = TURBO_AGENT_ROUTER_TOOL_PROPOSALS;
    turbo_agent_policy_t child = turbo_agent_policy_default();
    child.allow_custom_tools = 1;
    check_equal(turbo_agent_set_tool_policy(agents[1], &child), 0);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(turbo_agent_router_task_take_result(task, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(router_tool_calls, 0);
    check_equal(atomic_load(&transport[1].calls), 1);
  }

  it("rejects duplicate identities and shared state owners before admission") {
    turbo_agent_router_t *invalid = NULL;
    endpoints[1].runtime = endpoints[0].runtime;
    check_equal(turbo_agent_router_create(&config, &invalid), SALTS_EINVAL);
    check_null(invalid);
    endpoints[1].runtime = runtimes[1];
    endpoints[1].id = endpoints[0].id;
    check_equal(turbo_agent_router_create(&config, &invalid), SALTS_EINVAL);
    check_null(invalid);
  }

  it("preserves bounded executor rejection and rolls back the endpoint reservation") {
    atomic_store(&transport[0].open, 0);
    check_equal(cmeta_threadpool_try_submit(pool, router_queue_block, &transport[0]), SALTS_OK);
    router_wait_entered(&transport[0]);
    check_equal(cmeta_threadpool_try_submit(pool, router_queue_block, &transport[0]), SALTS_OK);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_ENOBUFS);
    check_null(task);
    check_equal(atomic_load(&transport[2].calls), 0);
    atomic_store_explicit(&transport[0].open, 1, memory_order_release);
    cmeta_threadpool_wait(pool);
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&transport[2].calls), 1);
  }

  it("rounds each token direction up and breaks equal-cost ties by configuration order") {
    check_equal(turbo_agent_router_destroy(router), SALTS_OK);
    for (int i = 0; i < 2; ++i) endpoints[i].input_price_micros = endpoints[i].output_price_micros = 1;
    check_equal(turbo_agent_router_create(&config, &router), SALTS_OK);
    request.input_tokens = request.output_tokens = 1;
    request.budget_micros = 1;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_ENOENT);
    request.budget_micros = 2;
    check_equal(turbo_agent_router_submit(router, pool, &request, input, &task), SALTS_OK);
    check_equal(turbo_agent_router_task_wait(task, 5000), SALTS_OK);
    check_equal(atomic_load(&transport[1].calls), 1);
    check_equal(atomic_load(&transport[2].calls), 0);
  }
}
