/* Exact Salts Plugin Registry + real Managed Provider DSO under native
 * dlclose failure injection. No runtime production source is patched. */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_managed_probe.h"

#include <salts/plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>
#include <stddef.h>

#ifndef PRAKTOR_DSO_MANAGED_PATH
#error "CMake must supply the actual Managed Provider DSO image path"
#endif

typedef struct managed_unload_worker {
  cmeta_plugin_registry *registry;
  cmeta_plugin_ref ref;
  cmeta_plugin_status result;
} managed_unload_worker;

static void managed_unload_in_thread(void *data) {
  managed_unload_worker *worker = (managed_unload_worker *)data;
  worker->result = cmeta_plugin_registry_unload(
      worker->registry, worker->ref);
}

static uint64_t managed_host_now(void *host) {
  (void)host;
  return cmeta_monotonic_ms();
}

static void managed_host_yield(void *host) {
  (void)host;
  cmeta_thread_yield();
}

spec("real Provider DSO unload failure is terminal and destroy-once") {
  it("rejects concurrent unload while destroy callback runs inside DSO") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease lease = {0}, rejected_lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info lifecycle = {0};
    praktor_dso_managed_probe *probe;
    managed_unload_worker worker = {0};
    cmeta_thread_t unload_thread = {0};
    atomic_int destroy_count, destroy_entered, allow_destroy_exit, timeout;
    bool quiet = false;
    int saw_destroy;
    cmeta_plugin_status concurrent_unload, concurrent_start, concurrent_acquire;
    const int injected_before = provider_close_fault_injected();
    const int native_before = provider_close_fault_native_attempts();
    const int success_before = provider_close_fault_native_successes();

    atomic_init(&destroy_count, 0);
    atomic_init(&destroy_entered, 0);
    atomic_init(&allow_destroy_exit, 0);
    atomic_init(&timeout, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &lease, &manifest), CMETA_PLUGIN_OK);
    check_not_null(manifest);
    check_not_null(manifest->self);
    probe = (praktor_dso_managed_probe *)manifest->self;
    probe->host_destroy_count = &destroy_count;
    probe->host_destroy_entered = &destroy_entered;
    probe->host_allow_destroy_exit = &allow_destroy_exit;
    probe->host_destroy_timeout = &timeout;
    probe->host = NULL;
    probe->host_now_ms = managed_host_now;
    probe->host_yield = managed_host_yield;
    check_equal(cmeta_plugin_registry_release(&registry, &lease),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);

    /* The first Registry thread enters the DSO's managed destroy callback.
     * Keep its function frame active while a second thread attempts unload.
     * The first native close deliberately fails only after destroy exits. */
    provider_close_fault_arm(1);
    worker.registry = &registry;
    worker.ref = ref;
    check_equal(cmeta_thread_create(
        &unload_thread, managed_unload_in_thread, &worker), SALTS_OK);
    {
      const uint64_t deadline = cmeta_monotonic_ms() + UINT64_C(10000);
      while (!atomic_load_explicit(
                 &destroy_entered, memory_order_acquire) &&
             cmeta_monotonic_ms() < deadline)
        cmeta_thread_yield();
    }
    saw_destroy = atomic_load_explicit(
        &destroy_entered, memory_order_acquire);
    concurrent_unload = cmeta_plugin_registry_unload(&registry, ref);
    concurrent_start = cmeta_plugin_registry_start(&registry, ref);
    concurrent_acquire = cmeta_plugin_registry_acquire(
        &registry, ref, &rejected_lease, &manifest);

    /* Always release the callback before asserting on races. */
    atomic_store_explicit(
        &allow_destroy_exit, 1, memory_order_release);
    check_equal(cmeta_thread_join(&unload_thread), SALTS_OK);
    check_equal(saw_destroy, 1);
    check_equal(atomic_load_explicit(
        &timeout, memory_order_acquire), 0);
    check_equal(concurrent_unload, CMETA_PLUGIN_BUSY);
    check_equal(concurrent_start, CMETA_PLUGIN_BUSY);
    check_equal(concurrent_acquire, CMETA_PLUGIN_BUSY);
    check_false(cmeta_plugin_lease_valid(rejected_lease));
    check_null(manifest);
    check_equal(worker.result, CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - injected_before, 1);
    check_equal(provider_close_fault_native_attempts() - native_before, 0);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(lifecycle.callbacks_inflight, (size_t)0u);

    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(provider_close_fault_native_attempts() - native_before, 1);
    check_equal(provider_close_fault_native_successes() - success_before, 1);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }

  it("preserves the quiescent image across two injected close failures") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info lifecycle = {0};
    praktor_dso_managed_probe *probe;
    atomic_int destroy_count;
    bool quiescent = false;
    const int injected_before = provider_close_fault_injected();
    const int native_before = provider_close_fault_native_attempts();
    const int successful_before = provider_close_fault_native_successes();

    atomic_init(&destroy_count, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_PATH, &ref), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_ref_valid(ref));
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &lease, &manifest), CMETA_PLUGIN_OK);
    check_not_null(manifest);
    check_not_null(manifest->self);
    probe = (praktor_dso_managed_probe *)manifest->self;
    probe->host_destroy_count = &destroy_count;

    /* Verify that the actual lease blocks stop/unload until explicitly
     * released. No raw borrowed manifest pointers are dereferenced later. */
    check_equal(cmeta_plugin_registry_unload(&registry, ref),
                CMETA_PLUGIN_BUSY);
    check_equal(cmeta_plugin_registry_release(&registry, &lease),
                CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(lease));
    check_equal(cmeta_plugin_registry_request_stop(&registry, ref),
                CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiescent), CMETA_PLUGIN_OK);
    check_true(quiescent);

    /* This is the ONLY injection: the production implementation reaches
     * its real native POSIX close through the wrapper on later success. */
    provider_close_fault_arm(2);
    check_equal(cmeta_plugin_registry_unload(&registry, ref),
                CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - injected_before, 1);
    check_equal(provider_close_fault_native_attempts() - native_before, 0);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(lifecycle.active_leases, (size_t)0u);
    check_equal(lifecycle.callbacks_inflight, (size_t)0u);

    /* Once destroy ran, the plugin MUST NOT start or admit fresh leases.
     * A second failed close may only touch the OS handle, not destroy(). */
    check_equal(cmeta_plugin_registry_start(&registry, ref),
                CMETA_PLUGIN_INVALID_STATE);
    lease = (cmeta_plugin_lease){0};
    manifest = NULL;
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &lease, &manifest), CMETA_PLUGIN_INVALID_STATE);
    check_false(cmeta_plugin_lease_valid(lease));
    check_null(manifest);
    check_equal(cmeta_plugin_registry_unload(&registry, ref),
                CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - injected_before, 2);
    check_equal(provider_close_fault_native_attempts() - native_before, 0);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);

    /* No failure remains armed. Now use actual dlclose and clear the slot.
     * host_destroy_count lives in this test, so remains safe to inspect
     * AFTER the DSO's executable pages have been unmapped. */
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(provider_close_fault_native_attempts() - native_before, 1);
    check_equal(provider_close_fault_native_successes() - successful_before, 1);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
