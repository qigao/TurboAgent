#include "tinytest.h"
#include "../src/turbo_agent_tool_executor_internal.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdatomic.h>

/* Compile the actual gate with a test-only condition scheduler. Public symbols
 * are renamed after their headers are loaded, so the installed Agent and its
 * ABI stay untouched, including on Windows. No gate logic is copied here. */
typedef struct wakeup_schedule_s {
  cmeta_cond_t stopped;
  cmeta_cond_t survivor;
  atomic_int stopped_waiting;
  atomic_int survivor_waiting;
  int stopped_ready;
  int survivor_ready;
  int signals;
} wakeup_schedule_t;

static wakeup_schedule_t schedule;

static void wakeup_wait(cmeta_cond_t *cond, cmeta_mutex_t *mutex) {
  (void)cond;
  atomic_store(&schedule.survivor_waiting, 1);
  while (!schedule.survivor_ready)
    cmeta_cond_wait(&schedule.survivor, mutex);
}

static int wakeup_timedwait(cmeta_cond_t *cond, cmeta_mutex_t *mutex,
                            uint64_t timeout_ns) {
  (void)cond;
  (void)timeout_ns;
  atomic_store(&schedule.stopped_waiting, 1);
  /* Model a descheduled timed waiter: it next runs on the owner's signal,
   * after its token has stopped. Real conditions still release/reacquire the
   * production mutex; predicates exclude spurious wakeups from the schedule. */
  while (!schedule.stopped_ready)
    cmeta_cond_wait(&schedule.stopped, mutex);
  return SALTS_OK;
}

static void wakeup_signal(cmeta_cond_t *cond) {
  (void)cond;
  /* POSIX/Windows conditions need not choose a particular waiter. Force the
   * legal adverse choice: owner -> stopped waiter -> token-free survivor. */
  if (++schedule.signals == 1) {
    schedule.stopped_ready = 1;
    cmeta_cond_signal(&schedule.stopped);
  } else {
    schedule.survivor_ready = 1;
    cmeta_cond_signal(&schedule.survivor);
  }
}

#define turbo_agent_tool_executor_config_init wakeup_executor_config_init
#define turbo_agent_tool_executor_create wakeup_executor_create
#define turbo_agent_tool_executor_destroy wakeup_executor_destroy
#define turbo_agent_tool_executor_configure wakeup_executor_configure
#define turbo_agent_tool_executor_execute wakeup_executor_execute
#define cmeta_cond_wait wakeup_wait
#define cmeta_cond_timedwait wakeup_timedwait
#define cmeta_cond_signal wakeup_signal
#include "../src/turbo_agent_tool_executor.c"
#undef cmeta_cond_signal
#undef cmeta_cond_timedwait
#undef cmeta_cond_wait
#undef turbo_agent_tool_executor_execute
#undef turbo_agent_tool_executor_configure
#undef turbo_agent_tool_executor_destroy
#undef turbo_agent_tool_executor_create
#undef turbo_agent_tool_executor_config_init

typedef struct wakeup_caller_s {
  turbo_agent_tool_executor_t *executor;
  const turbo_cancel_token_t *token;
  turbo_tool_status_t status;
  atomic_int done;
} wakeup_caller_t;

static void wakeup_acquire(void *user_data) {
  wakeup_caller_t *caller = user_data;
  caller->status = turbo_agent_tool_batch_acquire(caller->executor, caller->token);
  if (caller->status == TURBO_TOOL_OK)
    turbo_agent_tool_batch_release(caller->executor);
  atomic_store(&caller->done, 1);
}

static int wakeup_observe(atomic_int *value) {
  uint64_t deadline = cmeta_monotonic_ms() + 2000;
  while (!atomic_load(value)) {
    if (cmeta_monotonic_ms() >= deadline) return 0;
    cmeta_sleep_ms(1);
  }
  return 1;
}

static uint64_t wakeup_clock(void *user_data) {
  return atomic_load((atomic_uint_fast64_t *)user_data);
}

