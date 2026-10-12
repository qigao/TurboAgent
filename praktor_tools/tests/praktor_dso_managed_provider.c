#include "praktor_dso_managed_probe.h"
#include <salts/plugin_decl.h>

static praktor_dso_managed_probe managed = {0};

static cmeta_plugin_status CMETA_PLUGIN_CALL managed_start(void *self) {
  return self == &managed ? CMETA_PLUGIN_OK : CMETA_PLUGIN_INVALID_ARGUMENT;
}

static cmeta_plugin_status CMETA_PLUGIN_CALL managed_stop(void *self) {
  praktor_dso_managed_probe *probe = (praktor_dso_managed_probe *)self;
  if (probe != &managed) return CMETA_PLUGIN_INVALID_ARGUMENT;
  probe->stopped = 1;
  return CMETA_PLUGIN_OK;
}

static bool CMETA_PLUGIN_CALL managed_quiet(const void *self) {
  const praktor_dso_managed_probe *probe =
      (const praktor_dso_managed_probe *)self;
  return probe == &managed && probe->stopped == 1;
}

static void CMETA_PLUGIN_CALL managed_destroy(void *self) {
  praktor_dso_managed_probe *probe = (praktor_dso_managed_probe *)self;
  uint64_t deadline;
  if (probe != &managed) return;
  if (probe->host_destroy_count)
    atomic_fetch_add_explicit(
        probe->host_destroy_count, 1, memory_order_acq_rel);
  if (!probe->host_destroy_entered || !probe->host_allow_destroy_exit)
    return;

  /* Test-only deterministic interleaving: Registry has marked the slot
   * unloading, but the DSO's destroy callback has not returned. The host
   * calls another unload concurrently and must observe BUSY, not re-enter
   * destroy or close the library while this code executes. */
  atomic_store_explicit(
      probe->host_destroy_entered, 1, memory_order_release);
  if (!probe->host_now_ms || !probe->host_yield) return;
  deadline = probe->host_now_ms(probe->host) + UINT64_C(10000);
  while (!atomic_load_explicit(
             probe->host_allow_destroy_exit, memory_order_acquire) &&
         probe->host_now_ms(probe->host) < deadline)
    probe->host_yield(probe->host);
  if (!atomic_load_explicit(
          probe->host_allow_destroy_exit, memory_order_acquire) &&
      probe->host_destroy_timeout)
    atomic_store_explicit(
        probe->host_destroy_timeout, 1, memory_order_release);
}

CMETA_PLUGIN_DECLARE_EMPTY(
    praktor_dso_managed_close_fault,
    "test.turboagent.dso.managed.close-fault",
    (1,0,0),
    CMETA_PLUGIN_LIFECYCLE(
        &managed, managed_start, managed_stop, managed_quiet,
        managed_destroy));
