#ifndef TURBOAGENT_PROVIDER_CLOSE_FAULT_H
#define TURBOAGENT_PROVIDER_CLOSE_FAULT_H

/* Test-only fault injection at the actual Salts POSIX loader close boundary.
 * This is never included in normal SDK or production Plugin builds. */
void provider_close_fault_arm(int failures);
int provider_close_fault_injected(void);
int provider_close_fault_native_attempts(void);
int provider_close_fault_native_successes(void);

#endif
