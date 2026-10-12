#include "close_fault_hook.h"
#include "plugin_loader_internal.h"

#include <stdatomic.h>

static atomic_int pending;
static atomic_int injected;
static atomic_int attempts;
static atomic_int successes;

/* The real Salts plugin_posix.c implementation is compiled unmodified apart
 * from a test-only symbol rename. Every successful delegate performs dlclose. */
cmeta_plugin_status cmeta_plugin_platform_close_real(
    cmeta_plugin_library *library);

void provider_close_fault_arm(int failures) {
  atomic_store_explicit(&pending, failures, memory_order_release);
}

int provider_close_fault_injected(void) {
  return atomic_load_explicit(&injected, memory_order_acquire);
}

int provider_close_fault_native_attempts(void) {
  return atomic_load_explicit(&attempts, memory_order_acquire);
}

int provider_close_fault_native_successes(void) {
  return atomic_load_explicit(&successes, memory_order_acquire);
}

cmeta_plugin_status cmeta_plugin_platform_close(
    cmeta_plugin_library *library) {
  int count = atomic_load_explicit(&pending, memory_order_acquire);
  while (count > 0) {
    if (atomic_compare_exchange_weak_explicit(
            &pending, &count, count - 1,
            memory_order_acq_rel, memory_order_acquire)) {
      atomic_fetch_add_explicit(&injected, 1, memory_order_relaxed);
      /* No close, no handle mutation: emulate a failed OS close in the
       * same return contract as the actual Salts POSIX loader. */
      return CMETA_PLUGIN_UNLOAD_FAILED;
    }
  }

  atomic_fetch_add_explicit(&attempts, 1, memory_order_relaxed);
  cmeta_plugin_status status = cmeta_plugin_platform_close_real(library);
  if (status == CMETA_PLUGIN_OK)
    atomic_fetch_add_explicit(&successes, 1, memory_order_relaxed);
  return status;
}

/* Test-only failure of the Registry's lease-release entrypoint. A failed
 * release does not mutate the caller's authoritative lease. The actual pinned
 * Salts implementation handles all successful calls, including recovery. */
static atomic_int release_pending;
static atomic_uint_least64_t release_target_key;
static atomic_int release_injected;
static atomic_int release_native_attempts;

cmeta_plugin_status cmeta_plugin_registry_release_real(
    cmeta_plugin_registry *registry, cmeta_plugin_lease *lease);

static uint64_t provider_plugin_key(cmeta_plugin_ref ref) {
  return ((uint64_t)ref.slot << 32u) | (uint64_t)ref.generation;
}

void provider_release_fault_arm(int failures) {
  atomic_store_explicit(&release_target_key, 0u, memory_order_release);
  atomic_store_explicit(&release_pending, failures, memory_order_release);
}

void provider_release_fault_arm_plugin(cmeta_plugin_ref ref, int failures) {
  atomic_store_explicit(
      &release_target_key, provider_plugin_key(ref), memory_order_release);
  atomic_store_explicit(&release_pending, failures, memory_order_release);
}

int provider_release_fault_injected(void) {
  return atomic_load_explicit(&release_injected, memory_order_acquire);
}

int provider_release_fault_native_attempts(void) {
  return atomic_load_explicit(&release_native_attempts, memory_order_acquire);
}

cmeta_plugin_status cmeta_plugin_registry_release(
    cmeta_plugin_registry *registry, cmeta_plugin_lease *lease) {
  int count = atomic_load_explicit(&release_pending, memory_order_acquire);
  const uint64_t target =
      atomic_load_explicit(&release_target_key, memory_order_acquire);
  /* Other Plugin generations must be able to settle independently even
   * when their drain calls overlap the targeted failing release. */
  const bool matches = target == 0u ||
      (lease != NULL && provider_plugin_key(lease->plugin) == target);
  while (matches && count > 0) {
    if (atomic_compare_exchange_weak_explicit(
            &release_pending, &count, count - 1,
            memory_order_acq_rel, memory_order_acquire)) {
      atomic_fetch_add_explicit(&release_injected, 1, memory_order_relaxed);
      return CMETA_PLUGIN_BUSY;
    }
  }
  atomic_fetch_add_explicit(
      &release_native_attempts, 1, memory_order_relaxed);
  return cmeta_plugin_registry_release_real(registry, lease);
}
