/* Exact Salts Plugin Registry + real Managed Provider DSO under native
 * dlclose failure injection. No runtime production source is patched. */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_managed_probe.h"

#include <salts/plugin.h>
#include <stdatomic.h>
#include <stddef.h>

#ifndef PRAKTOR_DSO_MANAGED_PATH
#error "CMake must supply the actual Managed Provider DSO image path"
#endif

spec("real Provider DSO unload failure is terminal and destroy-once") {
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
