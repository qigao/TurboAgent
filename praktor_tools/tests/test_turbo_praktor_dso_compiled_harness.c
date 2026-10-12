/* Linux test-only contract: a real Managed Provider DSO must remain loaded
 * from compiled Harness enqueue until the Host's final source/plan/Scope drain.
 * No implementation or publication ABI changes, and no historical token replay.
 */
#include "tinytest.h"
#include "praktor_dso_fixture.h"
#include "praktor_dso_managed_probe.h"
#include "turbo_agent_harness.h"
#include "turbo_tool_registry.h"
#include "turbo_runtime_json.h"

#include <salts/component_plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>
#include <salts/plugin.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PRAKTOR_DSO_ASYNC_MANAGED_PATH
#error "A real Managed ComponentProvider DSO is required"
#endif

enum { DSO_COMPILED_EPOCH = 45 };
static const char dso_compiled_source[] =
    "{\"template_id\":\"inspect\",\"step_id\":\"read-1\","
    "\"tool\":\"repo_inspect\","
    "\"arguments_json\":\"{\\\"value\\\":\\\"hello\\\"}\"}";

typedef struct dso_compiled_host {
  cmeta_plugin_registry registry;
  cmeta_plugin_ref ref;
  salts_component_plugin_runtime runtime;
  salts_component_plugin_generation generation;
  salts_component_plugin_scope scope;
  salts_component_deployment deployments[1];
  salts_component_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t order[1];
  salts_component_plugin_module modules[1];
  praktor_dso_gate borrowed_gate;
  praktor_dso_control control;
  atomic_int destroy_calls;
  atomic_int tool_calls;
  atomic_int source_owner_releases;
} dso_compiled_host;

typedef struct dso_compiled_queue_gate {
  atomic_int entered;
  atomic_int open;
} dso_compiled_queue_gate;

static void parked_worker(void *context) {
  dso_compiled_queue_gate *gate = (dso_compiled_queue_gate *)context;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire))
    cmeta_thread_yield();
}

