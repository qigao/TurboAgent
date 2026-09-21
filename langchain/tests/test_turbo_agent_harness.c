#include "tinytest.h"

#include "turbo_agent_harness.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct harness_transport_gate_s {
  atomic_int entered;
  atomic_int open;
} harness_transport_gate_t;

static void harness_count_event(const json_value_t *event, void *user_data) {
  atomic_int *count = (atomic_int *)user_data;

  if (event && count) {
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
  }
}

static int harness_copy_response(char **out_response_json) {
  const char *response = "{\"id\":\"resp_harness\",\"output\":[{\"type\":\"message\","
                         "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                         "\"text\":\"ok\"}]}]}";
  size_t size;

  if (!out_response_json) {
    return -1;
  }
  size = strlen(response) + 1;
  *out_response_json = (char *)malloc(size);
  if (!*out_response_json) {
    return -1;
  }
  memcpy(*out_response_json, response, size);
  return 0;
}

static int harness_success_transport(const char *request_json, char **out_response_json,
                                     void *user_data) {
  (void)request_json;
  (void)user_data;
  return harness_copy_response(out_response_json);
}

static int harness_review_transport(const char *request_json, char **out_response_json,
                                    void *user_data) {
  const char *planner_response = "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\","
                                 "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                                 "\"text\":\"{\\\"steps\\\":[\\\"say ok\\\"]}\"}]}]}";
  const char *executor_response = "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\","
                                  "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                                  "\"text\":\"ok\"}]}]}";
  const char *response = request_json && strstr(request_json, "Execute plan step")
                             ? executor_response
                             : planner_response;
  size_t size;

  (void)user_data;
  if (!out_response_json) {
    return -1;
  }
  size = strlen(response) + 1;
  *out_response_json = (char *)malloc(size);
  if (!*out_response_json) {
    return -1;
  }
  memcpy(*out_response_json, response, size);
  return 0;
}

static int harness_gated_transport(const char *request_json, char **out_response_json,
                                   void *user_data) {
  harness_transport_gate_t *gate = (harness_transport_gate_t *)user_data;

  (void)request_json;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire)) {
    turbo_thread_yield();
  }
  return harness_copy_response(out_response_json);
}

static turbo_agent_harness_t *
harness_create_kind(turbo_threadpool_t *executor, turbo_agent_transport_fn transport,
                    void *transport_user_data, turbo_agent_session_workflow_kind_t workflow_kind) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t harness_config;

  session_config.runtime_store = turbo_agent_runtime_store_memory_create();
  session_config.agent_config.model = "gpt-5.4";
  session_config.agent_config.transport_fn = transport;
  session_config.agent_config.transport_user_data = transport_user_data;
  session_config.workflow_kind = workflow_kind;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&harness_config);
  harness_config.app_config = &app_config;
  harness_config.executor = executor;
  return turbo_agent_harness_create(&harness_config);
}

static turbo_agent_harness_t *harness_create(turbo_threadpool_t *executor,
                                             turbo_agent_transport_fn transport,
                                             void *transport_user_data) {
  return harness_create_kind(executor, transport, transport_user_data,
                             TURBO_AGENT_SESSION_WORKFLOW_LOOP);
}

