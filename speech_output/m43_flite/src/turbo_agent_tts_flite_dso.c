#include "turbo_agent_tts_flite_dso.h"

/* Test observer does not confer DSO ownership. It is only sampled after
 * dlclose returned on the caller's serialized control thread. */
static unsigned *unload_counter = 0;

static void test_set_unload_counter(unsigned *counter) {
  unload_counter = counter;
}

#if defined(__GNUC__)
__attribute__((destructor))
#endif
static void m43_dso_unload_probe(void) {
  if (unload_counter) ++*unload_counter;
}

static const turbo_m43_flite_dso_api_t entry = {
  .struct_size = sizeof(turbo_m43_flite_dso_api_t),
  .abi_version = TURBO_M43_FLITE_DSO_ABI_VERSION,
  .config_init = turbo_m43_flite_config_init,
  .create = turbo_m43_flite_create,
  .bind = turbo_m43_flite_bind,
  .destroy = turbo_m43_flite_destroy,
  .test_set_unload_counter = test_set_unload_counter
};

#if defined(__GNUC__)
__attribute__((visibility("default")))
#endif
const turbo_m43_flite_dso_api_t *turbo_m43_flite_dso_entry(void) {
  return &entry;
}
