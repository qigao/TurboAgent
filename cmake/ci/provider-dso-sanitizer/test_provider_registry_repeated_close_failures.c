/* Two explicit multi-attempt failure schedules over real Managed DSOs:
 * same-image:   close M43 fails twice, then M43 and M44 really dlclose.
 * across-images: M43 fails, then M43 dlclose succeeds and M44 fails.
 * In the latter case M43's slot is reused before the final Registry destroy.
 * No product loader, Registry, or lifecycle code is modified.
 */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_managed_probe.h"

#include <salts/plugin.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_PATH
#error "Missing Managed Component M43 image"
#endif
#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
#error "Missing Managed Component M44 image"
#endif

static cmeta_plugin_status attach_managed_destroy_counter(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref,
    atomic_int *counter) {
  cmeta_plugin_lease lease = {0};
  const cmeta_plugin_manifest *manifest = NULL;
  cmeta_plugin_status status = cmeta_plugin_registry_acquire(
      registry, ref, &lease, &manifest);
  if (status != CMETA_PLUGIN_OK) return status;
  if (manifest == NULL || manifest->self == NULL) {
    (void)cmeta_plugin_registry_release(registry, &lease);
    return CMETA_PLUGIN_INVALID_MANIFEST;
  }
  ((praktor_dso_managed_probe *)manifest->self)->host_destroy_count = counter;
  return cmeta_plugin_registry_release(registry, &lease);
}

static void stop_managed_provider(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  bool quiet = false;
  check_equal(cmeta_plugin_registry_request_stop(
      registry, ref), CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_poll_quiescent(
      registry, ref, &quiet), CMETA_PLUGIN_OK);
  check_true(quiet);
}

spec("repeated Managed DSO close failures across Registry destroy attempts") {
  it("preserves bounded ownership through two failures and explicit final retry") {
    const cmeta_plugin_registry_config config = {2u};
    const char *const paths[2] = {
      PRAKTOR_DSO_MANAGED_COMPONENT_PATH,
      PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
    };
    const char *schedule = getenv("TURBO_DSO_CLOSE_FAILURE_SCHEDULE");
    const bool across = schedule != NULL &&
        strcmp(schedule, "across-images") == 0;
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref original[2] = {{0}}, replacement = {0}, rejected = {0};
    cmeta_plugin_lifecycle_info info = {0};
    cmeta_plugin_lease denied_lease = {0};
    const cmeta_plugin_manifest *denied_manifest = NULL;
    atomic_int destroyed[2], fresh_destroyed;
    const int injected_before = provider_close_fault_injected();
    const int native_before = provider_close_fault_native_attempts();
    const int success_before = provider_close_fault_native_successes();

    check_true(schedule != NULL &&
        (strcmp(schedule, "same-image") == 0 ||
         strcmp(schedule, "across-images") == 0));
    atomic_init(&destroyed[0], 0);
    atomic_init(&destroyed[1], 0);
    atomic_init(&fresh_destroyed, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[i], &original[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_start(
          &registry, original[i]), CMETA_PLUGIN_OK);
      check_equal(attach_managed_destroy_counter(
          &registry, original[i], &destroyed[i]), CMETA_PLUGIN_OK);
      stop_managed_provider(&registry, original[i]);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    }
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);

    /* Attempt 1 fails on M43. In both schedules M44 has not yet been
     * destroyed or closed; no Slot can be reclaimed prematurely. */
    provider_close_fault_arm_sequence(
        across ? UINT32_C(0x5) : UINT32_C(0x3));
    check_equal(cmeta_plugin_registry_destroy(&registry),
                CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_sequence_calls(), 1);
    check_equal(provider_close_fault_injected() - injected_before, 1);
    check_equal(provider_close_fault_native_attempts() - native_before, 0);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[1], memory_order_acquire), 0);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, original[0], &info), CMETA_PLUGIN_OK);
    check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(info.active_leases, (size_t)0u);
    check_equal(info.callbacks_inflight, (size_t)0u);
    rejected = (cmeta_plugin_ref){1u, 123u};
    check_equal(cmeta_plugin_registry_load(
        &registry, paths[0], &rejected), CMETA_PLUGIN_CAPACITY_EXCEEDED);
    check_false(cmeta_plugin_ref_valid(rejected));

    /* Attempt 2 takes a different route according to the selected
     * schedule, but NEVER repeats M43's irreversible destroy callback. */
    check_equal(cmeta_plugin_registry_destroy(&registry),
                CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - injected_before, 2);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 1);
    if (across) {
      /* Call 2 genuinely closes M43. Call 3 rejects M44. */
      check_equal(provider_close_fault_sequence_calls(), 3);
      check_equal(provider_close_fault_native_attempts() - native_before, 1);
      check_equal(provider_close_fault_native_successes() - success_before, 1);
      check_equal(atomic_load_explicit(
          &destroyed[1], memory_order_acquire), 1);
      check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original[0], &info), CMETA_PLUGIN_STALE);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original[1], &info), CMETA_PLUGIN_OK);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.callbacks_inflight, (size_t)0u);
      denied_manifest = NULL;
      check_equal(cmeta_plugin_registry_acquire(
          &registry, original[1], &denied_lease, &denied_manifest),
          CMETA_PLUGIN_INVALID_STATE);
      check_false(cmeta_plugin_lease_valid(denied_lease));
      check_null(denied_manifest);

      /* The Host may reuse ONLY the cleared M43 Slot. M44's failed Slot
       * and its destroy_called sentinel are still authoritative. */
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[0], &replacement), CMETA_PLUGIN_OK);
      check_equal(replacement.slot, original[0].slot);
      check_true(replacement.generation != original[0].generation);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original[0], &info), CMETA_PLUGIN_STALE);
      check_equal(cmeta_plugin_registry_unload(
          &registry, original[0]), CMETA_PLUGIN_STALE);
      check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
      check_equal(cmeta_plugin_registry_start(
          &registry, replacement), CMETA_PLUGIN_OK);
      check_equal(attach_managed_destroy_counter(
          &registry, replacement, &fresh_destroyed), CMETA_PLUGIN_OK);
      stop_managed_provider(&registry, replacement);
    } else {
      /* Both failed calls targeted the first unchanged DSO handle.
       * Neither module was closed; capacity still rejects a third load. */
      check_equal(provider_close_fault_sequence_calls(), 2);
      check_equal(provider_close_fault_native_attempts() - native_before, 0);
      check_equal(atomic_load_explicit(
          &destroyed[1], memory_order_acquire), 0);
      check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original[0], &info), CMETA_PLUGIN_OK);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
      rejected = (cmeta_plugin_ref){1u, 456u};
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[0], &rejected), CMETA_PLUGIN_CAPACITY_EXCEEDED);
      check_false(cmeta_plugin_ref_valid(rejected));
    }

    /* Attempt 3 is an EXPLICIT Host retry. No fault bits remain set:
     * every remaining occupied DSO invokes the original POSIX dlclose.
     * Registry should finally destroy its lock/storage exactly once. */
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_null(registry.impl);
    check_equal(provider_close_fault_injected() - injected_before, 2);
    check_equal(provider_close_fault_sequence_calls(), across ? 5 : 4);
    check_equal(provider_close_fault_native_attempts() - native_before,
                across ? 3 : 2);
    check_equal(provider_close_fault_native_successes() - success_before,
                across ? 3 : 2);
    check_equal(atomic_load_explicit(
        &destroyed[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &destroyed[1], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &fresh_destroyed, memory_order_acquire), across ? 1 : 0);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
