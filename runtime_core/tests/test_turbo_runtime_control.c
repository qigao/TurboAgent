#include "tinytest.h"
#include "turbo_runtime_control.h"

#include <stdatomic.h>

#include <salts/thread.h>

typedef struct test_clock_s {
  atomic_uint_fast64_t now_ms;
} test_clock_t;

typedef struct cancel_waiter_s {
  turbo_cancel_token_t *token;
  atomic_int entered;
  turbo_cancel_wait_status_t wait_status;
  turbo_cancel_reason_t reason;
} cancel_waiter_t;

static uint64_t test_clock_now_ms(void *user_data) {
  test_clock_t *clock = (test_clock_t *)user_data;
  return atomic_load_explicit(&clock->now_ms, memory_order_acquire);
}

static void cancel_waiter_run(void *arg) {
  cancel_waiter_t *waiter = (cancel_waiter_t *)arg;
  atomic_store_explicit(&waiter->entered, 1, memory_order_release);
  waiter->wait_status = turbo_cancel_token_wait(waiter->token, TURBO_RUNTIME_WAIT_INFINITE);
  waiter->reason = turbo_cancel_token_reason(waiter->token);
}

spec("runtime cancellation control") {
  it("creates an active token without a deadline") {
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;

    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_not_null(source);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_not_null(token);
    check_int_eq(turbo_cancel_token_check(token), SALTS_OK);
    check_int_eq(turbo_cancel_token_reason(token), TURBO_CANCEL_NONE);
    check_true(turbo_cancel_token_deadline_mono_ms(token) == UINT64_C(0));

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
  }

  it("keeps the first cancellation reason") {
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;

    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
    check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_SHUTDOWN), SALTS_EALREADY);
    check_int_eq(turbo_cancel_token_check(token), SALTS_ECANCELED);
    check_int_eq(turbo_cancel_token_reason(token), TURBO_CANCEL_USER);
    check_int_eq(turbo_cancel_source_reason(source), TURBO_CANCEL_USER);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
  }

  it("materializes an expired monotonic deadline") {
    test_clock_t clock;
    turbo_cancel_source_config_t config = {sizeof(config), TURBO_RUNTIME_CONTROL_ABI_VERSION,
                                           UINT64_C(100), test_clock_now_ms, &clock};
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;

    atomic_init(&clock.now_ms, UINT64_C(100));
    check_int_eq(turbo_cancel_source_create(&config, &source), SALTS_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_int_eq(turbo_cancel_token_check(token), SALTS_ETIMEDOUT);
    check_int_eq(turbo_cancel_token_reason(token), TURBO_CANCEL_DEADLINE);
    check_true(turbo_cancel_token_deadline_mono_ms(token) == UINT64_C(100));
    check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_EALREADY);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
  }

  it("keeps tokens valid after the source is destroyed") {
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_cancel_token_t *retained = NULL;

    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    retained = turbo_cancel_token_retain(token);
    check_ptr_eq(retained, token);

    turbo_cancel_source_destroy(source);
    check_int_eq(turbo_cancel_token_check(token), SALTS_OK);

    turbo_cancel_token_release(retained);
    turbo_cancel_token_release(token);
  }

  it("distinguishes a caller wait timeout from cancellation") {
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;

    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_int_eq(turbo_cancel_token_wait(token, 0), TURBO_CANCEL_WAIT_TIMEOUT);
    check_int_eq(turbo_cancel_token_reason(token), TURBO_CANCEL_NONE);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
  }

  it("wakes every waiter when shutdown is requested") {
    enum { WAITER_COUNT = 2 };
    turbo_cancel_source_t *source = NULL;
    cancel_waiter_t waiters[WAITER_COUNT] = {0};
    salts_thread_t threads[WAITER_COUNT] = {0};
    int i;

    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    for (i = 0; i < WAITER_COUNT; ++i) {
      check_int_eq(turbo_cancel_source_token(source, &waiters[i].token), SALTS_OK);
      atomic_init(&waiters[i].entered, 0);
      check_int_eq(salts_thread_create(&threads[i], cancel_waiter_run, &waiters[i]), SALTS_OK);
    }

    for (i = 0; i < WAITER_COUNT; ++i) {
      while (!atomic_load_explicit(&waiters[i].entered, memory_order_acquire)) {
        salts_thread_yield();
      }
    }
    check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_SHUTDOWN), SALTS_OK);

    for (i = 0; i < WAITER_COUNT; ++i) {
      check_int_eq(salts_thread_join(&threads[i]), SALTS_OK);
      check_int_eq(waiters[i].wait_status, TURBO_CANCEL_WAIT_SIGNALED);
      check_int_eq(waiters[i].reason, TURBO_CANCEL_SHUTDOWN);
      turbo_cancel_token_release(waiters[i].token);
    }
    turbo_cancel_source_destroy(source);
  }

  it("rejects an incompatible configuration without publishing a handle") {
    turbo_cancel_source_config_t config = {sizeof(config), TURBO_RUNTIME_CONTROL_ABI_VERSION + 1U,
                                           0, NULL, NULL};
    turbo_cancel_source_t *source = (turbo_cancel_source_t *)(uintptr_t)1;

    check_int_eq(turbo_cancel_source_create(&config, &source), SALTS_EINVAL);
    check_null(source);
    check_int_eq(turbo_cancel_source_cancel(NULL, TURBO_CANCEL_USER), SALTS_EINVAL);
  }
}
