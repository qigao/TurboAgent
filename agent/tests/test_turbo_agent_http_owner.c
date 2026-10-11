#include "tinytest.h"
#include "turbo_agent_http_test_support.h"
#include "turbo_agent_session.h"
#include "turbo_agent_session_execution.h"
#include <fmt.h>
#include <stdatomic.h>
#include <string.h>

typedef struct owner_test_server_s {
  atomic_int status;
  atomic_int requests;
} owner_test_server_t;

static int owner_test_reply(void *user, const chttp_server_request_view *request,
                            chttp_server_response *response) {
  owner_test_server_t *server = user;
  const int status = atomic_load(&server->status);
  const char *body =
      status == 200 ? "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"42\"},"
                      "\"finish_reason\":\"stop\"}]}"
                    : "{\"error\":{\"type\":\"invalid_api_key\",\"message\":\"test rejection\"}}";
  const char *accept = chttp_server_request_header(request, "Accept");
  const int stream = status == 200 && accept && !strcmp(accept, "text/event-stream");
  if (stream) body =
      "data: {\"id\":\"stream-owner\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"4\"}}]}\n\n"
      "data: {\"id\":\"stream-owner\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"2\"},\"finish_reason\":\"stop\"}]}\n\n"
      "data: [DONE]\n\n";
  atomic_fetch_add(&server->requests, 1);
  return chttp_server_reply(response, (unsigned)status,
                            stream ? "text/event-stream" : "application/json", body, strlen(body));
}

static void owner_test_attempt(const json_value_t *state, int http, const char *outcome) {
  const json_value_t *attempts = json_object_get(state, "provider_attempts");
  const json_value_t *record;
  check_equal(json_type(attempts), JSON_ARRAY);
  check_equal(json_array_size(attempts), 1);
  record = json_array_get(attempts, 0);
  check_equal(json_get_int(record, "http_status", -1), http);
  check_equal(json_get_int(record, "transport_status", -1), 0);
  check_equal(json_get_int(record, "attempt", -1), 1);
  check_equal(json_get_string(record, "outcome"), outcome);
  check_false(json_get_bool(record, "retryable", true));
  check_null(json_object_get(record, "body"));
  check_greater_equal(json_get_double(record, "finished_mono_ms", -1),
                      json_get_double(record, "started_mono_ms", 0));
}

spec("model HTTP execution owner") {
  static chttp_server server;
  static int server_started;
  static owner_test_server_t replies;
  static tstr base_url;
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static cmeta_threadpool_t *workers[2];
  static turbo_agent_execution_t *execution;
  static json_value_t *input, *summary, *state;

  before_each() {
    chttp_server_config server_config = agent_http_test_server_config();
    turbo_agent_session_config_t config = {0};
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    uint16_t port = 0;
    atomic_init(&replies.status, 200);
    atomic_init(&replies.requests, 0);
    check_equal(chttp_server_init(&server, &server_config), SALTS_OK);
    check_equal(chttp_server_route(&server, CHTTP_METHOD_POST, "/v1/chat/completions",
                                   owner_test_reply, &replies),
                SALTS_OK);
    check_equal(chttp_server_start(&server), SALTS_OK);
    server_started = 1;
    check_equal(chttp_server_port(&server, &port), SALTS_OK);
    base_url = tstr_format("http://127.0.0.1:{}/v1", (unsigned)port);
    check_not_null(base_url);
    config.agent_config.model = "test-model";
    config.agent_config.api_key = "test-only-key";
    config.agent_config.base_url = base_url;
    config.agent_config.api_mode = TURBO_AGENT_API_CHAT_COMPLETIONS;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
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

  it("uses one Session sequentially on two workers and then its creator thread") {
    for (size_t i = 0; i < 2; ++i) {
      int rc = -1;
      check_equal(turbo_agent_session_submit(workers[i], session, graph, input, NULL, &execution),
                  SALTS_OK);
      check_equal(turbo_agent_execution_wait(execution, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
      check_equal(turbo_agent_execution_result_code(execution, &rc), SALTS_OK);
      check_equal(rc, 0);
      check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
      cmeta_threadpool_wait(workers[i]);
      check_equal(turbo_agent_state_final_answer_text(state), "42");
      owner_test_attempt(state, 200, "succeeded");
      turbo_agent_execution_release(execution);
      execution = NULL;
      json_free(summary);
      json_free(state);
      summary = state = NULL;
    }
    check_equal(turbo_agent_session_start_text(session, "19 plus 23?", NULL, &summary, &state), 0);
    check_equal(turbo_agent_state_final_answer_text(state), "42");
    check_equal(atomic_load(&replies.requests), 3);
  }

  it("drains a rejected request before a successful run on a different worker") {
    int rc = 0;
    atomic_store(&replies.status, 401);
    check_equal(turbo_agent_session_submit(workers[0], session, graph, input, NULL, &execution),
                SALTS_OK);
    check_equal(turbo_agent_execution_wait(execution, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(turbo_agent_execution_result_code(execution, &rc), SALTS_OK);
    check_not_equal(rc, 0);
    check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    cmeta_threadpool_wait(workers[0]);
    check_equal(turbo_agent_state_model_error_phase(state), "transport");
    check_contains(turbo_agent_state_model_error_detail(state), "http_status=401");
    owner_test_attempt(state, 401, "failed");
    check_equal(atomic_load(&replies.requests), 1);
    turbo_agent_execution_release(execution);
    execution = NULL;
    json_free(summary);
    json_free(state);
    summary = state = NULL;

    atomic_store(&replies.status, 200);
    check_equal(turbo_agent_session_submit(workers[1], session, graph, input, NULL, &execution),
                SALTS_OK);
    check_equal(turbo_agent_execution_wait(execution, AGENT_HTTP_TEST_TIMEOUT_MS), SALTS_OK);
    check_equal(turbo_agent_execution_result_code(execution, &rc), SALTS_OK);
    check_equal(rc, 0);
    check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    cmeta_threadpool_wait(workers[1]);
    check_equal(turbo_agent_state_final_answer_text(state), "42");
    check_equal(atomic_load(&replies.requests), 2);
  }

  it("keeps default HTTP single-attempt even when a retry policy allows more attempts") {
    turbo_agent_retry_policy_t retry;
    turbo_agent_retry_policy_init(&retry);
    retry.max_attempts = 3;
    check_equal(turbo_agent_retry_configure(turbo_agent_session_agent(session), &retry), SALTS_OK);
    atomic_store(&replies.status, 429);
    check_not_equal(turbo_agent_session_start_text(session, "hello", NULL, &summary, &state), 0);
    owner_test_attempt(state, 429, "failed");
    check_equal(atomic_load(&replies.requests), 1);
  }

  it("owns the HTTP body through SSE aggregation after client teardown") {
    turbo_agent_session_config_t config = {0};
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    config.agent_config.model = "test-model";
    config.agent_config.api_key = "test-only-key";
    config.agent_config.base_url = base_url;
    config.agent_config.api_mode = TURBO_AGENT_API_CHAT_COMPLETIONS;
    config.agent_config.stream_response = 1;
    config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_session_start_text(session, "19 plus 23?", NULL, &summary, &state), 0);
    check_equal(turbo_agent_state_final_answer_text(state), "42");
    owner_test_attempt(state, 200, "succeeded");
    check_equal(atomic_load(&replies.requests), 1);
  }
}
