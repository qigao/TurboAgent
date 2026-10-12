/* One real Managed Component Provider is held by a published Generation.
 * Faults occur at two different real Salts API boundaries:
 *   Registry Lease release during Generation drain, then POSIX DSO close.
 * Both source implementations remain intact, wrapped only in this
 * isolated test build. No implicit retry or second lifecycle authority.
 */
#include "tinytest.h"
#include "close_fault_hook.h"
#include "praktor_dso_fixture.h"
#include "praktor_dso_managed_probe.h"

#include <salts/component_plugin.h>
#include <stdatomic.h>
#include <stdint.h>

#ifndef PRAKTOR_DSO_MANAGED_COMPONENT_PATH
#error "CMake must supply the real managed Component Provider DSO image"
#endif

spec("Generation drain failure and Managed Provider close failure") {
  it("retains the authoritative lease and destroys exactly once across explicit recovery") {
    const cmeta_plugin_registry_config config = {1u};
    cmeta_plugin_registry registry = {0};
    cmeta_plugin_ref ref = {0};
    cmeta_plugin_lease host_lease = {0}, rejected_lease = {0};
    const cmeta_plugin_manifest *manifest = NULL;
    cmeta_plugin_lifecycle_info lifecycle = {0};
    const salts_component_plugin_failure *failure;
    praktor_dso_managed_probe *probe;
    salts_component_plugin_generation generation = {0};
    salts_component_deployment deployments[1] = {0};
    salts_component_instance instances[1] = {0};
    salts_component_dependency dependencies[1] = {0};
    size_t order[1] = {0};
    salts_component_plugin_module modules[1] = {0};
    const salts_component_plugin_generation_storage storage = {
      deployments, 1u, instances, 1u, dependencies, 1u, order, 1u,
      modules, 1u
    };
    salts_component_plugin_runtime runtime = SALTS_COMPONENT_PLUGIN_RUNTIME_INIT;
    salts_component_plugin_scope scope = SALTS_COMPONENT_PLUGIN_SCOPE_INIT;
    salts_component_plugin_generation *retired = NULL;
    salts_component_service service = {0};
    praktor_dso_gate gate = {0};
    praktor_dso_control control = {0};
    const salts_component_plugin_source source = {
      {0}, PRAKTOR_DSO_PROVIDER_EXPORT_ID, NULL, NULL
    };
    atomic_int destroy_count, callback_entered, callback_exited;
    const int release_fault_before = provider_release_fault_injected();
    const int release_native_before = provider_release_fault_native_attempts();
    const int close_fault_before = provider_close_fault_injected();
    const int close_native_before = provider_close_fault_native_attempts();
    const int close_success_before = provider_close_fault_native_successes();
    bool quiet = false;

    atomic_init(&destroy_count, 0);
    atomic_init(&callback_entered, 0);
    atomic_init(&callback_exited, 0);
    check_equal(cmeta_plugin_registry_init(&registry, &config), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_load(
        &registry, PRAKTOR_DSO_MANAGED_COMPONENT_PATH, &ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_start(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &host_lease, &manifest), CMETA_PLUGIN_OK);
    check_not_null(manifest);
    check_not_null(manifest->self);
    probe = (praktor_dso_managed_probe *)manifest->self;
    probe->host_destroy_count = &destroy_count;
    check_equal(cmeta_plugin_registry_release(
        &registry, &host_lease), CMETA_PLUGIN_OK);

    {
      salts_component_plugin_source actual_source = source;
      actual_source.plugin = ref;
      check_equal(salts_component_plugin_generation_build(
          &generation, UINT64_C(43), &registry, &storage,
          NULL, 0u, &actual_source, 1u, NULL, 0u),
          SALTS_COMPONENT_PLUGIN_OK);
    }
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);
    check_equal(salts_component_plugin_runtime_init(&runtime),
                SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generation, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_null(retired);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(salts_component_plugin_scope_find_service(
        &scope, praktor_dso_gate_interface(), &service),
        SALTS_COMPONENT_PLUGIN_OK);
    check_equal(praktor_dso_gate_borrow_from_object(
        service.object, service.interfaces, &gate), CMETA_OK);
    control.block_until_cancel = 0;
    atomic_init(&control.entered, 0);
    atomic_init(&control.exited, 0);
    check_equal(praktor_dso_gate_run(&gate, &control), 43);
    check_equal(salts_component_plugin_runtime_close(
        &runtime, &retired), SALTS_COMPONENT_PLUGIN_OK);
    check_true(retired == &generation);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation), SALTS_COMPONENT_PLUGIN_BUSY);
    check_equal(salts_component_plugin_scope_release(
        &scope), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 0);

    /* The one injected Registry release failure MUST NOT consume the
     * Generation-owned lease. This leaves attached Generation FAILED,
     * stopped Components and the authoritative Plugin lease still live. */
    provider_release_fault_arm(1);
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation), SALTS_COMPONENT_PLUGIN_PLUGIN_ERROR);
    check_equal(provider_release_fault_injected() - release_fault_before, 1);
    check_equal(generation.state, SALTS_COMPONENT_PLUGIN_GENERATION_FAILED);
    failure = salts_component_plugin_generation_failure(&generation);
    check_not_null(failure);
    check_equal(failure->plugin_status, CMETA_PLUGIN_BUSY);
    check_equal(generation.module_count, (size_t)1u);
    check_true(cmeta_plugin_lease_valid(modules[0].lease));
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)1u);
    check_equal(cmeta_plugin_registry_unload(
        &registry, ref), CMETA_PLUGIN_BUSY);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_BUSY);

    /* Deny stale generation publication and all new runtime admission.
     * No live scope owns the failed generation, and it may not be rebuilt
     * while its lease and runtime_owner remain authoritative. */
    check_equal(salts_component_plugin_runtime_publish(
        &runtime, &generation, &retired),
        SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    check_equal(salts_component_plugin_scope_acquire(
        &runtime, &scope), SALTS_COMPONENT_PLUGIN_INVALID_STATE);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 0);

    /* Explicit Host decision, NOT automatic drain retry. A successful
     * second drain can release the same held lease exactly once. */
    check_equal(salts_component_plugin_generation_drain(
        &runtime, &generation), SALTS_COMPONENT_PLUGIN_OK);
    check_equal(provider_release_fault_native_attempts() -
                release_native_before, 2);
    check_equal(generation.state, SALTS_COMPONENT_PLUGIN_GENERATION_DRAINED);
    check_equal(generation.module_count, (size_t)0u);
    check_false(cmeta_plugin_lease_valid(modules[0].lease));
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.active_leases, (size_t)0u);
    check_equal(salts_component_plugin_runtime_destroy(
        &runtime), SALTS_COMPONENT_PLUGIN_OK);

    check_equal(cmeta_plugin_registry_request_stop(
        &registry, ref), CMETA_PLUGIN_OK);
    check_equal(cmeta_plugin_registry_poll_quiescent(
        &registry, ref, &quiet), CMETA_PLUGIN_OK);
    check_true(quiet);
    provider_close_fault_arm(1);
    check_equal(cmeta_plugin_registry_unload(
        &registry, ref), CMETA_PLUGIN_UNLOAD_FAILED);
    check_equal(provider_close_fault_injected() - close_fault_before, 1);
    check_equal(provider_close_fault_native_attempts() - close_native_before, 0);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_OK);
    check_equal(lifecycle.state, CMETA_PLUGIN_LIFECYCLE_QUIESCENT);
    check_equal(lifecycle.active_leases, (size_t)0u);
    manifest = NULL;
    check_equal(cmeta_plugin_registry_acquire(
        &registry, ref, &rejected_lease, &manifest),
        CMETA_PLUGIN_INVALID_STATE);
    check_false(cmeta_plugin_lease_valid(rejected_lease));
    check_null(manifest);
    check_equal(cmeta_plugin_registry_start(
        &registry, ref), CMETA_PLUGIN_INVALID_STATE);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)1u);

    /* Only explicit Host retry reaches real POSIX dlclose. Plugin DSO
     * destruction must not be repeated after its irreversible callback. */
    check_equal(cmeta_plugin_registry_unload(&registry, ref), CMETA_PLUGIN_OK);
    check_equal(provider_close_fault_native_attempts() - close_native_before, 1);
    check_equal(provider_close_fault_native_successes() - close_success_before, 1);
    check_equal(atomic_load_explicit(
        &destroy_count, memory_order_acquire), 1);
    check_equal(cmeta_plugin_registry_count(&registry), (size_t)0u);
    check_equal(cmeta_plugin_registry_get_lifecycle(
        &registry, ref, &lifecycle), CMETA_PLUGIN_STALE);
    check_equal(cmeta_plugin_registry_destroy(&registry), CMETA_PLUGIN_OK);
  }
}
