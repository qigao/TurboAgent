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
#include <stdlib.h>
#include <string.h>

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


typedef struct provider_reuse_gate {
  atomic_int ready;
  atomic_int go;
  cmeta_plugin_registry *registry;
  salts_component_plugin_runtime *runtime;
} provider_reuse_gate;

typedef struct provider_reuse_acquire_worker {
  provider_reuse_gate *gate;
  cmeta_plugin_ref ref;
  cmeta_plugin_status status;
  cmeta_plugin_lease lease;
  const cmeta_plugin_manifest *manifest;
} provider_reuse_acquire_worker;

typedef struct provider_reuse_publish_worker {
  provider_reuse_gate *gate;
  salts_component_plugin_generation *generation;
  salts_component_plugin_generation *previous;
  salts_component_plugin_status status;
} provider_reuse_publish_worker;

static void provider_reuse_acquire_in_thread(void *data) {
  provider_reuse_acquire_worker *worker =
      (provider_reuse_acquire_worker *)data;
  worker->lease = (cmeta_plugin_lease){0};
  worker->manifest = NULL;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->status = cmeta_plugin_registry_acquire(
      worker->gate->registry, worker->ref,
      &worker->lease, &worker->manifest);
}


typedef struct provider_reuse_release_worker {
  provider_reuse_gate *gate;
  cmeta_plugin_lease old_token;
  cmeta_plugin_status status;
} provider_reuse_release_worker;

static void provider_reuse_release_in_thread(void *data) {
  provider_reuse_release_worker *worker =
      (provider_reuse_release_worker *)data;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->status = cmeta_plugin_registry_release(
      worker->gate->registry, &worker->old_token);
}

