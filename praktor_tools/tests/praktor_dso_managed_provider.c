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
  if (probe == &managed && probe->host_destroy_count)
    atomic_fetch_add_explicit(
        probe->host_destroy_count, 1, memory_order_acq_rel);
}

CMETA_PLUGIN_DECLARE_EMPTY(
    praktor_dso_managed_close_fault,
    "test.turboagent.dso.managed.close-fault",
    (1,0,0),
    CMETA_PLUGIN_LIFECYCLE(
        &managed, managed_start, managed_stop, managed_quiet,
        managed_destroy));
