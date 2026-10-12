#include "tinytest.h"
#include "turbo_praktor_approved_template.h"
#include "turbo_runtime_json.h"
#include "praktor_dso_fixture.h"

#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#ifndef PRAKTOR_DSO_N_PATH
#error "real Provider DSO paths must be provided by CMake"
#endif
#ifndef PRAKTOR_DSO_N1_PATH
#error "two distinct Provider DSO paths must be provided by CMake"
#endif

/* Each generation bundle and its borrowed arrays remain at a fixed address
 * until the Salts runtime reports that generation fully drained. */
typedef struct dso_generation {
  salts_component_plugin_generation generation;
  salts_component_deployment deployments[1];
  salts_component_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t order[1];
  salts_component_plugin_module modules[1];
} dso_generation;

typedef struct dso_probe {
  praktor_dso_gate gate;  /* borrowed from a published Provider generation */
  praktor_dso_control control;
  int expected_epoch;
  atomic_int inspect_calls;
  atomic_int change_calls;
  atomic_int verify_calls;
} dso_probe;

static salts_component_plugin_status dso_build(
    dso_generation *fixture, uint64_t id,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  const salts_component_plugin_generation_storage storage = {
    fixture->deployments, 1u,
    fixture->instances, 1u,
    fixture->dependencies, 1u,
    fixture->order, 1u,
    fixture->modules, 1u
  };
  const salts_component_plugin_source source = {
    ref, PRAKTOR_DSO_PROVIDER_EXPORT_ID, NULL, NULL
  };
  return salts_component_plugin_generation_build(
      &fixture->generation, id, registry, &storage,
      NULL, 0u, &source, 1u, NULL, 0u);
}

static int dso_borrow_gate(
    salts_component_plugin_runtime *runtime, uint64_t expected,
    praktor_dso_gate *out) {
  salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
  salts_component_service service = {0};
  int ok = 0;
  if (salts_component_plugin_scope_acquire(&*runtime, &scope) !=
      SALTS_COMPONENT_PLUGIN_OK)
    return 0;
  if (salts_component_plugin_scope_generation_id(&scope) == expected &&
      salts_component_plugin_scope_find_service(
          &scope, praktor_dso_gate_interface(), &service) ==
          SALTS_COMPONENT_PLUGIN_OK &&
      praktor_dso_gate_borrow_from_object(
          service.object, service.interfaces, out) == CMETA_OK)
    ok = 1;
  if (salts_component_plugin_scope_release(&scope) !=
      SALTS_COMPONENT_PLUGIN_OK)
    return 0;
  /* out is BORROWED; only call it while the session's generation Scope
   * remains live. The temporary service-lookup scope owns no work. */
  return ok;
}

static int no_model(const char *request, char **response, void *user) {
  (void)request; (void)response; (void)user;
  return -1;
}

static int dso_cancelled(void *host) {
  const turbo_tool_execution_context_t *ctx =
      (const turbo_tool_execution_context_t *)host;
  return ctx && ctx->cancel_token &&
      turbo_cancel_token_check(ctx->cancel_token) == SALTS_ECANCELED;
}
static uint64_t dso_now(void *host) {
  (void)host;
  return cmeta_monotonic_ms();
}
static void dso_yield(void *host) {
  (void)host;
  cmeta_thread_yield();
}

