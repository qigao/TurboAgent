/* The FINAL successful Registry destroy is exclusive. Concurrent calls are
 * tested only while a held lease or a blocked DSO destroy callback guarantees
 * that Registry storage cannot yet be freed.
 */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_managed_probe.h"

#include <salts/plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

#ifndef PRAKTOR_DSO_MANAGED_PATH
#error "The real Managed DSO path must be supplied by CMake"
#endif

typedef struct registry_destroy_gate {
  cmeta_plugin_registry *registry;
  cmeta_plugin_ref ref;
  atomic_int ready;
  atomic_int go;
} registry_destroy_gate;

typedef struct registry_destroy_worker {
  registry_destroy_gate *gate;
  cmeta_plugin_status result;
} registry_destroy_worker;

typedef struct registry_acquire_worker {
  registry_destroy_gate *gate;
  cmeta_plugin_lease lease;
  const cmeta_plugin_manifest *manifest;
  cmeta_plugin_status result;
} registry_acquire_worker;

static void attempt_registry_destroy(void *context) {
  registry_destroy_worker *worker = (registry_destroy_worker *)context;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->result = cmeta_plugin_registry_destroy(worker->gate->registry);
}

static void attempt_registry_acquire(void *context) {
  registry_acquire_worker *worker = (registry_acquire_worker *)context;
  atomic_fetch_add_explicit(&worker->gate->ready, 1, memory_order_acq_rel);
  while (!atomic_load_explicit(&worker->gate->go, memory_order_acquire))
    cmeta_thread_yield();
  worker->result = cmeta_plugin_registry_acquire(
      worker->gate->registry, worker->gate->ref,
      &worker->lease, &worker->manifest);
}

static uint64_t registry_host_now(void *host) {
  (void)host;
  return cmeta_monotonic_ms();
}
static void registry_host_yield(void *host) {
  (void)host;
  cmeta_thread_yield();
}