spec("turbo agent harness") {
  it("reports startup diagnostics and completes one asynchronous text run") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness = harness_create(pool, harness_success_transport, NULL);
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_agent_harness_execution_t *next_execution = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    json_value_t *diagnostics = NULL;
    json_value_t *harness_capabilities;
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    const char *run_id;
    char *text = NULL;
    atomic_int event_count;
    turbo_agent_harness_run_options_t run_options;

    atomic_init(&event_count, 0);
    turbo_agent_harness_run_options_init(&run_options);
    run_options.event_sink = harness_count_event;
    run_options.event_sink_user_data = &event_count;
    check_not_null(pool);
    check_not_null(harness);
    check_int_eq(turbo_agent_harness_get_startup_diagnostics(harness, &diagnostics), SALTS_OK);
    check_true(turbo_json_get_bool(diagnostics, "ok", false));
    harness_capabilities = turbo_json_object_get(diagnostics, "harness");
    check_not_null(harness_capabilities);
    check_true(turbo_json_get_bool(harness_capabilities, "has_async_execution", false));
    check_true(turbo_json_get_bool(harness_capabilities, "supports_cancel", false));
    check_int_eq(turbo_json_get_int(harness_capabilities, "max_concurrent_executions", 0), 1);
    check_int_eq(turbo_agent_harness_start_text(harness, "hello", &run_options, &execution),
                 SALTS_OK);
    check_not_null(execution);
    check_not_null(turbo_agent_harness_execution_id(execution));
    check_int_eq(turbo_agent_harness_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_COMPLETED);
    check_int_gt(atomic_load_explicit(&event_count, memory_order_relaxed), 0);
    check_int_eq(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    run_id = turbo_json_get_string(summary, "run_id");
    check_not_null(run_id);
    check_str_eq(turbo_agent_app_last_run_id(turbo_agent_harness_app(harness)), run_id);
    text = turbo_agent_app_result_text(state);
    check_not_null(text);
    check_str_eq(text, "ok");

    check_int_eq(turbo_agent_harness_start_text(harness, "next", NULL, &next_execution), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_wait(next_execution, UINT64_MAX), SALTS_OK);

    free(text);
    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(diagnostics);
    turbo_agent_harness_execution_release(execution);
    turbo_agent_harness_execution_release(next_execution);
    turbo_agent_harness_release(harness);
    turbo_threadpool_destroy(pool);
  }

  it("rejects a concurrent run and supports cooperative cancellation") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_transport_gate_t gate;
    turbo_agent_harness_t *harness;
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_agent_harness_execution_t *second = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    json_value_t *summary = NULL;
    json_value_t *state = NULL;

    atomic_init(&gate.entered, 0);
    atomic_init(&gate.open, 0);
    harness = harness_create(pool, harness_gated_transport, &gate);
    check_not_null(pool);
    check_not_null(harness);
    check_int_eq(turbo_agent_harness_start_text(harness, "first", NULL, &execution), SALTS_OK);
    while (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      turbo_thread_yield();
    }
    check_int_eq(turbo_agent_harness_start_text(harness, "second", NULL, &second), SALTS_EBUSY);
    check_null(second);
    check_int_eq(turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_USER), SALTS_OK);
    atomic_store_explicit(&gate.open, 1, memory_order_release);
    check_int_eq(turbo_agent_harness_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_CANCELLED);
    check_int_eq(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "cancelled");

    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_agent_harness_execution_release(execution);
    turbo_agent_harness_release(harness);
    turbo_threadpool_destroy(pool);
  }

  it("resumes and forks the default workflow with explicit command semantics") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness = harness_create_kind(pool, harness_review_transport, NULL,
                                                         TURBO_AGENT_SESSION_WORKFLOW_REVIEW);
    turbo_agent_harness_execution_t *start_execution = NULL;
    turbo_agent_harness_execution_t *resume_execution = NULL;
    turbo_agent_harness_execution_t *fork_execution = NULL;
    const char *interrupt_before_review[] = {"review"};
    turbo_graph_run_options_t graph_options = {.interrupt_before_nodes = interrupt_before_review,
                                               .interrupt_before_count = 1};
    turbo_agent_session_exec_options_t session_options = {
        .scope = TURBO_SESSION_SCOPE_THREAD, .input_kind = TURBO_SESSION_INPUT_COMMAND};
    turbo_agent_harness_run_options_t options;
    json_value_t *command = turbo_json_create_object();
    json_value_t *start_summary = NULL;
    json_value_t *start_state = NULL;
    json_value_t *resume_summary = NULL;
    json_value_t *resume_state = NULL;
    json_value_t *fork_summary = NULL;
    json_value_t *fork_state = NULL;
    const char *checkpoint_id;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    int operation_rc = SALTS_EIO;

    check_not_null(pool);
    check_not_null(harness);
    check_not_null(command);
    check_int_eq(
        turbo_runtime_json_object_set(command, "kind", turbo_json_create_string("approve_review")),
        TURBO_RUNTIME_JSON_OK);
    turbo_agent_harness_run_options_init(&options);
    options.graph_options = &graph_options;
    check_int_eq(turbo_agent_harness_start_text(harness, "review me", &options, &start_execution),
                 SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_wait(start_execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_get_status(start_execution, &status), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_result_code(start_execution, &operation_rc),
                 SALTS_OK);
    check_int_eq(operation_rc, SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_INTERRUPTED);
    check_int_eq(
        turbo_agent_harness_execution_take_result(start_execution, &start_summary, &start_state),
        SALTS_OK);
    check_str_eq(turbo_json_get_string(start_summary, "status"), "interrupted");
    checkpoint_id = turbo_json_get_string(start_summary, "checkpoint_id");
    check_not_null(checkpoint_id);

    options.graph_options = NULL;
    options.session_options = &session_options;
    check_int_eq(turbo_agent_harness_resume(harness, command, &options, &resume_execution),
                 SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_wait(resume_execution, UINT64_MAX), SALTS_OK);
    check_int_eq(
        turbo_agent_harness_execution_take_result(resume_execution, &resume_summary, &resume_state),
        SALTS_OK);
    check_str_eq(turbo_json_get_string(resume_summary, "status"), "completed");

    session_options.scope = TURBO_SESSION_SCOPE_CHECKPOINT;
    session_options.checkpoint_id = checkpoint_id;
    check_int_eq(turbo_agent_harness_fork(harness, command, &options, &fork_execution), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_wait(fork_execution, UINT64_MAX), SALTS_OK);
    check_int_eq(
        turbo_agent_harness_execution_take_result(fork_execution, &fork_summary, &fork_state),
        SALTS_OK);
    check_str_eq(turbo_json_get_string(fork_summary, "status"), "completed");

    turbo_runtime_json_destroy(fork_state);
    turbo_runtime_json_destroy(fork_summary);
    turbo_runtime_json_destroy(resume_state);
    turbo_runtime_json_destroy(resume_summary);
    turbo_runtime_json_destroy(start_state);
    turbo_runtime_json_destroy(start_summary);
    turbo_runtime_json_destroy(command);
    turbo_agent_harness_execution_release(fork_execution);
    turbo_agent_harness_execution_release(resume_execution);
    turbo_agent_harness_execution_release(start_execution);
    turbo_agent_harness_release(harness);
    turbo_threadpool_destroy(pool);
  }

  it("propagates an execution deadline to the controlled runtime") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    turbo_agent_harness_t *harness = harness_create(pool, harness_success_transport, NULL);
    turbo_agent_harness_run_options_t options;
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    json_value_t *summary = NULL;
    json_value_t *state = NULL;

    check_not_null(pool);
    check_not_null(harness);
    turbo_agent_harness_run_options_init(&options);
    options.deadline_mono_ms = turbo_monotonic_ms();
    check_int_eq(turbo_agent_harness_start_text(harness, "too late", &options, &execution),
                 SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_wait(execution, UINT64_MAX), SALTS_OK);
    check_int_eq(turbo_agent_harness_execution_get_status(execution, &status), SALTS_OK);
    check_int_eq(status, TURBO_AGENT_EXECUTION_TIMED_OUT);
    check_int_eq(turbo_agent_harness_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_str_eq(turbo_json_get_string(summary, "status"), "timed_out");

    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_agent_harness_execution_release(execution);
    turbo_agent_harness_release(harness);
    turbo_threadpool_destroy(pool);
  }
}
