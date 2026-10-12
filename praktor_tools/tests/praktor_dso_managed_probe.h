#ifndef TURBOAGENT_PRAKTOR_MANAGED_DSO_PROBE_H
#define TURBOAGENT_PRAKTOR_MANAGED_DSO_PROBE_H

#include <stdatomic.h>
#include <stdint.h>

/* Test-only shared ABI. This is accessed only while a Plugin Lease is live.
 * The host-owned counter outlives the provider image and any final dlclose. */
typedef struct praktor_dso_managed_probe {
  atomic_int *host_destroy_count;
  atomic_int *host_destroy_entered;
  atomic_int *host_allow_destroy_exit;
  atomic_int *host_destroy_timeout;
  void *host;
  uint64_t (*host_now_ms)(void *host);
  void (*host_yield)(void *host);
  int stopped;
} praktor_dso_managed_probe;

#endif
