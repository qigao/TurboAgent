#include "tinytest.h"
#include "turbo_cli.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { COMPLETION_TEST_TIMEOUT_MS = 5000 };

typedef struct completion_model_s {
  atomic_int entered;
  atomic_int open;
} completion_model_t;

static int completion_model(const char *request, char **out, void *user) {
  static const char response[] =
      "{\"id\":\"completion-test\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"late model answer\"}]}]}";
  completion_model_t *model = user;
  uint64_t deadline = cmeta_monotonic_ms() + COMPLETION_TEST_TIMEOUT_MS;
  (void)request;
  *out = NULL;
  atomic_store_explicit(&model->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&model->open, memory_order_acquire)) {
    if (cmeta_monotonic_ms() >= deadline) return -1;
    cmeta_thread_yield();
  }
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

static void completion_read(FILE *stream, char *text, size_t capacity) {
  size_t count;
  rewind(stream);
  count = fread(text, 1, capacity - 1, stream);
  text[count] = '\0';
}

static void completion_wait(cmeta_threadpool_t *pool, turbo_agent_harness_execution_t *execution,
                            turbo_agent_execution_status_t expected) {
  turbo_agent_execution_status_t status;
  int run_rc = -1;
  check_equal(turbo_agent_harness_execution_wait(execution, COMPLETION_TEST_TIMEOUT_MS), SALTS_OK);
  cmeta_threadpool_wait(pool);
  check_equal(turbo_agent_harness_execution_get_status(execution, &status), SALTS_OK);
  check_equal(status, expected);
  /* Persisting a cancelled/paused/timed-out run can succeed with return 0. */
  check_equal(turbo_agent_harness_execution_result_code(execution, &run_rc), SALTS_OK);
  check_equal(run_rc, 0);
}

spec("CLI execution completion") {
  static cmeta_threadpool_t *pool;
  static turbo_agent_harness_t *harness;
  static turbo_agent_harness_execution_t *execution;
  static completion_model_t model;
  static FILE *output, *diagnostics;
  static json_value_t *state;
  static turbo_agent_harness_run_options_t options;

  before_each() {
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_harness_config_t harness_config;
    atomic_init(&model.entered, 0);
    atomic_init(&model.open, 1);
    pool = cmeta_threadpool_create_with_config(&pool_config);
    check_not_null(pool);
    session_config.agent_config.model = "test-model";
    session_config.agent_config.transport_fn = completion_model;
    session_config.agent_config.transport_user_data = &model;
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    app_config.session_config = &session_config;
    turbo_agent_harness_config_init(&harness_config);
    harness_config.app_config = &app_config;
    harness_config.executor = pool;
    harness = turbo_agent_harness_create(&harness_config);
    check_not_null(harness);
    output = tmpfile();
    diagnostics = tmpfile();
    check_not_null(output);
    check_not_null(diagnostics);
    turbo_agent_harness_run_options_init(&options);
  }

  after_each() {
    atomic_store_explicit(&model.open, 1, memory_order_release);
    if (execution) {
      turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_harness_execution_wait(execution, UINT64_MAX);
      turbo_agent_harness_execution_release(execution);
      execution = NULL;
    }
    if (pool) cmeta_threadpool_wait(pool);
    turbo_agent_harness_release(harness);
    harness = NULL;
    if (pool) cmeta_threadpool_destroy(pool);
    pool = NULL;
    json_free(state);
    state = NULL;
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    output = diagnostics = NULL;
  }

  it("prints and transfers only a completed result, exactly once") {
    char text[128];
    json_value_t *duplicate = NULL;
    check_equal(turbo_agent_harness_start_text(harness, "hello", &options, &execution), SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &state),
                0);
    check_equal(turbo_agent_state_final_answer_text(state), "late model answer");
    check_equal(
        turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &duplicate), 1);
    check_null(duplicate);
    completion_read(output, text, sizeof(text));
    check_equal(text, "late model answer\n");
  }

  it("suppresses a late model answer after cancellation and permits the next run") {
    char text[2048];
    uint64_t deadline = cmeta_monotonic_ms() + COMPLETION_TEST_TIMEOUT_MS;
    atomic_store(&model.open, 0);
    check_equal(turbo_agent_harness_start_text(harness, "cancel this", &options, &execution),
                SALTS_OK);
    while (!atomic_load_explicit(&model.entered, memory_order_acquire) &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    check_true(atomic_load_explicit(&model.entered, memory_order_acquire));
    check_equal(turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_USER), SALTS_OK);
    atomic_store_explicit(&model.open, 1, memory_order_release);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_CANCELLED);
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &state),
                1);
    check_null(state);
    completion_read(output, text, sizeof(text));
    check_equal(text, "");
    completion_read(diagnostics, text, sizeof(text));
    check_contains(text, "execution_status=cancelled");
    check_contains(text, "Run cancelled.");
    check_null(strstr(text, "late model answer"));
    turbo_agent_harness_execution_release(execution);
    execution = NULL;

    check_equal(turbo_agent_harness_start_text(harness, "next task", &options, &execution),
                SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &state),
                0);
    completion_read(output, text, sizeof(text));
    check_equal(text, "late model answer\n");
  }

  it("honors a signal observed after worker completion but before printing") {
    char text[2048];
    check_equal(turbo_agent_harness_start_text(harness, "hello", &options, &execution), SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 1, output, diagnostics, &state),
                130);
    check_null(state);
    completion_read(output, text, sizeof(text));
    check_equal(text, "");
    completion_read(diagnostics, text, sizeof(text));
    check_contains(text, "Run interrupted by user.");
  }

  it("reports an execution deadline separately from a transport failure") {
    char text[2048];
    options.deadline_mono_ms = cmeta_monotonic_ms();
    check_equal(turbo_agent_harness_start_text(harness, "too late", &options, &execution),
                SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_TIMED_OUT);
    check_false(atomic_load(&model.entered));
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &state),
                1);
    check_null(state);
    completion_read(output, text, sizeof(text));
    check_equal(text, "");
    completion_read(diagnostics, text, sizeof(text));
    check_contains(text, "execution_status=timed_out");
    check_contains(text, "Run deadline exceeded.");
  }

  it("does not print partial output when the workflow pauses before its end") {
    char text[2048];
    const char *nodes[] = {"end"};
    turbo_graph_run_options_t graph_options = {.interrupt_before_nodes = nodes,
                                               .interrupt_before_count = 1};
    options.graph_options = &graph_options;
    check_equal(turbo_agent_harness_start_text(harness, "pause", &options, &execution), SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_INTERRUPTED);
    check_equal(turbo_cli_complete_execution(execution, SALTS_OK, 0, output, diagnostics, &state),
                1);
    check_null(state);
    completion_read(output, text, sizeof(text));
    check_equal(text, "");
    completion_read(diagnostics, text, sizeof(text));
    check_contains(text, "execution_status=interrupted");
    check_contains(text, "Run paused before completion.");
  }

  it("does not report success after a failed wait even if the worker then completes") {
    char text[128];
    check_equal(turbo_agent_harness_start_text(harness, "hello", &options, &execution), SALTS_OK);
    completion_wait(pool, execution, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(
        turbo_cli_complete_execution(execution, SALTS_EINVAL, 0, output, diagnostics, &state), 1);
    check_null(state);
    completion_read(output, text, sizeof(text));
    check_equal(text, "");
  }
}