static void wait_gate_parked(dso_compiled_queue_gate *gate) {
  const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
  while (!atomic_load_explicit(&gate->entered, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  check_true(atomic_load_explicit(&gate->entered, memory_order_acquire));
}

static uint64_t dso_host_now(void *context) {
  (void)context;
  return cmeta_monotonic_ms();
}

static void dso_host_yield(void *context) {
  (void)context;
  cmeta_thread_yield();
}

static int dso_host_never_cancel(void *context) {
  (void)context;
  return 0;
}

/* The callback code is Host-owned but calls the real DSO-owned interface
 * vtable. The Host must keep that DSO pinned across queue latency and invoke. */
static int dso_compiled_callback(
    const json_value_t *arguments, json_value_t **out_result, void *user_data) {
  dso_compiled_host *host = (dso_compiled_host *)user_data;
  json_value_t *result;
  if (!host || !arguments || !out_result ||
      !json_get_string(arguments, "value"))
    return -1;
  atomic_fetch_add_explicit(&host->tool_calls, 1, memory_order_acq_rel);
  if (praktor_dso_gate_run(&host->borrowed_gate, &host->control) !=
      DSO_COMPILED_EPOCH)
    return -1;
  result = json_create_object();
  if (!result || turbo_runtime_json_object_set(
          result, "ok", json_create_bool(1)) != TURBO_RUNTIME_JSON_OK) {
    json_free(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static void dso_source_callback_cleanup(void *user_data) {
  dso_compiled_host *host = (dso_compiled_host *)user_data;
  if (host)
    atomic_fetch_add_explicit(&host->source_owner_releases, 1,
                              memory_order_acq_rel);
}

static int dso_compiled_no_model(
    const char *request, char **response, void *user_data) {
  (void)request;
  (void)response;
  (void)user_data;
  return -1;
}

static turbo_tool_registry_t *dso_create_tool_registry(dso_compiled_host *host) {
  static const char *const capabilities[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry)
    return NULL;
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = "repo.inspect";
  definition.definition.description = "Read-only Managed DSO gate";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},"
      "\"required\":[\"value\"],\"additionalProperties\":false}";
  definition.definition.json_value_handler = dso_compiled_callback;
  definition.definition.user_data = host;
  definition.definition.user_data_free = dso_source_callback_cleanup;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = TURBO_TOOL_IDEMPOTENCY_READ_ONLY;
  definition.required_capabilities = capabilities;
  definition.required_capability_count = 1;
  definition.result_schema_json =
      "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},"
      "\"required\":[\"ok\"],\"additionalProperties\":false}";
  definition.strict_result = 1;
  definition.effect_flags = TURBO_TOOL_EFFECT_READ;
  if (turbo_tool_registry_add_v6(registry, &definition) != TURBO_TOOL_OK) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static turbo_agent_harness_t *dso_create_harness(
    cmeta_threadpool_t *executor, turbo_tool_registry_t *registry,
    const char *thread_id) {
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t config;
  session.runtime_store = turbo_agent_runtime_store_memory_create();
  session.thread_id = thread_id;
  session.agent_config.model = "test";
  session.agent_config.transport_fn = dso_compiled_no_model;
  session.agent_config.tool_registry = registry;
  app.session_config = &session;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app;
  config.executor = executor;
  return turbo_agent_harness_create(&config);
}

static void dso_host_initialize(dso_compiled_host *host) {
  const cmeta_plugin_registry_config config = {1u};
  const salts_component_plugin_generation_storage storage = {
    host->deployments, 1u,
    host->instances, 1u,
    host->dependencies, 1u,
    host->order, 1u,
    host->modules, 1u
  };
  const cmeta_plugin_manifest *manifest = NULL;
  cmeta_plugin_lease inspection_lease = {0};
  salts_component_plugin_source source;
  salts_component_plugin_generation *retired = NULL;
  salts_component_service service = {0};

  atomic_init(&host->destroy_calls, 0);
  atomic_init(&host->tool_calls, 0);
  atomic_init(&host->source_owner_releases, 0);
  atomic_init(&host->control.entered, 0);
  atomic_init(&host->control.exited, 0);
  host->control.block_until_cancel = 0;
  host->control.now_ms = dso_host_now;
  host->control.yield = dso_host_yield;
  host->control.cancelled = dso_host_never_cancel;

  check_equal(cmeta_plugin_registry_init(
      &host->registry, &config), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_load(
      &host->registry, PRAKTOR_DSO_ASYNC_MANAGED_PATH,
      &host->ref), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_start(
      &host->registry, host->ref), CMETA_PLUGIN_OK);

  /* Attach the Host destroy-once counter to real Managed DSO state while
   * holding an authoritative inspection lease. */
  check_equal(cmeta_plugin_registry_acquire(
      &host->registry, host->ref, &inspection_lease, &manifest),
      CMETA_PLUGIN_OK);
  check_not_null(manifest);
  check_not_null(manifest->self);
  ((praktor_dso_managed_probe *)manifest->self)->host_destroy_count =
      &host->destroy_calls;
  check_equal(cmeta_plugin_registry_release(
      &host->registry, &inspection_lease), CMETA_PLUGIN_OK);

  source = (salts_component_plugin_source){
    host->ref, PRAKTOR_DSO_PROVIDER_EXPORT_ID, NULL, NULL
  };
  check_equal(salts_component_plugin_generation_build(
      &host->generation, UINT64_C(45), &host->registry,
      &storage, NULL, 0u, &source, 1u, NULL, 0u),
      SALTS_COMPONENT_PLUGIN_OK);
  check_equal(salts_component_plugin_runtime_init(
      &host->runtime), SALTS_COMPONENT_PLUGIN_OK);
  check_equal(salts_component_plugin_runtime_publish(
      &host->runtime, &host->generation, &retired),
      SALTS_COMPONENT_PLUGIN_OK);
  check_null(retired);
  check_equal(salts_component_plugin_scope_acquire(
      &host->runtime, &host->scope), SALTS_COMPONENT_PLUGIN_OK);
  check_equal(salts_component_plugin_scope_find_service(
      &host->scope, praktor_dso_gate_interface(), &service),
      SALTS_COMPONENT_PLUGIN_OK);
  check_equal(praktor_dso_gate_borrow_from_object(
      service.object, service.interfaces, &host->borrowed_gate), CMETA_OK);
}

static void dso_host_final_release(dso_compiled_host *host) {
  bool quiet = false;
  check_equal(salts_component_plugin_generation_drain(
      &host->runtime, &host->generation), SALTS_COMPONENT_PLUGIN_BUSY);
  check_equal(atomic_load_explicit(
      &host->destroy_calls, memory_order_acquire), 0);
  check_equal(salts_component_plugin_scope_release(
      &host->scope), SALTS_COMPONENT_PLUGIN_OK);
  check_equal(salts_component_plugin_generation_drain(
      &host->runtime, &host->generation), SALTS_COMPONENT_PLUGIN_OK);
  check_equal(salts_component_plugin_runtime_destroy(
      &host->runtime), SALTS_COMPONENT_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_poll_quiescent(
      &host->registry, host->ref, &quiet), CMETA_PLUGIN_OK);
  check_true(quiet);
  check_equal(cmeta_plugin_registry_unload(
      &host->registry, host->ref), CMETA_PLUGIN_OK);
  /* Success reaches the genuine installed Salts POSIX native dlclose. */
  check_equal(atomic_load_explicit(
      &host->destroy_calls, memory_order_acquire), 1);
  check_equal(cmeta_plugin_registry_count(&host->registry), (size_t)0u);
  check_equal(cmeta_plugin_registry_destroy(&host->registry), CMETA_PLUGIN_OK);
  check_null(host->registry.impl);
  check_equal(atomic_load_explicit(
      &host->destroy_calls, memory_order_acquire), 1);
}

static void dso_run_queued_scenario(int cancel_before_dequeue) {
  dso_compiled_host host = {0};
  dso_compiled_queue_gate gate = {0};
  const char *allowed[] = {"runtime_tools"};
  const cmeta_threadpool_config_t pool_config = {
    .num_threads = 1, .queue_capacity = 1
  };
  cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
  turbo_tool_registry_t *tools;
  turbo_agent_harness_t *harness;
  turbo_agent_harness_execution_t *execution = NULL;
  turbo_agent_compiler_config_t compiler;
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  salts_component_plugin_generation *retired = NULL;
  json_value_t *summary = NULL, *state = NULL;
  bool quiet = true;

  check_not_null(pool);
  dso_host_initialize(&host);
  tools = dso_create_tool_registry(&host);
  check_not_null(tools);
  harness = dso_create_harness(
      pool, tools, cancel_before_dequeue
          ? "compiled-dso-cancel" : "compiled-dso-complete");
  check_not_null(harness);
  atomic_init(&gate.entered, 0);
  atomic_init(&gate.open, 0);
  check_equal(cmeta_threadpool_try_submit(
      pool, parked_worker, &gate), SALTS_OK);
  wait_gate_parked(&gate);

  turbo_agent_compiler_config_init(&compiler);
  compiler.allowed_capabilities = allowed;
  compiler.allowed_capability_count = 1;
  check_equal(turbo_agent_harness_submit_compiled_inspect_graph_json(
      harness, &compiler, dso_compiled_source, strlen(dso_compiled_source),
      cancel_before_dequeue ? "dso_cancel" : "dso_queue", NULL, 0,
      NULL, &execution), SALTS_OK);
  check_not_null(execution);
  check_equal(turbo_agent_harness_execution_get_status(
      execution, &status), SALTS_OK);
  check_equal(status, TURBO_AGENT_EXECUTION_QUEUED);

  /* A queued future can reach DSO vtable bytes, so the Host's previously
   * acquired Generation scope and module Lease must prevent any early close. */
  check_equal(cmeta_plugin_registry_destroy(
      &host.registry), CMETA_PLUGIN_BUSY);
  check_equal(cmeta_plugin_registry_request_stop(
      &host.registry, host.ref), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_poll_quiescent(
      &host.registry, host.ref, &quiet), CMETA_PLUGIN_OK);
  check_false(quiet);
  check_equal(cmeta_plugin_registry_unload(
      &host.registry, host.ref), CMETA_PLUGIN_BUSY);
  check_equal(atomic_load_explicit(
      &host.destroy_calls, memory_order_acquire), 0);
  check_equal(salts_component_plugin_runtime_close(
      &host.runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
  check_true(retired == &host.generation);
  check_equal(salts_component_plugin_generation_drain(
      &host.runtime, &host.generation), SALTS_COMPONENT_PLUGIN_BUSY);

  if (cancel_before_dequeue)
    check_equal(turbo_agent_harness_execution_cancel(
        execution, TURBO_CANCEL_USER), SALTS_OK);
  turbo_agent_harness_release(harness);
  atomic_store_explicit(&gate.open, 1, memory_order_release);
  check_equal(turbo_agent_harness_execution_wait(
      execution, UINT64_MAX), SALTS_OK);
  check_equal(turbo_agent_harness_execution_get_status(
      execution, &status), SALTS_OK);
  check_equal(status, cancel_before_dequeue
      ? TURBO_AGENT_EXECUTION_CANCELLED : TURBO_AGENT_EXECUTION_COMPLETED);
  check_equal(atomic_load_explicit(
      &host.tool_calls, memory_order_acquire),
      cancel_before_dequeue ? 0 : 1);
  check_equal(atomic_load_explicit(
      &host.control.entered, memory_order_acquire),
      cancel_before_dequeue ? 0 : 1);
  if (!cancel_before_dequeue) {
    check_equal(turbo_agent_harness_execution_take_result(
        execution, &summary, &state), SALTS_OK);
    check_equal(json_get_string(summary, "status"), "completed");
    check_true(json_get_bool(
        json_object_get(state, "compiled_result"), "ok", false));
    json_free(summary);
    json_free(state);
  }
  turbo_agent_harness_execution_release(execution);
  check_equal(atomic_load_explicit(
      &host.source_owner_releases, memory_order_acquire), 0);
  turbo_tool_registry_destroy(tools);
  check_equal(atomic_load_explicit(
      &host.source_owner_releases, memory_order_acquire), 1);
  dso_host_final_release(&host);
  cmeta_threadpool_wait(pool);
  cmeta_threadpool_destroy(pool);
}

spec("real Managed DSO pinned across queued compiled Harness lifetime") {
  it("executes the queued DSO callback once, refuses early unload, then dlcloses") {
    dso_run_queued_scenario(0);
  }
  it("cancels before dequeue with zero DSO callbacks, then drains and dlcloses") {
    dso_run_queued_scenario(1);
  }
}
