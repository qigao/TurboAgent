#include "turbo_wasm_invoke_gate.h"

#include <salts/clock.h>

#include <stdint.h>
#include <string.h>

enum {
  TURBO_WASM_INVOKE_GATE_POLL_MS = 25
};

static turbo_tool_status_t gate_context_status(
    const turbo_tool_execution_context_t *context) {
  if (!context) return TURBO_TOOL_OK;

  if (context->cancel_token &&
      turbo_cancel_token_check(context->cancel_token) != 0) {
    return turbo_cancel_token_reason(context->cancel_token) ==
                   TURBO_CANCEL_DEADLINE
               ? TURBO_TOOL_DEADLINE_EXCEEDED
               : TURBO_TOOL_CANCELLED;
  }

  if (context->deadline_mono_ms &&
      salts_monotonic_ms() >= context->deadline_mono_ms) {
    return TURBO_TOOL_DEADLINE_EXCEEDED;
  }

  return TURBO_TOOL_OK;
}

static uint64_t gate_wait_ns(
    const turbo_tool_execution_context_t *context) {
  uint64_t wait_ms = TURBO_WASM_INVOKE_GATE_POLL_MS;

  if (context && context->deadline_mono_ms) {
    uint64_t now_ms = salts_monotonic_ms();
    uint64_t remaining_ms;
    if (now_ms >= context->deadline_mono_ms) return 0u;
    remaining_ms = context->deadline_mono_ms - now_ms;
    if (remaining_ms < wait_ms) wait_ms = remaining_ms;
  }

  return wait_ms * UINT64_C(1000000);
}

int turbo_wasm_invoke_gate_init(turbo_wasm_invoke_gate_t *gate) {
  if (!gate) return -1;
  memset(gate, 0, sizeof(*gate));

  salts_mutex_init(&gate->mutex);
  if (!gate->mutex) return -1;

  salts_cond_init(&gate->changed);
  if (!gate->changed) {
    salts_mutex_destroy(&gate->mutex);
    return -1;
  }

  return 0;
}

void turbo_wasm_invoke_gate_destroy(turbo_wasm_invoke_gate_t *gate) {
  if (!gate) return;
  salts_cond_destroy(&gate->changed);
  salts_mutex_destroy(&gate->mutex);
  memset(gate, 0, sizeof(*gate));
}

turbo_tool_status_t turbo_wasm_invoke_gate_acquire(
    turbo_wasm_invoke_gate_t *gate,
    const turbo_tool_execution_context_t *context) {
  turbo_tool_status_t status;

  if (!gate || !gate->mutex || !gate->changed) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  salts_mutex_lock(&gate->mutex);
  for (;;) {
    status = gate_context_status(context);
    if (status != TURBO_TOOL_OK) {
      salts_mutex_unlock(&gate->mutex);
      return status;
    }

    if (!gate->active) {
      gate->active = 1;
      salts_mutex_unlock(&gate->mutex);
      return TURBO_TOOL_OK;
    }

    if (!context) {
      salts_cond_wait(&gate->changed, &gate->mutex);
      continue;
    }

    {
      uint64_t wait_ns = gate_wait_ns(context);
      if (wait_ns == 0u) continue;
      (void)salts_cond_timedwait(&gate->changed, &gate->mutex, wait_ns);
    }
  }
}

void turbo_wasm_invoke_gate_release(turbo_wasm_invoke_gate_t *gate) {
  if (!gate || !gate->mutex || !gate->changed) return;

  salts_mutex_lock(&gate->mutex);
  gate->active = 0;
  salts_cond_signal(&gate->changed);
  salts_mutex_unlock(&gate->mutex);
}
