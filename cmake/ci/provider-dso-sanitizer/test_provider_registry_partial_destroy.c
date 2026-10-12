/* Partial Registry destroy with two real Managed Provider DSOs. The first
 * native close succeeds; the second fails once. Preserve the correct Slot
 * ownership and ensure no Managed destroy callback is ever repeated.
 */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_managed_probe.h"

#include <salts/plugin.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_PATH
#error "Real Managed Provider M43 path not defined"
#endif
#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
#error "Real Managed Provider M44 path not defined"
#endif

static cmeta_plugin_status attach_host_destroy_counter(
    cmeta_plugin_registry *registry,
    cmeta_plugin_ref ref, atomic_int *counter) {
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  cmeta_plugin_status status = cmeta_plugin_registry_acquire(
      registry, ref, &lease, &manifest);
  if (status != CMETA_PLUGIN_OK)
    return status;
  if (manifest == NULL || manifest->self == NULL) {
    (void)cmeta_plugin_registry_release(registry, &lease);
    return CMETA_PLUGIN_INVALID_MANIFEST;
  }
  ((praktor_dso_managed_probe *)manifest->self)->host_destroy_count = counter;
  return cmeta_plugin_registry_release(registry, &lease);
}

static void prepare_stop(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  bool quiet = false;
  check_equal(cmeta_plugin_registry_request_stop(registry, ref),
              CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_poll_quiescent(
      registry, ref, &quiet), CMETA_PLUGIN_OK);
  check_true(quiet);
}

spec("partial Registry destroy: one real dlclose then one simulated failure") {
  it("retains only failed DSO and safely reuses successful Slot before final destroy") {
    const cmeta_plugin_registry_config config = {2u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref old_refs[2] = {{0}};
    cmeta_plugin_ref fresh = {0};
    const char *const paths[2] = {
      PRAKTOR_DSO_MANAGED_COMPONENT_PATH,
      PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
    };
    cmeta_plugin_lifecycle_info info = {0};
    cmeta_plugin_lease refused_lease = {0};
    const cmeta_plugin_manifest *refused_manifest = NULL;
    atomic_int destroy_old[2], destroy_fresh;
    const int initial_injected = provider_close_fault_injected();
    const int initial_native = provider_close_fault_native_attempts();
    const int initial_successes = provider_close_fault_native_successes();
    bool quiet = false;

    atomic_init(&destroy_old[0], 0);
    atomic_init(&destroy_old[1], 0);
    atomic_init(&destroy_fresh, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    for (size_t i = 0; i < 2u; ++i) {
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[i], &old_refs[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_start(
          &registry, old_refs[i]), CMETA_PLUGIN_OK);
      check_equal(attach_host_destroy_counter(
          &registry, old_refs[i], &destroy_old[i]), CMETA_PLUGIN_OK);
      prepare_stop(&registry, old_refs[i]);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, old_refs[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    }
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);

    /* Leave first DSO's close genuine, then fail the second native close
     * ONCE without changing its handle. Both destroy callbacks have run. */
    provider_close_fault_arm_after(1, 1);
    check_equal(cmeta_plugin_registry_destroy(&registry),
                CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - initial_injected, 1);
    check_equal(provider_close_fault_native_attempts() - initial_native, 1);
    check_equal(provider_close_fault_native_successes() - initial_successes, 1);
    check_not_null(registry.impl);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
    check_equal(atomic_load_explicit(
        &destroy_old[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroy_old[1], memory_order_acquire), 1);

    /* First occupied Slot has been cleared and invalidated; second retains
     * exactly its own quiescent module with zero leases/callbacks. */
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_refs[0], &info), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_unload(
        &registry, old_refs[0]), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, old_refs[1], &info), CMETA_PLUGIN_OK);
    check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(info.active_leases, (size_t)0u);
    check_equal(info.callbacks_inflight, (size_t)0u);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, old_refs[1], &refused_lease, &refused_manifest),
        CMETA_PLUGIN_INVALID_STATE);
    check_false(cmeta_plugin_lease_valid(refused_lease));
    check_null(refused_manifest);
    check_equal(cmeta_plugin_registry_start(
        &registry, old_refs[1]), CMETA_PLUGIN_INVALID_STATE);

    /* Once destroy's rollback clears the admission gate, an independent
     * Host can reuse ONLY the released slot; its identity must change.
     * The second, failed DSO remains isolated while this happens. */
    check_equal(cmeta_plugin_registry_load(
        &registry, paths[0], &fresh), CMETA_PLUGIN_OK);
    check_equal(fresh.slot, old_refs[0].slot);
    check_true(fresh.generation != old_refs[0].generation);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, old_refs[0], &refused_lease, &refused_manifest),
        CMETA_PLUGIN_STALE);
    check_false(cmeta_plugin_lease_valid(refused_lease));
    check_null(refused_manifest);
    check_equal(cmeta_plugin_registry_start(
        &registry, fresh), CMETA_PLUGIN_OK);
    check_equal(attach_host_destroy_counter(
        &registry, fresh, &destroy_fresh), CMETA_PLUGIN_OK);
    prepare_stop(&registry, fresh);

    /* Explicit retry now closes the fresh occupant and the original failed
     * second DSO. destroy of that second DSO MUST NOT be called again. */
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_null(registry.impl);
    check_equal(provider_close_fault_injected() - initial_injected, 1);
    check_equal(provider_close_fault_native_attempts() - initial_native, 3);
    check_equal(provider_close_fault_native_successes() - initial_successes, 3);
    check_equal(atomic_load_explicit(
        &destroy_old[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroy_old[1], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroy_fresh, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
