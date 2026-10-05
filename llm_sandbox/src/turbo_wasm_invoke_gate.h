#ifndef TURBO_WASM_INVOKE_GATE_H
#define TURBO_WASM_INVOKE_GATE_H

#include <salts/thread.h>

#include "turbo_tool.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal single-owner gate for one TurboWasm instance.
 *
 * The mutex protects gate state only; it is never held across guest execution.
 * A caller that acquires the gate owns the instance until release.
 */
typedef struct turbo_wasm_invoke_gate_s {
  salts_mutex_t mutex;
  salts_cond_t changed;
  int active;
} turbo_wasm_invoke_gate_t;

int turbo_wasm_invoke_gate_init(turbo_wasm_invoke_gate_t *gate);
void turbo_wasm_invoke_gate_destroy(turbo_wasm_invoke_gate_t *gate);

/**
 * Acquire exclusive instance ownership.
 *
 * Queue time is part of the invocation deadline. Cancellation/deadline is
 * checked before ownership transfer and while queued. A non-context caller
 * waits until the owner releases the gate.
 */
turbo_tool_status_t turbo_wasm_invoke_gate_acquire(
    turbo_wasm_invoke_gate_t *gate,
    const turbo_tool_execution_context_t *context);

void turbo_wasm_invoke_gate_release(turbo_wasm_invoke_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif
