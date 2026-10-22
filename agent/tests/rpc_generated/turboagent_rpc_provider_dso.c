#include "turboagent_rpc_provider_dso.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdbool.h>

/* Layout must match the test's DataBind native record projections. */
typedef struct turboagent_rpc_provider_dso_request {
  uint32_t left, right, scale;
  uint8_t presence;
} turboagent_rpc_provider_dso_request;
typedef struct turboagent_rpc_provider_dso_response {
  uint32_t sum;
} turboagent_rpc_provider_dso_response;

/* This code is compiled only into the fixture shared object. A host-side
 * replacement would not prove that Server pins executed DSO code on stop. */
static bool DATA_BIND_NATIVE_CALL turboagent_rpc_provider_dso_invoke(
    void *context, void *return_storage, void *const *params,
    size_t param_count) {
  turboagent_rpc_provider_dso_probe *probe =
      (turboagent_rpc_provider_dso_probe *)context;
  const turboagent_rpc_provider_dso_request *request;
  turboagent_rpc_provider_dso_response *response;
  if (return_storage == NULL || params == NULL || param_count != 2u ||
      params[0] == NULL || params[1] == NULL)
    return false;
  if (probe != NULL)
    atomic_fetch_add_explicit(&probe->invoked, 1, memory_order_acq_rel);
  if (probe != NULL &&
      atomic_load_explicit(&probe->enabled, memory_order_acquire)) {
    const uint64_t deadline = cmeta_monotonic_ms() + 3000u;
    atomic_fetch_add_explicit(&probe->entered, 1, memory_order_acq_rel);
    while (!atomic_load_explicit(&probe->release, memory_order_acquire)) {
      if (cmeta_monotonic_ms() >= deadline) return false;
      cmeta_thread_yield();
    }
  }
  request = (const turboagent_rpc_provider_dso_request *)params[0];
  response = (turboagent_rpc_provider_dso_response *)params[1];
  if (request->left == 99u) {
    *(int *)return_storage = 7;
    return true;
  }
  response->sum = request->left + request->right * request->scale;
  *(int *)return_storage = 0;
  return true;
}

static const turboagent_rpc_provider_dso_offer TURBOAGENT_RPC_DSO_OFFER = {
    turboagent_rpc_provider_dso_invoke};

const turboagent_rpc_provider_dso_offer *
turboagent_rpc_provider_dso_get_offer(void) {
  return &TURBOAGENT_RPC_DSO_OFFER;
}

#define TURBOAGENT_RPC_PROVIDER_DSO_EXPORTS(X) \
  X(function, turboagent_rpc_provider_dso_get_offer, \
    TURBOAGENT_RPC_PROVIDER_DSO_EXPORT, TURBOAGENT_RPC_PROVIDER_DSO_CONTRACT, \
    TURBOAGENT_RPC_PROVIDER_DSO_VERSION, 0)

CMETA_PLUGIN_DECLARE(
    turboagent_rpc_provider_dso, "test.turboagent.rpc.generation.dso", (1, 0, 0),
    TURBOAGENT_RPC_PROVIDER_DSO_EXPORTS, CMETA_PLUGIN_PASSIVE());