spec("Registry destruction fences live leases and Managed Provider callback") {
  it("rejects destroy with leased DSO, then excludes access during in-flight destroy") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease held = {0}, stale = {0}, denied = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info info = {0};
    praktor_dso_managed_probe *probe;
    registry_destroy_gate initial_gate = {0}, terminal_gate = {0};
    registry_destroy_worker first_destroy = {0}, terminal_destroy = {0};
    registry_acquire_worker rival = {0};
    cmeta_thread_t first_thread = {0}, rival_thread = {0};
    atomic_int destroy_count, entered, allow_exit, timed_out;
    cmeta_plugin_status admission, replay, unload, concurrent_destroy;
    const int injected_before = provider_close_fault_injected();
    const int native_before = provider_close_fault_native_attempts();
    const int succeeded_before = provider_close_fault_native_successes();
    bool quiet = false;
    int ready, arrived;

    atomic_init(&destroy_count, 0);
    atomic_init(&entered, 0);
    atomic_init(&allow_exit, 0);
    atomic_init(&timed_out, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &held, &manifest), CMETA_PLUGIN_OK);
    check_not_null(manifest);
    check_not_null(manifest->self);
    probe = (praktor_dso_managed_probe *)manifest->self;
    probe->host_destroy_count = &destroy_count;
    probe->host_destroy_entered = &entered;
    probe->host_allow_destroy_exit = &allow_exit;
    probe->host_destroy_timeout = &timed_out;
    probe->host = NULL;
    probe->host_now_ms = registry_host_now;
    probe->host_yield = registry_host_yield;

    /* The original Lease forces destroy to fail regardless of the order in
     * which an additional acquire reaches the locked Registry. */
    initial_gate.registry = &registry;
    initial_gate.ref = ref;
    atomic_init(&initial_gate.ready, 0);
    atomic_init(&initial_gate.go, 0);
    first_destroy.gate = &initial_gate;
    rival.gate = &initial_gate;
    check_equal(cmeta_thread_create(
        &first_thread, attempt_registry_destroy, &first_destroy), SALTS_OK);
    check_equal(cmeta_thread_create(
        &rival_thread, attempt_registry_acquire, &rival), SALTS_OK);
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (atomic_load_explicit(&initial_gate.ready, memory_order_acquire) < 2 &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    ready = atomic_load_explicit(&initial_gate.ready, memory_order_acquire);
    atomic_store_explicit(&initial_gate.go, 1, memory_order_release);
    check_equal(cmeta_thread_join(&first_thread), SALTS_OK);
    check_equal(cmeta_thread_join(&rival_thread), SALTS_OK);
    check_equal(ready, 2);
    check_equal(first_destroy.result, CMETA_PLUGIN_BUSY);
    check_equal(rival.result, CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(rival.lease));
    check_not_null(rival.manifest);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.active_leases, (size_t)2u);
    check_equal(atomic_load_explicit(&destroy_count, memory_order_acquire), 0);
    stale = held; /* Copied token is test data, never a second owner. */
    check_equal(cmeta_plugin_registry_release(
        &registry, &held), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_release(
        &registry, &rival.lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(held));
    check_false(cmeta_plugin_lease_valid(rival.lease));
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);

    /* This destroy would free Registry memory if successful. Hold the real
     * Provider DSO's destroy() inside the callback before attempting API
     * operations; all callers finish BEFORE allowing the final free. */
    provider_close_fault_arm(1);
    terminal_gate.registry = &registry;
    terminal_gate.ref = ref;
    atomic_init(&terminal_gate.ready, 0);
    atomic_init(&terminal_gate.go, 1);
    terminal_destroy.gate = &terminal_gate;
    check_equal(cmeta_thread_create(
        &first_thread, attempt_registry_destroy, &terminal_destroy), SALTS_OK);
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(&entered, memory_order_acquire) &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    arrived = atomic_load_explicit(&entered, memory_order_acquire);
    denied = (cmeta_plugin_lease){0};
    manifest = NULL;
    admission = cmeta_plugin_registry_acquire(
        &registry, ref, &denied, &manifest);
    replay = cmeta_plugin_registry_release(&registry, &stale);
    unload = cmeta_plugin_registry_unload(&registry, ref);
    concurrent_destroy = cmeta_plugin_registry_destroy(&registry);

    /* Release callback before any assertion, then join the Registry thread
     * before inspecting the retained QUIESCENT slot. */
    atomic_store_explicit(&allow_exit, 1, memory_order_release);
    check_equal(cmeta_thread_join(&first_thread), SALTS_OK);
    check_equal(arrived, 1);
    check_equal(atomic_load_explicit(&timed_out, memory_order_acquire), 0);
    check_equal(admission, CMETA_PLUGIN_BUSY);
    check_false(cmeta_plugin_lease_valid(denied));
    check_null(manifest);
    check_equal(replay, CMETA_PLUGIN_STALE);
    check_true(cmeta_plugin_lease_valid(stale));
    check_equal(unload, CMETA_PLUGIN_BUSY);
    check_equal(concurrent_destroy, CMETA_PLUGIN_BUSY);
    check_equal(terminal_destroy.result, CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - injected_before, 1);
    check_equal(provider_close_fault_native_attempts() - native_before, 0);
    check_equal(atomic_load_explicit(&destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &info), CMETA_PLUGIN_OK);
    check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(info.active_leases, (size_t)0u);
    check_equal(info.callbacks_inflight, (size_t)0u);
    manifest = NULL;
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &denied, &manifest), CMETA_PLUGIN_INVALID_STATE);
    check_null(manifest);
    check_equal(cmeta_plugin_registry_start(
        &registry, ref), CMETA_PLUGIN_INVALID_STATE);

    /* Final destroy is exclusive. No other thread is touching Registry
     * storage while it can be freed; the real dlclose succeeds and destroy
     * must not be invoked twice. */
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_null(registry.impl);
    check_equal(provider_close_fault_native_attempts() - native_before, 1);
    check_equal(provider_close_fault_native_successes() - succeeded_before, 1);
    check_equal(atomic_load_explicit(&destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
