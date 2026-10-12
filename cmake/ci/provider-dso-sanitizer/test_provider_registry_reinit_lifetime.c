/* Destroy / re-initialize one Registry object after repeated native close
 * failures. Ref/Lease identity is scoped to ONE Registry incarnation:
 * old credentials are never passed to a newly initialized Registry. The
 * current exact Salts Ref format has no cross-Registry incarnation tag.
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
#error "Missing real Managed Provider M43 image"
#endif
#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
#error "Missing real Managed Provider M44 image"
#endif

static cmeta_plugin_status attach_reinit_host_counter(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref,
    atomic_int *counter) {
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

static void stop_reinit_provider(
    cmeta_plugin_registry *registry, cmeta_plugin_ref ref) {
  bool quiet = false;
  check_equal(cmeta_plugin_registry_request_stop(registry, ref),
              CMETA_PLUGIN_OK);
  check_equal(cmeta_plugin_registry_poll_quiescent(
      registry, ref, &quiet), CMETA_PLUGIN_OK);
  check_true(quiet);
}

spec("Registry destroy and re-init require separate Ref/Lease lifetimes") {
  it("finishes both close failure histories, then starts a clean Registry") {
    const char *case_name = getenv("TURBO_REGISTRY_REINIT_CASE");
    const bool partial = case_name && strcmp(case_name, "partial") == 0;
    const cmeta_plugin_registry_config original_cfg = {2u};
    const cmeta_plugin_registry_config new_cfg = {partial ? 2u : 1u};
    const char *const paths[2] = {
      PRAKTOR_DSO_MANAGED_COMPONENT_PATH,
      PRAKTOR_DSO_MANAGED_COMPONENT_N1_PATH
    };
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref original_refs[2] = {{0}}, fresh_refs[2] = {{0}};
    cmeta_plugin_lease old_lease = {0}, saved_old_lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info info = {0};
    atomic_int original_destroy[2], fresh_destroy[2];
    const int faults_before = provider_close_fault_injected();
    const int native_before = provider_close_fault_native_attempts();
    const int native_success_before = provider_close_fault_native_successes();
    size_t fresh_count = partial ? 2u : 1u;

    check_true(case_name != NULL &&
        (strcmp(case_name, "repeated") == 0 || partial));
    for (size_t i = 0u; i < 2u; ++i) {
      atomic_init(&original_destroy[i], 0);
      atomic_init(&fresh_destroy[i], 0);
    }
    check_equal(cmeta_plugin_registry_init(
        &registry, &original_cfg), CMETA_PLUGIN_OK);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[i], &original_refs[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_start(
          &registry, original_refs[i]), CMETA_PLUGIN_OK);
      check_equal(attach_reinit_host_counter(
          &registry, original_refs[i], &original_destroy[i]), CMETA_PLUGIN_OK);
    }

    /* Hold a genuine lease from the first incarnation, preserve an INERT
     * copy of its token, then release the authoritative lease. Never pass
     * saved_old_lease into a new Registry incarnation. */
    check_equal(cmeta_plugin_registry_acquire(
        &registry, original_refs[0], &old_lease, &manifest), CMETA_PLUGIN_OK);
    check_true(cmeta_plugin_lease_valid(old_lease));
    check_not_null(manifest);
    saved_old_lease = old_lease;
    check_equal(cmeta_plugin_registry_release(
        &registry, &old_lease), CMETA_PLUGIN_OK);
    check_false(cmeta_plugin_lease_valid(old_lease));
    for (size_t i = 0u; i < 2u; ++i) {
      stop_reinit_provider(&registry, original_refs[i]);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original_refs[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.callbacks_inflight, (size_t)0u);
    }
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
    check_equal(cmeta_plugin_registry_init(
        &registry, &new_cfg), CMETA_PLUGIN_INVALID_ARGUMENT);

    /* "repeated": first two attempts both fail on M43. "partial":
     * first M43 dlclose succeeds, second M44 close fails. Both cases
     * explicitly complete a third / second Host destroy respectively. */
    provider_close_fault_arm_sequence(
        partial ? UINT32_C(0x2) : UINT32_C(0x3));
    check_equal(cmeta_plugin_registry_destroy(
        &registry), CMETA_PLUGIN_UNLOAD_FAILED);
    check_not_null(registry.impl);
    check_equal(provider_close_fault_injected() - faults_before, 1);
    check_equal(atomic_load_explicit(
        &original_destroy[0], memory_order_acquire), 1);
    check_equal(atomic_load_explicit(
        &original_destroy[1], memory_order_acquire), partial ? 1 : 0);
    check_equal(cmeta_plugin_registry_count(&registry),
                partial ? (size_t)1u : (size_t)2u);
    check_equal(cmeta_plugin_registry_init(
        &registry, &new_cfg), CMETA_PLUGIN_INVALID_ARGUMENT);

    if (!partial) {
      check_equal(cmeta_plugin_registry_destroy(
          &registry), CMETA_PLUGIN_UNLOAD_FAILED);
      check_equal(provider_close_fault_injected() - faults_before, 2);
      check_equal(cmeta_plugin_registry_count(&registry), (size_t)2u);
      check_equal(atomic_load_explicit(
          &original_destroy[0], memory_order_acquire), 1);
      check_equal(atomic_load_explicit(
          &original_destroy[1], memory_order_acquire), 0);
    } else {
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original_refs[0], &info), CMETA_PLUGIN_STALE);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, original_refs[1], &info), CMETA_PLUGIN_OK);
      check_equal(info.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.callbacks_inflight, (size_t)0u);
    }

    /* The last recovery attempt is explicit and must free ALL Registry
     * storage. A previous simulated failure must not preserve hidden
     * leases, callbacks or OS handles. */
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_null(registry.impl);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_equal(provider_close_fault_native_successes() -
                native_success_before, 2);
    check_equal(provider_close_fault_native_attempts() - native_before, 2);
    for (size_t i = 0u; i < 2u; ++i) {
      check_equal(atomic_load_explicit(
          &original_destroy[i], memory_order_acquire), 1);
    }
    /* On an EMPTY Registry the old Lease cannot be consumed. Its bytes
     * remain inert diagnostic data; no old token is used after re-init. */
    check_equal(cmeta_plugin_registry_release(
        &registry, &saved_old_lease), CMETA_PLUGIN_INVALID_ARGUMENT);
    check_true(cmeta_plugin_lease_valid(saved_old_lease));

    /* Start a fresh incarnation on exactly the same Host Registry object.
     * The public Ref and Lease tokens have NO Registry-incarnation field.
     * Equal numeric values between unrelated lifetimes are possible and
     * are NOT interpreted as transfer of authority. */
    check_equal(cmeta_plugin_registry_init(
        &registry, &new_cfg), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_init(
        &registry, &new_cfg), CMETA_PLUGIN_INVALID_ARGUMENT);
    for (size_t i = 0u; i < fresh_count; ++i) {
      check_equal(cmeta_plugin_registry_load(
          &registry, paths[i], &fresh_refs[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_start(
          &registry, fresh_refs[i]), CMETA_PLUGIN_OK);
      check_equal(attach_reinit_host_counter(
          &registry, fresh_refs[i], &fresh_destroy[i]), CMETA_PLUGIN_OK);
      check_equal(cmeta_plugin_registry_get_lifecycle(
          &registry, fresh_refs[i], &info), CMETA_PLUGIN_OK);
      check_equal(info.active_leases, (size_t)0u);
      check_equal(info.callbacks_inflight, (size_t)0u);
    }
    check_equal(cmeta_plugin_registry_count(&registry), fresh_count);
    for (size_t i = 0u; i < fresh_count; ++i)
      stop_reinit_provider(&registry, fresh_refs[i]);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
    check_null(registry.impl);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(provider_close_fault_native_successes() -
                native_success_before, 2 + (int)fresh_count);
    check_equal(provider_close_fault_native_attempts() -
                native_before, 2 + (int)fresh_count);
    for (size_t i = 0u; i < 2u; ++i)
      check_equal(atomic_load_explicit(
          &original_destroy[i], memory_order_acquire), 1);
    for (size_t i = 0u; i < fresh_count; ++i)
      check_equal(atomic_load_explicit(
          &fresh_destroy[i], memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