static void provider_reuse_publish_in_thread(void *data) {
  provider_reuse_publish_worker *worker =
      (provider_reuse_publish_worker *)data;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->status = salts_component_plugin_runtime_publish(
      worker->gate->runtime, worker->generation, &worker->previous);
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

  it("rejects stale Lease replay while reused Slot acquires and publishes") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref old_ref = {0}, new_ref = {0};
    cmeta_plugin_lease original_lease = {0}, stale_token = {0};
    cmeta_plugin_lease consumed_token = {0}, replacement_lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info info = {0};
    managed_generation new_generation = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    provider_reuse_gate gate = {0};
    provider_reuse_release_worker stale_release = {0};
    provider_reuse_acquire_worker current_acquire = {0};
    provider_reuse_publish_worker publication = {0};
    cmeta_thread_t threads[3] = {0};
    atomic_int old_destroy, new_destroy, entered, exited;
    int ready;
    bool quiet = false;

    atomic_init(&old_destroy, 0);
    atomic_init(&new_destroy, 0);
    atomic_init(&entered, 0);
    atomic_init(&exited, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);

    /* First incarnation: obtain a genuine Plugin Lease, preserve a COPY of
     * the token, and release the authoritative original before dlclose. */
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(managed_set_host_destroy_counter(
        &registry, old_ref, &old_destroy), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, old_ref, &original_lease, &manifest), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(original_lease));
    check_not_null(manifest);
    stale_token = original_lease; /* Not an authoritative second Lease! */
    check_equal(cmeta_plugin_registry_release(
        &registry, &original_lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(original_lease));
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, old_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(&old_destroy, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);

    /* Physical slot and lease-index may be reused, but Plugin generation
     * fencing must prevent a copied token from releasing any new Lease. */
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &new_ref), CMETA_PLUGIN_OK);
    check_equal(new_ref.slot, old_ref.slot);
    check_true(new_ref.generation != old_ref.generation);
    check_equal(cmeta_plugin_registry_start(&registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(managed_set_host_destroy_counter(
        &registry, new_ref, &new_destroy), CMETA_PLUGIN_OK);
    check_equal(managed_build(
        &new_generation, UINT64_C(45), &registry, new_ref),
        SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);

    gate.registry = &registry;
    gate.runtime = &runtime;
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.go, 0);
    stale_release.gate = &gate;
    stale_release.old_token = stale_token;
    current_acquire.gate = &gate;
    current_acquire.ref = new_ref;
    publication.gate = &gate;
    publication.generation = &new_generation.generation;
    check_equal(cmeta_thread_create(
        &threads[0], provider_reuse_release_in_thread, &stale_release), SALTS_OK);
    check_equal(cmeta_thread_create(
        &threads[1], provider_reuse_acquire_in_thread, &current_acquire), SALTS_OK);
    check_equal(cmeta_thread_create(
        &threads[2], provider_reuse_publish_in_thread, &publication), SALTS_OK);
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (atomic_load_explicit(&gate.ready, memory_order_acquire) < 3 &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&gate.ready, memory_order_acquire);
    atomic_store_explicit(&gate.go, 1, memory_order_release);
    for (size_t i = 0u; i < 3u; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    check_equal(ready, 3);
    check_equal(stale_release.status, CMETA_PLUGIN_STALE);
    check_true(cmeta_plugin_lease_valid(stale_release.old_token));
    check_equal(stale_release.old_token.plugin.slot, stale_token.plugin.slot);
    check_equal(stale_release.old_token.plugin.generation,
                stale_token.plugin.generation);
    check_equal(stale_release.old_token.generation, stale_token.generation);
    check_equal(current_acquire.status, CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(current_acquire.lease));
    check_not_null(current_acquire.manifest);
    check_equal(current_acquire.lease.plugin.slot, stale_token.plugin.slot);
    check_true(current_acquire.lease.plugin.generation !=
               stale_token.plugin.generation);
    check_equal(current_acquire.lease.slot, stale_token.slot);
    check_equal(publication.status, SALTS_COMPONENT_PLUGIN_OK);
    check_null(publication.previous);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)2u);

    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(
        &scope), UINT64_C(45));
    check_equal(managed_run_scoped(&scope, &entered, &exited), 43);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &new_generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(
        &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);

    /* A copied token from THIS SAME new incarnation must also be fenced
     * after its real holder releases it and its lease slot gets recycled. */
    consumed_token = current_acquire.lease;
    check_equal(cmeta_plugin_registry_release(
        &registry, &current_acquire.lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(current_acquire.lease));
    manifest = NULL;
    check_equal(cmeta_plugin_registry_acquire(
        &registry, new_ref, &replacement_lease, &manifest), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(replacement_lease));
    check_not_null(manifest);
    check_equal(replacement_lease.slot, consumed_token.slot);
    check_true(replacement_lease.generation != consumed_token.generation);
    check_equal(cmeta_plugin_registry_release(
        &registry, &stale_token), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_release(
        &registry, &consumed_token), CMETA_PLUGIN_STALE);
    check_true(cmeta_plugin_lease_valid(stale_token));
    check_true(cmeta_plugin_lease_valid(consumed_token));
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_release(
        &registry, &replacement_lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(replacement_lease));
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, new_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(&old_destroy, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(&new_destroy, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_ref, &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }


  it("fences stale Ref acquire against reused slot and new Generation publication") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref old_ref = {0}, new_ref = {0};
    cmeta_plugin_lifecycle_info info = {0};
    cmeta_plugin_lease negative_lease = {0};
    const cmeta_plugin_manifest *negative_manifest = NULL;
    managed_generation new_generation = {0};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope new_scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    provider_reuse_gate gate = {0};
    provider_reuse_acquire_worker acquisition[2] = {{0}};
    provider_reuse_publish_worker publication = {0};
    cmeta_thread_t threads[3] = {0};
    atomic_int old_destroy, new_destroy, entered, exited;
    const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
    bool quiet = false;
    int ready;

    atomic_init(&old_destroy, 0);
    atomic_init(&new_destroy, 0);
    atomic_init(&entered, 0);
    atomic_init(&exited, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);

    /* The first real Managed DSO fully finishes its lifetime and returns
     * its bounded Registry slot to the free list. */
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &old_ref),
        CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(managed_set_host_destroy_counter(
        &registry, old_ref, &old_destroy), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, old_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, old_ref), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &old_destroy, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);

    /* The same physical slot is occupied by a NEW DSO incarnation. Even
     * before STARTED, the old ref must never resolve to its manifest. */
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &new_ref),
        CMETA_PLUGIN_OK);
    check_equal(new_ref.slot, old_ref.slot);
    check_true(new_ref.generation != old_ref.generation);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, old_ref, &negative_lease, &negative_manifest),
        CMETA_PLUGIN_STALE);
    check_false(cmeta_plugin_lease_valid(negative_lease));
    check_null(negative_manifest);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, new_ref, &negative_lease, &negative_manifest),
        CMETA_PLUGIN_INVALID_STATE);
    check_false(cmeta_plugin_lease_valid(negative_lease));
    check_null(negative_manifest);

    check_equal(cmeta_plugin_registry_start(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(managed_set_host_destroy_counter(
        &registry, new_ref, &new_destroy), CMETA_PLUGIN_OK);
    check_equal(managed_build(
        &new_generation, UINT64_C(45), &registry, new_ref),
        SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_init(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);

    /* Same atomic barrier: an old stale Plugin ref attempts acquire while
     * an authorized new Ref acquires its lease and Generation 45 publishes.
     * Registry and ComponentPlugin must each preserve their own ownership. */
    gate.registry = &registry;
    gate.runtime = &runtime;
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.go, 0);
    acquisition[0].gate = &gate;
    acquisition[0].ref = old_ref;
    acquisition[1].gate = &gate;
    acquisition[1].ref = new_ref;
    publication.gate = &gate;
    publication.generation = &new_generation.generation;
    check_equal(cmeta_thread_create(
        &threads[0], provider_reuse_acquire_in_thread, &acquisition[0]),
        SALTS_OK);
    check_equal(cmeta_thread_create(
        &threads[1], provider_reuse_acquire_in_thread, &acquisition[1]),
        SALTS_OK);
    check_equal(cmeta_thread_create(
        &threads[2], provider_reuse_publish_in_thread, &publication),
        SALTS_OK);
    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < 3 &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    ready = atomic_load_explicit(&gate.ready, memory_order_acquire);
    atomic_store_explicit(&gate.go, 1, memory_order_release);
    for (size_t i = 0u; i < 3u; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    check_equal(ready, 3);
    check_equal(acquisition[0].status, CMETA_PLUGIN_STALE);
    check_false(cmeta_plugin_lease_valid(acquisition[0].lease));
    check_null(acquisition[0].manifest);
    check_equal(acquisition[1].status, CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(acquisition[1].lease));
    check_not_null(acquisition[1].manifest);
    check_equal(publication.status, SALTS_COMPONENT_PLUGIN_OK);
    check_null(publication.previous);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)2u);
    check_equal(cmeta_plugin_registry_unload(
        &registry, old_ref), CMETA_PLUGIN_STALE);

    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &new_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_generation_id(
        &new_scope), UINT64_C(45));
    check_equal(managed_run_scoped(&new_scope, &entered, &exited), 43);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &new_generation.generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(
        &new_scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, retired), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_BUSY);

    /* The one valid concurrent-acquire worker, not the rejected stale
     * caller, releases the final Plugin lease. Then actual dlclose occurs. */
    check_equal(cmeta_plugin_registry_release(
        &registry, &acquisition[1].lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(acquisition[1].lease));
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, new_ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, new_ref), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &old_destroy, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &new_destroy, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_ref, &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, new_ref, &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(
        &registry), CMETA_PLUGIN_OK);
  }

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

  it("recovers two failed Generations in either order and fences reused Plugin slots") {
    const cmeta_plugin_registry_config config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref refs[2] = {{0}}, recycled = {0}, rejected = {0};
    managed_generation generations[2] = {{0}};
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_plugin_scope scopes[2] = {
      SALTS_COMPONENT_PLUGIN_SCOPE_INIT, SALTS_COMPONENT_PLUGIN_SCOPE_INIT
    };
    managed_drain_gate gate = {0};
    managed_drain_worker workers[2] = {{0}};
    cmeta_thread_t threads[2] = {0};
    cmeta_plugin_lifecycle_info info = {0};
    atomic_int destroyed[2], recycled_destroy, entered[2], exited[2];
    const char *const paths[2] = {
      PRAKTOR_DSO_MANAGED_COMPONENT_PATH,
      PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
    };
    const char *order = getenv("TURBO_DSO_RECOVERY_ORDER");
    const size_t first = order && strcmp(order, "new-first") == 0 ? 1u : 0u;
    const size_t last = first == 0u ? 1u : 0u;
    const int fault_before = provider_release_fault_injected();
    const int native_release_before = provider_release_fault_native_attempts();
    const int close_fault_before = provider_close_fault_injected();
    const int native_close_before = provider_close_fault_native_attempts();
    const int close_success_before = provider_close_fault_native_successes();
    int ready = 0;
    bool quiet = false;

    atomic_init(&recycled_destroy, 0);
    for (size_t i = 0u; i < 2u; ++i) {
      atomic_init(&destroyed[i], 0);
      atomic_init(&entered[i], 0);
      atomic_init(&exited[i], 0);
    }
    check_true(!order || strcmp(order, "old-first") == 0 ||
               strcmp(order, "new-first") == 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[i], &refs[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_start(
          &registry, refs[i]), CMETA_PLUGIN_OK);
      check_equal(managed_set_host_destroy_counter(
          &registry, refs[i], &destroyed[i]), CMETA_PLUGIN_OK);
      check_equal(managed_build(&generations[i], UINT64_C(43) + i,
                                &registry, refs[i]), SALTS_COMPONENT_PLUGIN_OK);
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
    check_equal(managed_run_scoped(&scopes[0], &entered[0], &exited[0]), 43);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generations[1].generation, &retired),
        SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generations[0].generation);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scopes[1]), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(managed_run_scoped(&scopes[1], &entered[1], &exited[1]), 44);
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

    gate.runtime = &runtime;
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.go, 0);
    for (size_t i = 0u; i < 2u; ++i) {
      workers[i].gate = &gate;
      workers[i].generation = &generations[i].generation;
      check_equal(cmeta_thread_create(
          &threads[i], managed_drain_concurrent, &workers[i]), SALTS_OK);
    }
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (atomic_load_explicit(&gate.ready, memory_order_acquire) < 2 &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&gate.ready, memory_order_acquire);
    provider_release_fault_arm_two_plugins(refs[0], refs[1]);
    atomic_store_explicit(&gate.go, 1, memory_order_release);
    for (size_t i = 0u; i < 2u; ++i)
      check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);
    check_equal(ready, 2);
    check_equal(provider_release_fault_injected() - fault_before, 2);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(workers[i].result, SALTS_COMPONENT_PLUGIN_PLUGIN_ERROR);
      check_equal(generations[i].generation.state,
                  SALTS_COMPONENT_PLUGIN_GENERATION_FAILED);
      check_equal(salts_component_plugin_generation_failure(
          &generations[i].generation)->plugin_status, CMETA_PLUGIN_BUSY);
      check_equal(generations[i].generation.module_count, (size_t)1u);
      check_true(cmeta_plugin_lease_valid(generations[i].modules[0].lease));
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, refs[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)1u);
      check_equal(salts_component_plugin_runtime_publish(
          &runtime, &generations[i].generation, &retired),
          SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    }
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scopes[0]), SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    rejected = (cmeta_plugin_ref){1u, 777u};
    check_equal(cmeta_plugin_registry_load(
        &registry, paths[first], &rejected), CMETA_PLUGIN_CAPACITY_EXCEEDED);
    check_false(cmeta_plugin_ref_valid(rejected));

    /* The Host selects which generation recovers FIRST. The other stays
     * FAILED and retains precisely its original Plugin Lease. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generations[first].generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(generations[first].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_DRAINED);
    check_false(cmeta_plugin_lease_valid(generations[first].modules[0].lease));
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[last], &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)1u);
    check_equal(generations[last].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_FAILED);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_BUSY);

    check_equal(cmeta_plugin_registry_request_stop(
        &registry, refs[first]), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, refs[first], &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    provider_close_fault_arm(1);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[first]), CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - close_fault_before, 1);
    check_equal(atomic_load_explicit(
        &destroyed[first], memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    rejected = (cmeta_plugin_ref){1u, 888u};
    check_equal(cmeta_plugin_registry_load(
        &registry, paths[first], &rejected), CMETA_PLUGIN_CAPACITY_EXCEEDED);
    check_false(cmeta_plugin_ref_valid(rejected));
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[first]), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[first], &info), CMETA_PLUGIN_STALE);
    check_equal(atomic_load_explicit(
        &destroyed[first], memory_order_acquire), 1);

    /* Reuse exactly the released Registry slot, but with a NEW generation.
     * The old Ref can neither observe nor unload the new Plugin occupant. */
    check_equal(cmeta_plugin_registry_load(
        &registry, paths[first], &recycled), CMETA_PLUGIN_OK);
    check_equal(recycled.slot, refs[first].slot);
    check_true(recycled.generation != refs[first].generation);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[first], &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[first]), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    check_equal(cmeta_plugin_registry_start(
        &registry, recycled), CMETA_PLUGIN_OK);
    check_equal(managed_set_host_destroy_counter(
        &registry, recycled, &recycled_destroy), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, recycled), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, recycled, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, recycled), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &recycled_destroy, memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[first], memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);

    /* Recover the second FAILED Generation only AFTER the first slot was
     * reused and safely closed. No hidden admission or release retries. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generations[last].generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(generations[last].generation.state,
                SALTS_COMPONENT_PLUGIN_GENERATION_DRAINED);
    check_false(cmeta_plugin_lease_valid(generations[last].modules[0].lease));
    check_equal(provider_release_fault_native_attempts() -
                native_release_before, 5);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, refs[last]), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, refs[last], &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    check_equal(cmeta_plugin_registry_unload(
        &registry, refs[last]), CMETA_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &destroyed[last], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[first], memory_order_acquire), 1);
    check_equal(provider_close_fault_native_attempts() -
                native_close_before, 3);
    check_equal(provider_close_fault_native_successes() -
                close_success_before, 3);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, refs[last], &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

}
