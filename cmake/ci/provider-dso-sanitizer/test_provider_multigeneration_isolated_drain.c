/* Simultaneous retirement of two independently loaded Managed Provider DSOs.
 * Exact-ref lease-release fault affects only Generation 43; Generation 44
 * still drains, destroys, and performs real dlclose before Host recovery.
 * All state ownership and retry decisions belong to the test Host.
 */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_fixture.h"
#include "praktor_dso_managed_probe.h"

#include <salts/component_plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdint.h>

#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_PATH
#error "Missing Managed Component Provider 43 path"
#endif
#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
#error "Missing Managed Component Provider 44 path"
#endif

typedef struct managed_generation {
  salts_component_plugin_generation generation;
  salts_component_deployment deployments[1];
  salts_component_instance instances[1];
  salts_component_dependency dependencies[1];
  size_t order[1];
  salts_component_plugin_module modules[1];
} managed_generation;

typedef struct managed_drain_gate {
  salts_component_plugin_runtime *runtime;
  atomic_int ready;
  atomic_int go;
} managed_drain_gate;

typedef struct managed_drain_worker {
  managed_drain_gate *gate;
  salts_component_plugin_generation *generation;
  salts_component_plugin_status result;
} managed_drain_worker;

static void managed_drain_concurrent(void *data) {
  managed_drain_worker *worker = (managed_drain_worker *)data;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->result = salts_component_plugin_generation_drain(
      worker->gate->runtime, worker->generation);
}

static salts_component_plugin_status managed_build(
    managed_generation *item, uint64_t epoch,
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  const salts_component_plugin_generation_storage storage = {
    item->deployments, 1u, item->instances, 1u, item->dependencies, 1u,
    item->order, 1u, item->modules, 1u
  };
  const salts_component_plugin_source source = {
    ref, PRAKTOR_DSO_PROVIDER_EXPORT_ID, NULL, NULL
  };
  return salts_component_plugin_generation_build(
      &item->generation, epoch, registry, &storage,
      NULL, 0u, &source, 1u, NULL, 0u);
}

static cmeta_plugin_status managed_set_host_destroy_counter(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref,
    atomic_int *counter) {
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  cmeta_plugin_status status = cmeta_plugin_registry_acquire(
      registry, ref, &lease, &manifest);
  if (status != CMETA_PLUGIN_OK) return status;
  if (!manifest || !manifest->self) {
    (void)cmeta_plugin_registry_release(registry, &lease);
    return CMETA_PLUGIN_INVALID_MANIFEST;
  }
  ((praktor_dso_managed_probe *)manifest->self)->host_destroy_count = counter;
  return cmeta_plugin_registry_release(registry, &lease);
}

static int managed_run_scoped(
    salts_component_plugin_scope *scope, atomic_int *entered,
    atomic_int *exited) {
  salts_component_service service = {0};
  praktor_dso_gate gate = {0};
  praktor_dso_control control = {0};
  if (salts_component_plugin_scope_find_service(
          scope, praktor_dso_gate_interface(), &service)
        != SALTS_COMPONENT_PLUGIN_OK)
    return -10001;
  if (praktor_dso_gate_borrow_from_object(
          service.object, service.interfaces, &gate) != CMETA_OK)
    return -10002;
  atomic_init(&control.entered, 0);
  atomic_init(&control.exited, 0);
  control.block_until_cancel = 0;
  const int result = praktor_dso_gate_run(&gate, &control);
  atomic_store_explicit(
      entered, atomic_load_explicit(&control.entered, memory_order_acquire),
      memory_order_release);
  atomic_store_explicit(
      exited, atomic_load_explicit(&control.exited, memory_order_acquire),
      memory_order_release);
  return result;
}

