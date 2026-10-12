#ifndef TURBOAGENT_PRAKTOR_MANAGED_DSO_PROBE_H
#define TURBOAGENT_PRAKTOR_MANAGED_DSO_PROBE_H

#include <stdatomic.h>

/* Test-only shared ABI. This is accessed only while a Plugin Lease is live.
 * The host-owned counter outlives the provider image and any final dlclose. */
typedef struct praktor_dso_managed_probe {
  atomic_int *host_destroy_count;
  int stopped;
} praktor_dso_managed_probe;

#endif