static turbo_tool_status_t dso_inspect(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  dso_probe *probe = (dso_probe *)user;
  (void)args; (void)ctx;
  atomic_fetch_add_explicit(&probe->inspect_calls, 1, memory_order_relaxed);
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t dso_change(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  dso_probe *probe = (dso_probe *)user;
  int epoch;
  (void)args;
  atomic_fetch_add_explicit(&probe->change_calls, 1, memory_order_relaxed);
  /* The host's actual Session cancellation token is borrowed solely for
   * this synchronous callback. The DSO blocks INSIDE its run method. */
  probe->control.host = (void *)ctx;
  epoch = praktor_dso_gate_run(&probe->gate, &probe->control);
  if (probe->control.block_until_cancel &&
      epoch == -probe->expected_epoch && dso_cancelled((void *)ctx))
    return TURBO_TOOL_CANCELLED;
  if (epoch != probe->expected_epoch)
    return TURBO_TOOL_ERROR;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t dso_verify(
    const json_value_t *args, const turbo_tool_execution_context_t *ctx,
    json_value_t **out, void *user) {
  dso_probe *probe = (dso_probe *)user;
  (void)args; (void)ctx;
  atomic_fetch_add_explicit(&probe->verify_calls, 1, memory_order_relaxed);
  *out = json_create_object();
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  return turbo_runtime_json_object_set(
      *out, "verified", json_create_bool(1)) == TURBO_RUNTIME_JSON_OK
      ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static int dso_register(
    turbo_tool_registry_t *registry, const char *name,
    turbo_tool_idempotency_t idempotency,
    turbo_tool_effect_flags_t effects,
    turbo_tool_json_value_context_handler_fn callback, dso_probe *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = name;
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1;
  definition.json_value_context_handler = callback;
  definition.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &definition) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *dso_registry(dso_probe *probe) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (!dso_register(registry, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                    TURBO_TOOL_EFFECT_READ, dso_inspect, probe) ||
      !dso_register(registry, "repo.change", TURBO_TOOL_IDEMPOTENCY_NONE,
                    TURBO_TOOL_EFFECT_WRITE, dso_change, probe) ||
      !dso_register(registry, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                    TURBO_TOOL_EFFECT_READ, dso_verify, probe)) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static turbo_agent_template_plan_t *dso_plan(
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
      &compiler, registry, &source, &plan, NULL) == TURBO_AGENT_COMPILE_OK
      ? plan : NULL;
}

static turbo_agent_session_t *dso_session(
    turbo_tool_registry_t *registry, const char *thread) {
  turbo_agent_session_config_t config = {0};
  config.runtime_store = turbo_agent_runtime_store_memory_create();
  config.thread_id = thread;
  config.agent_config.model = "test";
  config.agent_config.transport_fn = no_model;
  config.agent_config.tool_registry = registry;
  return turbo_agent_session_create(&config);
}

static void dso_probe_init(dso_probe *probe, int epoch, int block) {
  memset(probe, 0, sizeof(*probe));
  probe->expected_epoch = epoch;
  probe->control.block_until_cancel = block;
  probe->control.cancelled = dso_cancelled;
  probe->control.now_ms = dso_now;
  probe->control.yield = dso_yield;
  atomic_init(&probe->control.entered, 0);
  atomic_init(&probe->control.exited, 0);
  atomic_init(&probe->inspect_calls, 0);
  atomic_init(&probe->change_calls, 0);
  atomic_init(&probe->verify_calls, 0);
}

spec("real Provider DSO generation N to N+1 hot replacement") {
  it("refuses dlclose while old HostTool runs and unloads old image only after drain") {
    const cmeta_plugin_registry_config plugin_config = {2};
    cmeta_plugin_registry plugin_registry = {0};
    cmeta_plugin_ref old_ref = {0}, new_ref = {0};
    cmeta_plugin_lifecycle_info info;
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    dso_generation old_generation = {0}, new_generation = {0};
    dso_probe old_probe, new_probe;
    turbo_tool_registry_t *old_tools = NULL, *new_tools = NULL;
    turbo_agent_session_t *old_session = NULL, *new_session = NULL;
    turbo_agent_template_plan_t *old_plan = NULL, *new_plan = NULL;
    turbo_praktor_template_session_run_t *old_run = NULL, *new_run = NULL;
    turbo_praktor_template_session_run_t *stale_run = NULL;
    cmeta_threadpool_config_t pool_config = {
      .num_threads = 1, .queue_capacity = 2
    };
    cmeta_threadpool_t *old_pool = NULL, *new_pool = NULL;
    turbo_agent_execution_status_t old_status, new_status;
    json_value_t *args = NULL, *old_attempt = NULL;
    int old_entered;
    bool quiescent = false;

    dso_probe_init(&old_probe, 41, 1);
    dso_probe_init(&new_probe, 42, 0);
    check_equal(cmeta_plugin_registry_init(&plugin_registry, &plugin_config),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &plugin_registry, PRAKTOR_DSO_N_PATH, &old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &plugin_registry, PRAKTOR_DSO_N1_PATH, &new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&plugin_registry, old_ref),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&plugin_registry, new_ref),
                CMETA_PLUGIN_OK);
    check_equal(dso_build(&old_generation, UINT64_C(41),
                          &plugin_registry, old_ref), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(dso_build(&new_generation, UINT64_C(42),
                          &plugin_registry, new_ref), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &old_generation.generation, &retired),
        SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_true(dso_borrow_gate(&runtime, UINT64_C(41), &old_probe.gate));

    old_tools = dso_registry(&old_probe);
    old_session = dso_session(old_tools, "dso_generation_N");
    old_pool = cmeta_threadpool_create_with_config(&pool_config);
    args = json_create_object();
    old_plan = dso_plan(old_tools, args);
    check_not_null(old_tools); check_not_null(old_session);
    check_not_null(old_pool); check_not_null(old_plan);
    check_equal(turbo_praktor_template_approval_commit(
        turbo_agent_session_runtime(old_session), old_plan,
        "dso_generation_N", "attempt_N", "approval_N"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_session_submit_scoped(
        old_pool, old_session, old_plan, &runtime, UINT64_C(41),
        "attempt_N", "approval_N", NULL, 0, 0, &old_run), SALTS_OK);
    check_not_null(old_run);
    {
      const uint64_t stop = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&old_probe.control.entered,
                                   memory_order_acquire) &&
             cmeta_monotonic_ms() < stop)
        cmeta_thread_yield();
    }
    old_entered = atomic_load_explicit(
        &old_probe.control.entered, memory_order_acquire);
    check_true(old_entered);

    /* Publish new executable Provider while the old DSO is executing. */
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &new_generation.generation, &retired),
        SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &old_generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &old_generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_request_stop(
        &plugin_registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_unload(
        &plugin_registry, old_ref), CMETA_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &plugin_registry, old_ref, &info), CMETA_PLUGIN_OK);
    check_true(info.active_leases > 0u);

    /* The old compiled projection must NOT be accidentally rebound to N+1. */
    check_equal(turbo_praktor_template_session_submit_scoped(
        old_pool, old_session, old_plan, &runtime, UINT64_C(41),
        "attempt_stale", "approval_stale", NULL, 0, 0, &stale_run),
        SALTS_EPROTO);
    check_null(stale_run);
    check_equal(atomic_load_explicit(&old_probe.change_calls,
                                     memory_order_acquire), 1);

    /* A newly compiled N+1 projection runs normally while N is still busy. */
    check_true(dso_borrow_gate(&runtime, UINT64_C(42), &new_probe.gate));
    new_tools = dso_registry(&new_probe);
    new_session = dso_session(new_tools, "dso_generation_N1");
    new_pool = cmeta_threadpool_create_with_config(&pool_config);
    new_plan = dso_plan(new_tools, args);
    check_not_null(new_tools); check_not_null(new_session);
    check_not_null(new_pool); check_not_null(new_plan);
    check_equal(turbo_praktor_template_approval_commit(
        turbo_agent_session_runtime(new_session), new_plan,
        "dso_generation_N1", "attempt_N1", "approval_N1"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_session_submit_scoped(
        new_pool, new_session, new_plan, &runtime, UINT64_C(42),
        "attempt_N1", "approval_N1", NULL, 0, 0, &new_run), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(
        new_run, UINT64_C(10000)), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(
        new_run, &new_status), SALTS_OK);
    check_equal(new_status, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(atomic_load_explicit(&new_probe.change_calls,
                                     memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&new_probe.verify_calls,
                                     memory_order_acquire), 1);
    check_true(atomic_load_explicit(
        &new_probe.control.exited, memory_order_acquire));
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &old_generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);

    /* Cancel from the real Session; the old DSO sees its borrowed token. */
    if (old_entered)
      check_equal(turbo_praktor_template_session_run_cancel(
          old_run, TURBO_CANCEL_USER), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_wait(
        old_run, UINT64_C(10000)), SALTS_OK);
    check_equal(turbo_praktor_template_session_run_status(
        old_run, &old_status), SALTS_OK);
    check_true(old_status != TURBO_AGENT_EXECUTION_COMPLETED);
    check_true(atomic_load_explicit(
        &old_probe.control.exited, memory_order_acquire));
    check_equal(atomic_load_explicit(&old_probe.change_calls,
                                     memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&old_probe.verify_calls,
                                     memory_order_acquire), 0);
    check_equal(turbo_agent_runtime_store_get_json_status(
        turbo_agent_session_runtime(old_session), "praktor_template_attempts",
        "021_dso_generation_N_attempt_N", &old_attempt), SALTS_OK);
    check_equal(json_get_string(old_attempt, "phase"), "committed");
    json_free(old_attempt);

    /* Terminal completion is not release: the caller-held run still pins N. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &old_generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    turbo_praktor_template_session_run_release(old_run);
    old_run = NULL;
    cmeta_threadpool_destroy(old_pool); old_pool = NULL;
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &old_generation.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &plugin_registry, old_ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);
    /* Registry unload is the actual OS dlclose/FreeLibrary, not a mocked
     * callback. The removed N module must never be called again. */
    check_equal(cmeta_plugin_registry_unload(
        &plugin_registry, old_ref), CMETA_PLUGIN_OK);

    /* N+1 remains published independently of N's module lifetime. */
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &new_generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &new_generation.generation), SALTS_COMPONENT_PLUGIN_BUSY);
    turbo_praktor_template_session_run_release(new_run);
    new_run = NULL;
    cmeta_threadpool_destroy(new_pool); new_pool = NULL;
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &new_generation.generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_destroy(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &plugin_registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &plugin_registry, new_ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);
    check_equal(cmeta_plugin_registry_unload(
        &plugin_registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_destroy(&plugin_registry),
                CMETA_PLUGIN_OK);

    turbo_agent_template_plan_destroy(old_plan);
    turbo_agent_template_plan_destroy(new_plan);
    json_free(args);
    turbo_agent_session_destroy(old_session);
    turbo_agent_session_destroy(new_session);
    turbo_tool_registry_destroy(old_tools);
    turbo_tool_registry_destroy(new_tools);
  }
}