spec("independent Managed DSO Generation retirement under scoped release failure") {
  it("allows healthy N+1 drain/unload while N retains failed lease") {
    const cmeta_plugin_registry_config config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref refs[2] = {{0}};
    managed_generation generations[2] = {{0}};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope scopes[2] = {
      SALTS_COMPONENT_PLUGIN_SCOPE_INIT,
      SALTS_COMPONENT_PLUGIN_SCOPE_INIT
    };
    cmeta_plugin_lifecycle_info info = {0};
    const salts_component_plugin_failure *failure = NULL;
    managed_drain_gate drain_gate = {0};
    managed_drain_worker workers[2] = {{0}};
    cmeta_thread_t threads[2] = {0};
    atomic_int destroyed[2], entered[2], exited[2];
    bool quiet = false;
    int ready = 0;
    const int injected_before = provider_release_fault_injected();
    const int native_release_before = provider_release_fault_native_attempts();
    const int close_injected_before = provider_close_fault_injected();
    const int native_close_before = provider_close_fault_native_attempts();
    const int native_success_before = provider_close_fault_native_successes();

    for (size_t i = 0u; i < 2u; ++i) {
      atomic_init(&destroyed[i], 0);
      atomic_init(&entered[i], 0);
      atomic_init(&exited[i], 0);
    }
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &refs[0]),
        CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH, &refs[1]),
        CMETA_PLUGIN_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(cmeta_plugin_registry_start(
          &registry, refs[i]), CMETA_PLUGIN_OK);
      check_equal(managed_set_host_destroy_counter(
          &registry, refs[i], &destroyed[i]), CMETA_PLUGIN_OK);
      check_equal(managed_build(
          &generations[i], (uint64_t)(43u + i), &registry, refs[i]),
          SALTS_COMPONENT_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, refs[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)1u);
    }

    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generations[0].generation, &retired),
        SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scopes[0]), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(managed_run_scoped(
        &scopes[0], &entered[0], &exited[0]), 43);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generations[1].generation, &retired),
        SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generations[0].generation);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scopes[1]), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(managed_run_scoped(
        &scopes[1], &entered[1], &exited[1]), 44);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generations[1].generation);

    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(atomic_load_explicit(
          &entered[i], memory_order_acquire), 1);
      check_equal(atomic_load_explicit(
          &exited[i], memory_order_acquire), 1);
      check_equal(salts_component_plugin_generation_drain(
          &runtime, &generations[i].generation), SALTS_COMPONENT_PLUGIN_BUSY);
      check_equal(salts_component_plugin_scope_release(
          &scopes[i]), SALTS_COMPONENT_PLUGIN_OK);
    }

    drain_gate.runtime = &runtime;
    atomic_init(&drain_gate.ready, 0);
    atomic_init(&drain_gate.go, 0);
    for (size_t i = 0u; i < 2u; ++i) {
      workers[i].gate = &drain_gate;
      workers[i].generation = &generations[i].generation;
      check_equal(cmeta_thread_create(
          &threads[i], managed_drain_concurrent, &workers[i]), SALTS_OK);
    }
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (atomic_load_explicit(
                 &drain_gate.ready, memory_order_acquire) < 2 &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&drain_gate.ready, memory_order_acquire);
    /* Only release the lease of Plugin 43 fails. Concurrent Plugin 44
     * operations must remain owned by and succeed inside their own DSO. */
    provider_release_fault_arm_plugin(refs[0], 1);
    atomic_store_explicit(&drain_gate.go, 1, memory_order_release);
    for (size_t i = 0u; i < 2u; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    check_equal(ready, 2);
    check_equal(workers[0].result, SALTS_COMPONENT_PLUGIN_PLUGIN_ERROR);
    check_equal(workers[1].result, SALTS_COMPONENT_PLUGIN_OK);
    check_equal(provider_release_fault_injected() - injected_before, 1);

    check_equal(generations[0].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_FAILED);
    failure = salts_component_plugin_generation_failure(
        &generations[0].generation);
    check_not_null(failure);
    check_equal(failure->plugin_status, CMETA_PLUGIN_BUSY);
    check_equal(generations[0].generation.module_count, (size_t)1u);
    check_true(cmeta_plugin_lease_valid(generations[0].modules[0].lease));
    check_equal(generations[1].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_DRAINED);
    check_equal(generations[1].generation.module_count, (size_t)0u);
    check_false(cmeta_plugin_lease_valid(generations[1].modules[0].lease));
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[0], &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[1], &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)0u);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generations[0].generation, &retired),
        SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scopes[0]), SALTS_COMPONENT_PLUGIN_INVALID_STATE);

    /* The healthy N+1 Provider must stop and unload before the Host even
     * attempts recovery of N's FAILED Generation and retained lease. */
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, refs[1]), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, refs[1], &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[1]), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &destroyed[1], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 0);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[1], &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[0], &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[0]), CMETA_PLUGIN_BUSY);

    /* Explicit Host recovery: the original failed Generation consumes only
     * its own retained lease, and no second lease is invented. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generations[0].generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(provider_release_fault_native_attempts() -
                native_release_before, 4);
    check_false(cmeta_plugin_lease_valid(generations[0].modules[0].lease));
    check_equal(generations[0].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_DRAINED);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);

    /* A simulated close failure of N cannot resurrect N+1's stale identity.
     * destroy() executes once inside N, then explicit Host retry calls
     * actual POSIX dlclose without running destroy() again. */
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, refs[0]), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, refs[0], &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    provider_close_fault_arm(1);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[0]), CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - close_injected_before, 1);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[0], &info), CMETA_PLUGIN_OK);
    check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[0]), CMETA_PLUGIN_OK);
    check_equal(provider_close_fault_native_attempts() -
                native_close_before, 2);
    check_equal(provider_close_fault_native_successes() -
                native_success_before, 2);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[1], memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[0], &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