spec("ToolExecutor stopped waiter wakeup handoff") {
  for (int deadline_case = 0; deadline_case < 2; ++deadline_case) {
    it("wakes a token-free third caller after the signaled waiter hits %s",
       deadline_case ? "deadline" : "cancellation") {
      turbo_agent_tool_executor_t executor = {0};
      turbo_cancel_source_config_t config = {0};
      turbo_cancel_source_t *source = NULL;
      turbo_cancel_token_t *token = NULL;
      atomic_uint_fast64_t now;
      wakeup_caller_t callers[2] = {0};
      cmeta_thread_t threads[2] = {0};
      int completed_before_rescue;

      atomic_init(&now, cmeta_monotonic_ms());
      atomic_init(&schedule.stopped_waiting, 0);
      atomic_init(&schedule.survivor_waiting, 0);
      schedule.stopped_ready = 0;
      schedule.survivor_ready = 0;
      schedule.signals = 0;
      cmeta_cond_init(&schedule.stopped);
      cmeta_cond_init(&schedule.survivor);
      cmeta_mutex_init(&executor.batch_mutex);
      cmeta_cond_init(&executor.batch_changed);
      check_not_null(schedule.stopped);
      check_not_null(schedule.survivor);
      check_not_null(executor.batch_mutex);
      check_not_null(executor.batch_changed);

      config.struct_size = sizeof(config);
      config.abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION;
      config.monotonic_ms = wakeup_clock;
      config.clock_user_data = &now;
      if (deadline_case) config.deadline_mono_ms = atomic_load(&now) + 60000;
      check_equal(turbo_cancel_source_create(&config, &source), SALTS_OK);
      check_equal(turbo_cancel_source_token(source, &token), SALTS_OK);

      /* The test thread is caller one and owns the active batch. Both other
       * callers must reach their actual wait before it stops/releases. */
      check_equal(turbo_agent_tool_batch_acquire(&executor, NULL), TURBO_TOOL_OK);
      for (int i = 0; i < 2; ++i) {
        callers[i].executor = &executor;
        callers[i].token = i == 0 ? token : NULL;
        atomic_init(&callers[i].done, 0);
        check_equal(cmeta_thread_create(&threads[i], wakeup_acquire, &callers[i]),
                    SALTS_OK);
      }
      check_true(wakeup_observe(&schedule.stopped_waiting));
      check_true(wakeup_observe(&schedule.survivor_waiting));
      check_false(atomic_load(&callers[0].done));
      check_false(atomic_load(&callers[1].done));
      if (deadline_case)
        atomic_store(&now, config.deadline_mono_ms);
      else
        check_equal(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
      turbo_agent_tool_batch_release(&executor);
      check_true(wakeup_observe(&callers[0].done));
      completed_before_rescue = wakeup_observe(&callers[1].done);

      /* A missing handoff fails a bounded assertion rather than hanging join.
       * This is cleanup only and cannot satisfy completed_before_rescue. */
      cmeta_mutex_lock(&executor.batch_mutex);
      schedule.stopped_ready = 1;
      schedule.survivor_ready = 1;
      cmeta_cond_signal(&schedule.stopped);
      cmeta_cond_signal(&schedule.survivor);
      cmeta_mutex_unlock(&executor.batch_mutex);
      for (int i = 0; i < 2; ++i)
        check_equal(cmeta_thread_join(&threads[i]), SALTS_OK);

      check_true(completed_before_rescue);
      check_equal(callers[0].status, deadline_case ? TURBO_TOOL_DEADLINE_EXCEEDED
                                                 : TURBO_TOOL_CANCELLED);
      check_equal(callers[1].status, TURBO_TOOL_OK);
      check_equal(schedule.signals, 3);
      check_equal(executor.batch_active, 0);
      turbo_cancel_token_release(token);
      turbo_cancel_source_destroy(source);
      cmeta_cond_destroy(&executor.batch_changed);
      cmeta_mutex_destroy(&executor.batch_mutex);
      cmeta_cond_destroy(&schedule.survivor);
      cmeta_cond_destroy(&schedule.stopped);
    }
  }
}
