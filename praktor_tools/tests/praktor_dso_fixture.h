#ifndef TURBO_AGENT_PRAKTOR_DSO_FIXTURE_H
#define TURBO_AGENT_PRAKTOR_DSO_FIXTURE_H

/* Test-only ABI shared by the native host and two independently loaded DSOs.
 * The host supplies cancellation and clock callbacks so no second runtime or
 * background thread is embedded in either Provider module. */
#include <salts/component_plugin_abi.h>
#include <stdatomic.h>
#include <stdint.h>

#define PRAKTOR_DSO_PROVIDER_EXPORT_ID "praktor-test-provider"

typedef struct praktor_dso_control {
  void *host;
  int (*cancelled)(void *host);
  uint64_t (*now_ms)(void *host);
  void (*yield)(void *host);
  atomic_int entered;
  atomic_int exited;
  int block_until_cancel;
} praktor_dso_control;

#define PRAKTOR_DSO_GATE_METHODS(X, I) \
  X(I, R1, int, run, praktor_dso_control *, control)

CMETA_INTERFACE(praktor_dso_gate, PRAKTOR_DSO_GATE_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(praktor_dso_gate);

#endif /* TURBO_AGENT_PRAKTOR_DSO_FIXTURE_H */
