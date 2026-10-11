#include "tinytest.h"
#include "turbo_agent_http_test_support.h"
#include "turbo_agent_session_execution.h"
#include <fmt.h>
#include <salts/clock.h>
#include <stdatomic.h>
#include <string.h>

enum {
  DEFER_DISABLED = 0,
  DEFER_PENDING = 1,
  DEFER_CLAIMED = 2,
  HTTP_CANCEL_DEADLINE_MS = 1500,
  HTTP_SERVER_IDLE_MS = 60000
};

typedef struct cancel_server_s {
  atomic_int mode;
  atomic_int ready;
  atomic_int requests;
  chttp_server_deferred deferred;
  int defer_status;
} cancel_server_t;

static int cancel_server_reply(void *user, const chttp_server_request_view *request,
                               chttp_server_response *response) {
  static const char body[] =
      "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"42\"},"
      "\"finish_reason\":\"stop\"}]}";
  cancel_server_t *gate = user;
  int expected = DEFER_PENDING;
  (void)request;
  atomic_fetch_add(&gate->requests, 1);
  if (atomic_compare_exchange_strong(&gate->mode, &expected, DEFER_CLAIMED)) {
    gate->defer_status = chttp_server_response_defer(response, &gate->deferred);
    atomic_store_explicit(&gate->ready, 1, memory_order_release);
    return gate->defer_status;
  }
  return chttp_server_reply(response, 200, "application/json", body, sizeof(body) - 1);
}

