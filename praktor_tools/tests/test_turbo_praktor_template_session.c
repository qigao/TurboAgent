#include "tinytest.h"
#include "turbo_praktor_approved_template.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store_sqlite.h"

#include <salts/thread_pool.h>
#include <salts/thread.h>
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct session_probe_s {
  int inspect, change, verify;
  int wait_for_cancel;
  atomic_int change_entered;
  atomic_int cancel_observed;
} session_probe_t;

static int no_model(const char *request, char **response, void *user) {
  (void)request; (void)response; (void)user;
  return -1;
}

static turbo_tool_status_t session_inspect(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  session_probe_t *p = user;
  (void)args; (void)ctx;
  ++p->inspect;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t session_change(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  session_probe_t *p = user;
  (void)args; (void)ctx;
  ++p->change;
  if (p->wait_for_cancel) {
    const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
    atomic_store_explicit(&p->change_entered, 1, memory_order_release);
    /* This callback is deliberately cooperative. Absence of a borrowed
     * Session token fails the test without hanging the CI worker. */
    while (ctx && ctx->cancel_token &&
           turbo_cancel_token_check(ctx->cancel_token) == SALTS_OK &&
           cmeta_monotonic_ms() < stop)
      cmeta_thread_yield();
    if (ctx && ctx->cancel_token &&
        turbo_cancel_token_check(ctx->cancel_token) == SALTS_ECANCELED) {
      atomic_store_explicit(&p->cancel_observed, 1, memory_order_release);
      return TURBO_TOOL_CANCELLED;
    }
    return TURBO_TOOL_ERROR;
  }
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t session_verify(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  session_probe_t *p = user;
  (void)args; (void)ctx;
  ++p->verify;
  *out = json_create_object();
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  return turbo_runtime_json_object_set(*out, "verified", json_create_bool(1)) ==
             TURBO_RUNTIME_JSON_OK ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int register_session_tool(
    turbo_tool_registry_t *r, const char *name, turbo_tool_idempotency_t idem,
    turbo_tool_effect_flags_t effects,
    turbo_tool_json_value_context_handler_fn cb, session_probe_t *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t d = {0};
  d.struct_size = sizeof(d);
  d.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  d.definition.name = name;
  d.definition.description = name;
  d.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  d.definition.strict = 1;
  d.definition.user_data = probe;
  d.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  d.execution_policy.idempotency = idem;
  d.required_capabilities = caps;
  d.required_capability_count = 1;
  d.json_value_context_handler = cb;
  d.effect_flags = effects;
  return turbo_tool_registry_add_v6(r, &d) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *session_registry(session_probe_t *probe) {
  turbo_tool_registry_t *r = turbo_tool_registry_create();
  if (!r) return NULL;
  if (!register_session_tool(r, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                             TURBO_TOOL_EFFECT_READ, session_inspect, probe) ||
      !register_session_tool(r, "repo.change", TURBO_TOOL_IDEMPOTENCY_NONE,
                             TURBO_TOOL_EFFECT_WRITE, session_change, probe) ||
      !register_session_tool(r, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                             TURBO_TOOL_EFFECT_READ, session_verify, probe)) {
    turbo_tool_registry_destroy(r);
    return NULL;
  }
  return r;
}

static turbo_agent_template_plan_t *session_plan(
    turbo_tool_registry_t *registry, const json_value_t *args) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t compiler;
  turbo_agent_change_source_t source;
  turbo_agent_template_plan_t *plan = NULL;
  turbo_agent_compiler_config_init(&compiler);
  compiler.allowed_capabilities = caps;
  compiler.allowed_capability_count = 1;
  turbo_agent_change_source_init(&source);
  source.inspect.tool_name = "repo.inspect";
  source.inspect.arguments = args;
  source.change.tool_name = "repo.change";
  source.change.arguments = args;
  source.verify.tool_name = "repo.verify";
  source.verify.arguments = args;
  return turbo_agent_compile_change_template(
             &compiler, registry, &source, &plan, NULL) ==
                 TURBO_AGENT_COMPILE_OK ? plan : NULL;
}

static turbo_agent_session_t *new_session(
    turbo_tool_registry_t *registry, const char *thread,
    turbo_agent_runtime_store_t store) {
  turbo_agent_session_config_t config = {0};
  config.runtime_store = store;
  config.thread_id = thread;
  config.agent_config.model = "test";
  config.agent_config.transport_fn = no_model;
  config.agent_config.tool_registry = registry;
  return turbo_agent_session_create(&config);
}

spec("approved Praktor asynchronous Session Checkpoint") {
  it("interrupts before approval, resumes after host approval, replays without another callback") {
    session_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 2};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = session_registry(&probe);
    turbo_agent_session_t *session = new_session(
        registry, "praktor_session_A", turbo_agent_runtime_store_memory_create());
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = session_plan(registry, args);
    turbo_praktor_template_session_run_t *first = NULL, *second = NULL, *third = NULL, *denied = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    json_value_t *summary = NULL, *state = NULL;
    char *checkpoint_id = NULL;
    const char *id;

    check_not_null(pool); check_not_null(registry);
    check_not_null(session); check_not_null(plan);
    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "first_attempt", "host_approval_1",
        NULL, 1, 0, &first), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(first, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(first, &status), SALTS_OK);
    check_equal(status, TURBO_AGENT_EXECUTION_INTERRUPTED);
    check_equal(turbo_praktor_template_session_run_take_result(
        first, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "interrupted");
    id = json_get_string(summary, "checkpoint_id");
    check_not_null(id);
    checkpoint_id = malloc(strlen(id) + 1);
    check_not_null(checkpoint_id);
    strcpy(checkpoint_id, id);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    json_free(summary); json_free(state);
    summary = state = NULL;
    turbo_praktor_template_session_run_release(first);

    /* Exact checkpoint without an approved host receipt must never reach
     * Praktor. A second resume with a wrong attempt/approval identity is
     * equally forbidden, even though the compiled plan hash is unchanged. */
    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "first_attempt", "host_approval_1",
        checkpoint_id, 0, 0, &denied), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(denied, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(denied, &status), SALTS_OK);
    check_equal(status, TURBO_AGENT_EXECUTION_FAILED);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    turbo_praktor_template_session_run_release(denied);
    denied = NULL;

    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "forged_attempt", "host_approval_1",
        checkpoint_id, 0, 0, &denied), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(denied, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(denied, &status), SALTS_OK);
    check_equal(status, TURBO_AGENT_EXECUTION_FAILED);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    turbo_praktor_template_session_run_release(denied);
    denied = NULL;

    check_equal(turbo_praktor_template_approval_commit(
        turbo_agent_session_runtime(session), plan, "praktor_session_A",
        "first_attempt", "host_approval_1"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "first_attempt", "host_approval_1",
        checkpoint_id, 0, 0, &second), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(second, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(second, &status), SALTS_OK);
    check_equal(status, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(turbo_praktor_template_session_run_take_result(
        second, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(json_get_int(state, "praktor_outcome", -1),
                TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    json_free(summary); json_free(state);
    summary = state = NULL;
    turbo_praktor_template_session_run_release(second);

    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "first_attempt", "host_approval_1",
        checkpoint_id, 0, 0, &third), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(third, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_take_result(
        third, &summary, &state), SALTS_OK);
    check_true(json_get_bool(state, "praktor_replayed", false));
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    json_free(summary); json_free(state);
    turbo_praktor_template_session_run_release(third);

    free(checkpoint_id);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("denies wrong checkpoint thread and expired deadline without external effects") {
    session_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 2};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = session_registry(&probe);
    turbo_agent_session_t *session = new_session(
        registry, "praktor_session_B", turbo_agent_runtime_store_memory_create());
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = session_plan(registry, args);
    turbo_praktor_template_session_run_t *run = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    json_value_t *summary = NULL, *state = NULL;

    check_not_null(pool); check_not_null(session); check_not_null(plan);
    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "deadline", "host_approval_2",
        NULL, 1, 1, &run), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(run, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(run, &status), SALTS_OK);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    check_equal(turbo_praktor_template_session_run_take_result(
        run, &summary, &state), SALTS_OK);
    json_free(summary); json_free(state);
    turbo_praktor_template_session_run_release(run);

    run = NULL;
    check_not_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "wrong", "host_approval_2",
        "no_such_checkpoint", 0, 0, &run), SALTS_OK);
    check_null(run);
    check_equal(probe.inspect + probe.change + probe.verify, 0);

    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }
  it("propagates in-flight Session cancellation to a cooperative Praktor HostTool and drains") {
    session_probe_t probe = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 2};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = session_registry(&probe);
    turbo_agent_session_t *session = new_session(
        registry, "praktor_cancel_thread", turbo_agent_runtime_store_memory_create());
    json_value_t *args = json_create_object(), *summary = NULL, *state = NULL;
    turbo_agent_template_plan_t *plan = session_plan(registry, args);
    turbo_praktor_template_session_run_t *run = NULL;
    turbo_agent_execution_status_t exec_status;
    json_value_t *attempt = NULL;
    int entered;
    probe.wait_for_cancel = 1;
    atomic_init(&probe.change_entered, 0);
    atomic_init(&probe.cancel_observed, 0);
    check_not_null(pool);
    check_not_null(registry);
    check_not_null(session);
    check_not_null(plan);

    check_equal(turbo_praktor_template_approval_commit(
        turbo_agent_session_runtime(session), plan, "praktor_cancel_thread",
        "cancel_inflight", "host_review_cancel"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_session_submit(
        pool, session, plan, "cancel_inflight", "host_review_cancel",
        NULL, 0, 0, &run), SALTS_OK);
    check_not_null(run);
    {
      const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&probe.change_entered, memory_order_acquire) &&
             cmeta_monotonic_ms() < stop)
        cmeta_thread_yield();
    }
    entered = atomic_load_explicit(&probe.change_entered, memory_order_acquire);
    if (entered) {
      check_equal(turbo_praktor_template_session_run_cancel(
          run, TURBO_CANCEL_USER), SALTS_OK);
    }
    check_equal(turbo_praktor_template_session_run_wait(run, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(run, &exec_status), SALTS_OK);
    check_true(entered);
    check_true(atomic_load_explicit(&probe.cancel_observed, memory_order_acquire));
    check_true(exec_status != TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(probe.inspect, 1);
    check_equal(probe.change, 1);
    check_equal(probe.verify, 0);
    check_equal(turbo_agent_runtime_store_get_json_status(
        turbo_agent_session_runtime(session), "praktor_template_attempts",
        "021_praktor_cancel_thread_cancel_inflight", &attempt), SALTS_OK);
    check_equal(json_get_string(attempt, "phase"), "committed");
    json_free(attempt);
    /* Release after terminal/drain, not while provider callbacks are active. */
    (void)turbo_praktor_template_session_run_take_result(run, &summary, &state);
    json_free(summary);
    json_free(state);
    turbo_praktor_template_session_run_release(run);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
    cmeta_threadpool_destroy(pool);
  }

  it("pins the exact Salts ComponentPlugin generation across async cancellation and release") {
    session_probe_t probe = {0};
    salts_component_plugin_generation generation = SALTS_COMPONENT_PLUGIN_GENERATION_INIT;
    salts_component_plugin_generation_storage storage = {0};
    salts_component_plugin_runtime component_runtime =
        SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 2};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_tool_registry_t *registry = session_registry(&probe);
    turbo_agent_session_t *session = new_session(
        registry, "praktor_scoped_thread", turbo_agent_runtime_store_memory_create());
    json_value_t *args = json_create_object(), *summary = NULL, *state = NULL;
    turbo_agent_template_plan_t *plan = session_plan(registry, args);
    turbo_praktor_template_session_run_t *run = NULL;
    turbo_agent_execution_status_t status;
    int entered;

    probe.wait_for_cancel = 1;
    atomic_init(&probe.change_entered, 0);
    atomic_init(&probe.cancel_observed, 0);
    check_not_null(pool); check_not_null(registry);
    check_not_null(session); check_not_null(plan);

    /* A real Salts ComponentPlugin generation, with no dynamic DSO fixture.
     * It is an admission/lease witness, not a claim about a loaded module. */
    check_equal(salts_component_plugin_generation_build(
        &generation, UINT64_C(41), NULL, &storage,
        NULL, 0, NULL, 0, NULL, 0), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&component_runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &component_runtime, &generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(turbo_praktor_template_approval_commit(
        turbo_agent_session_runtime(session), plan, "praktor_scoped_thread",
        "scoped_cancel", "host_scoped"), TURBO_TOOL_OK);

    /* A stale compiled generation cannot borrow the new published one. */
    check_equal(turbo_praktor_template_session_submit_scoped(
        pool, session, plan, &component_runtime, UINT64_C(40),
        "scoped_cancel", "host_scoped", NULL, 0, 0, &run), SALTS_EPROTO);
    check_null(run);

    check_equal(turbo_praktor_template_session_submit_scoped(
        pool, session, plan, &component_runtime, UINT64_C(41),
        "scoped_cancel", "host_scoped", NULL, 0, 0, &run), SALTS_OK);
    check_not_null(run);
    {
      const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&probe.change_entered, memory_order_acquire) &&
             cmeta_monotonic_ms() < stop) cmeta_thread_yield();
    }
    entered = atomic_load_explicit(&probe.change_entered, memory_order_acquire);
    check_equal(salts_component_plugin_runtime_close(
        &component_runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generation);
    check_equal(salts_component_plugin_generation_drain(
        &component_runtime, &generation), SALTS_COMPONENT_PLUGIN_BUSY);
    if (entered) check_equal(turbo_praktor_template_session_run_cancel(
        run, TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(run, UINT64_MAX), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(run, &status), SALTS_OK);
    check_true(entered);
    check_true(atomic_load_explicit(&probe.cancel_observed, memory_order_acquire));
    check_true(status != TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(probe.change, 1);
    check_equal(probe.verify, 0);
    /* Even when the callback finished, an outstanding caller-owned run
     * still protects the generation from draining. */
    check_equal(salts_component_plugin_generation_drain(
        &component_runtime, &generation), SALTS_COMPONENT_PLUGIN_BUSY);

    (void)turbo_praktor_template_session_run_take_result(run, &summary, &state);
    json_free(summary); json_free(state);
    turbo_praktor_template_session_run_release(run);
    run = NULL;
    /* Worker completion callback releases its own run reference.
     * Joining the bounded executor proves callback quiescence. */
    cmeta_threadpool_destroy(pool); pool = NULL;
    check_equal(salts_component_plugin_generation_drain(
        &component_runtime, &generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(&component_runtime),
                SALTS_COMPONENT_PLUGIN_OK);

    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_session_destroy(session);
    turbo_tool_registry_destroy(registry);
  }

}
