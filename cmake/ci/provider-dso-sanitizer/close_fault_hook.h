#ifndef TURBOAGENT_PROVIDER_CLOSE_FAULT_H
#define TURBOAGENT_PROVIDER_CLOSE_FAULT_H

#include <salts/plugin.h>

/* Test-only fault injection at the actual Salts POSIX loader close boundary.
 * This is never included in normal SDK or production Plugin builds. */
void provider_close_fault_arm(int failures);
int provider_close_fault_injected(void);
int provider_close_fault_native_attempts(void);
int provider_close_fault_native_successes(void);

/* Fail exactly the next N Plugin Lease releases without consuming a lease. */
void provider_release_fault_arm(int failures);
/* Scope a single non-consuming failure to an exact Plugin slot+generation. */
void provider_release_fault_arm_plugin(cmeta_plugin_ref ref, int failures);
int provider_release_fault_injected(void);
int provider_release_fault_native_attempts(void);

#endif
