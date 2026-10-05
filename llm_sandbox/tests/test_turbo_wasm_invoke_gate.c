#include "tinytest.h"

#include "../src/turbo_wasm_invoke_gate.h"

#include <salts/clock.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <string.h>

typedef struct wasm_gate_waiter_s {
  turbo_wasm_invoke_gate_t *gate;
  const turbo_tool_execution_context_t *context;
  atomic_int entered;
  atomic_int acquired;
  turbo_tool_status_t status;
} wasm_gate_waiter_t;

static void wasm_gate_waiter_run(void *arg) {
  wasm_gate_waiter_t *waiter = (wasm_gate_waiter_t *)arg;
  atomic_store_explicit(&waiter->entered, 1, memory_order_release);
  waiter->status =
      turbo_wasm_invoke_gate_acquire(waiter->gate, waiter->context);
  if (waiter->status == TURBO_TOOL_OK) {
    atomic_store_explicit(&waiter->acquired, 1, memory_order_release);
    turbo_wasm_invoke_gate_release(waiter->gate);
  }
}

static void wasm_gate_wait_until_entered(wasm_gate_waiter_t *waiter) {
  while (!atomic_load_explicit(&waiter->entered, memory_order_acquire)) {
    salts_thread_yield();
  }
}

spec("TurboWasm single-owner invocation gate") {
  it("expires a queued invocation before ownership transfer") {
    turbo_wasm_invoke_gate_t gate = {0};
    turbo_tool_execution_context_t context = {0};
    wasm_gate_waiter_t waiter;
    salts_thread_t thread = NULL;

    check_equal(turbo_wasm_invoke_gate_init(&gate), 0);
    check_equal(
        turbo_wasm_invoke_gate_acquire(&gate, NULL),
        TURBO_TOOL_OK);

    memset(&waiter, 0, sizeof(waiter));
    waiter.gate = &gate;
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.deadline_mono_ms = salts_monotonic_ms() + UINT64_C(100);
    waiter.context = &context;
    waiter.status = TURBO_TOOL_ERROR;
    atomic_init(&waiter.entered, 0);
    atomic_init(&waiter.acquired, 0);

    check_equal(
        salts_thread_create(&thread, wasm_gate_waiter_run, &waiter),
        SALTS_OK);
    wasm_gate_wait_until_entered(&waiter);

    /*
     * Keep the gate owned while joining. The waiter can finish only by
     * observing its absolute deadline; a lock-only implementation would hang.
     */
    check_equal(salts_thread_join(&thread), SALTS_OK);
    check_equal(waiter.status, TURBO_TOOL_DEADLINE_EXCEEDED);
    check_false(atomic_load_explicit(
        &waiter.acquired, memory_order_acquire));

    turbo_wasm_invoke_gate_release(&gate);
    salts_thread_destroy(&thread);
    turbo_wasm_invoke_gate_destroy(&gate);
  }

  it("observes cancellation while queued without entering the guest owner slot") {
    turbo_wasm_invoke_gate_t gate = {0};
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    wasm_gate_waiter_t waiter;
    salts_thread_t thread = NULL;

    check_equal(turbo_wasm_invoke_gate_init(&gate), 0);
    check_equal(
        turbo_wasm_invoke_gate_acquire(&gate, NULL),
        TURBO_TOOL_OK);
    check_equal(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);

    memset(&waiter, 0, sizeof(waiter));
    waiter.gate = &gate;
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;
    waiter.context = &context;
    waiter.status = TURBO_TOOL_ERROR;
    atomic_init(&waiter.entered, 0);
    atomic_init(&waiter.acquired, 0);

    check_equal(
        salts_thread_create(&thread, wasm_gate_waiter_run, &waiter),
        SALTS_OK);
    wasm_gate_wait_until_entered(&waiter);
    check_equal(
        turbo_cancel_source_cancel(source, TURBO_CANCEL_USER),
        SALTS_OK);

    /*
     * The owner remains active through join. Cancellation must therefore wake
     * through the bounded condition-variable polling path, not owner release.
     */
    check_equal(salts_thread_join(&thread), SALTS_OK);
    check_equal(waiter.status, TURBO_TOOL_CANCELLED);
    check_false(atomic_load_explicit(
        &waiter.acquired, memory_order_acquire));

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    turbo_wasm_invoke_gate_release(&gate);
    salts_thread_destroy(&thread);
    turbo_wasm_invoke_gate_destroy(&gate);
  }

  it("transfers single ownership only after the active owner releases") {
    turbo_wasm_invoke_gate_t gate = {0};
    wasm_gate_waiter_t waiter;
    salts_thread_t thread = NULL;

    check_equal(turbo_wasm_invoke_gate_init(&gate), 0);
    check_equal(
        turbo_wasm_invoke_gate_acquire(&gate, NULL),
        TURBO_TOOL_OK);

    memset(&waiter, 0, sizeof(waiter));
    waiter.gate = &gate;
    waiter.context = NULL;
    waiter.status = TURBO_TOOL_ERROR;
    atomic_init(&waiter.entered, 0);
    atomic_init(&waiter.acquired, 0);

    check_equal(
        salts_thread_create(&thread, wasm_gate_waiter_run, &waiter),
        SALTS_OK);
    wasm_gate_wait_until_entered(&waiter);

    check_false(atomic_load_explicit(
        &waiter.acquired, memory_order_acquire));

    turbo_wasm_invoke_gate_release(&gate);
    check_equal(salts_thread_join(&thread), SALTS_OK);
    check_equal(waiter.status, TURBO_TOOL_OK);
    check_true(atomic_load_explicit(
        &waiter.acquired, memory_order_acquire));

    salts_thread_destroy(&thread);
    turbo_wasm_invoke_gate_destroy(&gate);
  }
}