static void cancel_server_wait_ready(cancel_server_t *gate) {
  const uint64_t deadline = cmeta_monotonic_ms() + AGENT_HTTP_TEST_TIMEOUT_MS;
  while (!atomic_load_explicit(&gate->ready, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  check_true(atomic_load_explicit(&gate->ready, memory_order_acquire));
  check_equal(gate->defer_status, SALTS_OK);
}

static void cancel_server_release(cancel_server_t *gate) {
  const int previous = atomic_exchange(&gate->mode, DEFER_DISABLED);
  /* Once the handler claims the slot, it publishes the handle before this
   * thread can consume it. Shutdown also closes this single admission gate. */
  if (previous == DEFER_CLAIMED) cancel_server_wait_ready(gate);
  if (atomic_load_explicit(&gate->ready, memory_order_acquire) && gate->deferred.impl) {
    const int status = chttp_server_deferred_cancel(&gate->deferred);
    check_true(status == SALTS_OK || status == SALTS_ENOENT);
    gate->deferred = (chttp_server_deferred){0};
  }
}

spec("model HTTP cancellation") {
  static chttp_server server;
  static int server_started;
  static cancel_server_t gate;
  static tstr base_url;
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static cmeta_threadpool_t *workers[2];
  static turbo_agent_execution_t *execution;
  static json_value_t *input, *summary, *state;

  before_each() {
    chttp_server_config server_config = agent_http_test_server_config();
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_retry_policy_t retry;
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    uint16_t port = 0;
    atomic_init(&gate.mode, DEFER_PENDING);
    atomic_init(&gate.ready, 0);
    atomic_init(&gate.requests, 0);
    gate.deferred = (chttp_server_deferred){0};
    gate.defer_status = SALTS_EINVAL;
    /* A test must finish while the server has not replied or timed out. */
    server_config.network.read_timeout_ms = HTTP_SERVER_IDLE_MS;
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/v1/chat/completions",
                                   cancel_server_reply, &gate),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    server_started = 1;
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    base_url = tstr_format("http://127.0.0.1:{}/v1", (unsigned)port);
    check_not_null(base_url);
    session_config.agent_config.model = "test-model";
    session_config.agent_config.api_key = "test-only-key";
    session_config.agent_config.base_url = base_url;
    session_config.agent_config.api_mode = TURBO_AGENT_API_CHAT_COMPLETIONS;
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&session_config);
    check_not_null(session);
    turbo_agent_retry_policy_init(&retry);
    retry.max_attempts = 3;
    check_equal(turbo_agent_retry_configure(turbo_agent_session_agent(session), &retry), SALTS_OK);
    graph = turbo_agent_session_create_preset_graph(session, TURBO_AGENT_SESSION_WORKFLOW_LOOP);
    check_not_null(graph);
    input = turbo_agent_session_create_input_state_json_value("19 plus 23?");
    check_not_null(input);
    for (size_t i = 0; i < 2; ++i) {
      workers[i] = cmeta_threadpool_create_with_config(&pool_config);
      check_not_null(workers[i]);
    }
  }

  after_each() {
    /* Release server-side admission even if an assertion fails before cancel. */
    cancel_server_release(&gate);
    if (execution) {
      turbo_agent_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      turbo_agent_execution_wait(execution, UINT64_MAX);
      turbo_agent_execution_release(execution);
      execution = NULL;
    }
    for (size_t i = 0; i < 2; ++i) {
      if (workers[i]) cmeta_threadpool_wait(workers[i]);
      cmeta_threadpool_destroy(workers[i]);
      workers[i] = NULL;
    }
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(input);
    json_free(summary);
    json_free(state);
    input = summary = state = NULL;
    tstr_free(base_url);
    base_url = NULL;
    if (server_started) {
      check_equal(chttp_server_stop(&server, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
      server_started = 0;
    }
    if (server.impl) check_equal(chttp_server_destroy(&server), SALTS_OK);
  }

  const struct {
    const char *name;
    turbo_cancel_reason_t reason;
    turbo_agent_execution_status_t status;
    int transport_status;
    const char *summary_status;
  } cases[] = {{"user cancellation", TURBO_CANCEL_USER, TURBO_AGENT_EXECUTION_CANCELLED,
                SALTS_ECANCELED, "cancelled"},
               {"shutdown cancellation", TURBO_CANCEL_SHUTDOWN, TURBO_AGENT_EXECUTION_CANCELLED,
                SALTS_ECANCELED, "cancelled"},
               {"in-flight deadline", TURBO_CANCEL_DEADLINE, TURBO_AGENT_EXECUTION_TIMED_OUT,
                SALTS_ETIMEDOUT, "timed_out"}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    it("drains %s before a server reply and resumes on another worker", cases[i].name) {
      turbo_agent_session_submit_options_t options;
      turbo_agent_session_exec_options_t resume = {0};
      const char *interrupt_nodes[] = {"model"};
      turbo_graph_run_options_t graph_options = {.interrupt_before_nodes = interrupt_nodes,
                                                 .interrupt_before_count = 1,
                                                 .skip_initial_interrupt = 1};
      turbo_agent_execution_status_t status;
      const json_value_t *attempts, *attempt;
      int run_rc = -1;
      turbo_agent_session_submit_options_init(&options);
      /* Cancellation at the returning model boundary must beat a simultaneous
       * ordinary workflow pause before that same node. */
      if (cases[i].reason == TURBO_CANCEL_USER) options.graph_options = &graph_options;
      if (cases[i].reason == TURBO_CANCEL_DEADLINE)
        options.deadline_mono_ms = cmeta_monotonic_ms() + HTTP_CANCEL_DEADLINE_MS;
      check_equal(
          turbo_agent_session_submit(workers[0], session, graph, input, &options, &execution),
          SALTS_OK);
      cancel_server_wait_ready(&gate);
      if (cases[i].reason != TURBO_CANCEL_DEADLINE)
        check_equal(turbo_agent_execution_cancel(execution, cases[i].reason), SALTS_OK);
      /* No server reply/cancel yet: completion must come from the client. */
      check_equal(turbo_agent_execution_wait(execution, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
      cmeta_threadpool_wait(workers[0]);
      check_equal(turbo_agent_execution_get_status(execution, &status), SALTS_OK);
      check_equal(status, cases[i].status);
      check_equal(turbo_agent_execution_result_code(execution, &run_rc), SALTS_OK);
      check_equal(run_rc, 0);
      check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
      check_equal(json_get_string(summary, "status"), cases[i].summary_status);
      check_not_null(json_get_string(summary, "checkpoint_id"));
      check_equal(turbo_agent_state_event_count(state), 0);
      check_null(turbo_agent_state_final_answer_text(state));
      check_null(turbo_agent_state_last_output_text(state));
      attempts = json_object_get(state, "provider_attempts");
      check_equal(json_array_size(attempts), 1);
      attempt = json_array_get(attempts, 0);
      check_equal(json_get_int(attempt, "transport_status", 0), cases[i].transport_status);
      check_equal(json_get_int(attempt, "http_status", -1), 0);
      check_false(json_get_bool(attempt, "retryable", true));
      check_equal(atomic_load(&gate.requests), 1);
      turbo_agent_execution_release(execution);
      execution = NULL;
      cancel_server_release(&gate);
      json_free(summary);
      json_free(state);
      summary = state = NULL;

      /* Only explicit resume admits the second POST; the checkpoint must point
       * to the uncommitted model node, not skip ahead to its end/tools node. */
      turbo_agent_session_submit_options_init(&options);
      options.operation = TURBO_AGENT_SESSION_RESUME;
      options.session_options = &resume;
      check_equal(
          turbo_agent_session_submit(workers[1], session, graph, NULL, &options, &execution),
          SALTS_OK);
      check_equal(turbo_agent_execution_wait(execution, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
      cmeta_threadpool_wait(workers[1]);
      check_equal(turbo_agent_execution_get_status(execution, &status), SALTS_OK);
      check_equal(status, TURBO_AGENT_EXECUTION_COMPLETED);
      check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
      check_equal(turbo_agent_state_final_answer_text(state), "42");
      check_equal(atomic_load(&gate.requests), 2);
      check_equal(turbo_agent_state_event_count(state), 1);
      attempts = json_object_get(state, "provider_attempts");
      check_equal(json_array_size(attempts), 2);
      attempt = json_array_get(attempts, 1);
      check_equal(json_get_int(attempt, "transport_status", -1), SALTS_OK);
      check_equal(json_get_int(attempt, "http_status", -1), 200);
    }
  }
}
