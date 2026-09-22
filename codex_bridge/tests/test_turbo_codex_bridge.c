#include "tinytest.h"
#include "turbo_codex_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json_parser.h>

typedef struct test_codex_transport_s {
  char *messages[32];
  size_t head;
  size_t count;
  unsigned int thread_counter;
  unsigned int turn_counter;
  int initialized;
  int closed;
  int approval_response_seen;
  int approval_declined;
} test_codex_transport_t;

typedef struct test_codex_events_s {
  int deltas;
  int completed;
} test_codex_events_t;

static char *test_codex_strdup(const char *text) {
  size_t length = strlen(text);
  char *copy = (char *)malloc(length + 1);
  if (copy) memcpy(copy, text, length + 1);
  return copy;
}

static int test_codex_enqueue(test_codex_transport_t *transport, const char *message) {
  size_t slot;
  if (!transport || !message || transport->count >= 32) return SALTS_EBUSY;
  slot = (transport->head + transport->count) % 32;
  transport->messages[slot] = test_codex_strdup(message);
  if (!transport->messages[slot]) return SALTS_ENOMEM;
  transport->count++;
  return SALTS_OK;
}

static int test_codex_response(test_codex_transport_t *transport, const char *id,
                               const char *result_json) {
  char message[2048];
  int length = snprintf(message, sizeof(message), "{\"id\":\"%s\",\"result\":%s}", id,
                        result_json);
  if (length < 0 || (size_t)length >= sizeof(message)) return SALTS_EMSGSIZE;
  return test_codex_enqueue(transport, message);
}

static int test_codex_write(const uint8_t *frame, size_t frame_size, void *user_data) {
  test_codex_transport_t *transport = (test_codex_transport_t *)user_data;
  json_value_t *message = NULL;
  const char *method;
  const char *id;
  const json_value_t *result;
  char response[2048];
  int length;
  int rc = SALTS_OK;
  if (!transport || !frame || !frame_size || frame[frame_size - 1] != '\n' ||
      turbo_parse_json(frame, frame_size - 1, &message) != 0 || !message) {
    turbo_free_json(&message);
    return SALTS_EPROTO;
  }
  method = json_get_string(message, "method");
  id = json_get_string(message, "id");
  if (method && !strcmp(method, "initialize")) {
    rc = test_codex_response(transport, id,
                             "{\"serverInfo\":{\"name\":\"codex-app-server\"}}");
  } else if (method && !strcmp(method, "initialized")) {
    transport->initialized = 1;
  } else if (method && (!strcmp(method, "thread/start") || !strcmp(method, "thread/resume"))) {
    const json_value_t *params = json_object_get(message, "params");
    const char *requested = params ? json_get_string(params, "threadId") : NULL;
    char thread_id[64];
    if (requested) {
      snprintf(thread_id, sizeof(thread_id), "%s", requested);
    } else {
      snprintf(thread_id, sizeof(thread_id), "thread-%u", ++transport->thread_counter);
    }
    length = snprintf(response, sizeof(response),
                      "{\"thread\":{\"id\":\"%s\",\"status\":{\"type\":\"idle\"}}}",
                      thread_id);
    rc = length < 0 || (size_t)length >= sizeof(response)
             ? SALTS_EMSGSIZE
             : test_codex_response(transport, id, response);
  } else if (method && !strcmp(method, "turn/start")) {
    const json_value_t *params = json_object_get(message, "params");
    const char *thread_id = params ? json_get_string(params, "threadId") : NULL;
    char turn_id[64];
    snprintf(turn_id, sizeof(turn_id), "turn-%u", ++transport->turn_counter);
    length = snprintf(response, sizeof(response),
                      "{\"method\":\"item/agentMessage/delta\",\"params\":{"
                      "\"threadId\":\"%s\",\"turnId\":\"%s\",\"itemId\":\"item-1\","
                      "\"delta\":\"Codex completed the task\"}}",
                      thread_id, turn_id);
    rc = length < 0 || (size_t)length >= sizeof(response) ? SALTS_EMSGSIZE
                                                          : test_codex_enqueue(transport, response);
    if (rc == SALTS_OK) {
      length = snprintf(response, sizeof(response),
                        "{\"method\":\"item/commandExecution/requestApproval\","
                        "\"id\":\"approval-1\",\"params\":{\"threadId\":\"%s\","
                        "\"turnId\":\"%s\",\"itemId\":\"command-1\",\"startedAtMs\":1}}",
                        thread_id, turn_id);
      rc = length < 0 || (size_t)length >= sizeof(response) ? SALTS_EMSGSIZE
                                                            : test_codex_enqueue(transport, response);
    }
    if (rc == SALTS_OK) {
      length = snprintf(response, sizeof(response),
                        "{\"turn\":{\"id\":\"%s\",\"items\":[],\"status\":\"inProgress\"}}",
                        turn_id);
      rc = length < 0 || (size_t)length >= sizeof(response)
               ? SALTS_EMSGSIZE
               : test_codex_response(transport, id, response);
    }
    if (rc == SALTS_OK) {
      length = snprintf(response, sizeof(response),
                        "{\"method\":\"turn/completed\",\"params\":{\"threadId\":\"%s\","
                        "\"turn\":{\"id\":\"%s\",\"items\":[],\"status\":\"completed\"}}}",
                        thread_id, turn_id);
      rc = length < 0 || (size_t)length >= sizeof(response) ? SALTS_EMSGSIZE
                                                            : test_codex_enqueue(transport, response);
    }
  } else if (!method && id && !strcmp(id, "approval-1")) {
    result = json_object_get(message, "result");
    transport->approval_response_seen = 1;
    transport->approval_declined =
        result && json_get_string(result, "decision") &&
        !strcmp(json_get_string(result, "decision"), "decline");
  } else {
    rc = SALTS_EPROTO;
  }
  turbo_free_json(&message);
  return rc;
}

static int test_codex_read(uint64_t timeout_ms, char **out_line, void *user_data) {
  test_codex_transport_t *transport = (test_codex_transport_t *)user_data;
  char *message;
  (void)timeout_ms;
  if (!transport || !out_line) return SALTS_EINVAL;
  *out_line = NULL;
  if (!transport->count) return SALTS_ETIMEDOUT;
  message = transport->messages[transport->head];
  transport->messages[transport->head] = NULL;
  transport->head = (transport->head + 1) % 32;
  transport->count--;
  *out_line = message;
  return SALTS_OK;
}

static void test_codex_close(void *user_data) {
  test_codex_transport_t *transport = (test_codex_transport_t *)user_data;
  if (transport) transport->closed = 1;
}

static void test_codex_transport_cleanup(test_codex_transport_t *transport) {
  while (transport && transport->count) {
    free(transport->messages[transport->head]);
    transport->messages[transport->head] = NULL;
    transport->head = (transport->head + 1) % 32;
    transport->count--;
  }
}

static turbo_codex_client_t *test_codex_client(test_codex_transport_t *transport,
                                               turbo_codex_server_request_fn request_handler,
                                               void *request_user_data,
                                               turbo_codex_event_fn event_handler,
                                               void *event_user_data) {
  turbo_codex_client_config_t config;
  turbo_codex_client_config_init(&config);
  config.transport.write = test_codex_write;
  config.transport.read = test_codex_read;
  config.transport.close = test_codex_close;
  config.transport.user_data = transport;
  config.server_request = request_handler;
  config.server_request_user_data = request_user_data;
  config.event = event_handler;
  config.event_user_data = event_user_data;
  return turbo_codex_client_create(&config);
}

static int test_codex_approve(const char *method, const json_value_t *params,
                              json_value_t **out_result, void *user_data) {
  int *calls = (int *)user_data;
  (void)params;
  if (!method || strcmp(method, "item/commandExecution/requestApproval") || !out_result) {
    return SALTS_EINVAL;
  }
  (*calls)++;
  *out_result = json_create_object();
  if (!*out_result) return SALTS_ENOMEM;
  json_object_set_string(*out_result, "decision", "accept");
  return SALTS_OK;
}

static void test_codex_event(const char *method, const json_value_t *params, void *user_data) {
  test_codex_events_t *events = (test_codex_events_t *)user_data;
  (void)params;
  if (!strcmp(method, "item/agentMessage/delta")) events->deltas++;
  if (!strcmp(method, "turn/completed")) events->completed++;
}

spec("Codex App Server bridge") {
  it("validates transport configuration and performs the required handshake") {
    turbo_codex_client_config_t config;
    test_codex_transport_t transport = {0};
    test_codex_transport_t rejected_transport = {0};
    turbo_codex_client_t *client;
    json_value_t *server_info = NULL;
    turbo_codex_client_config_init(&config);
    check_null(turbo_codex_client_create(&config));
    config.transport.write = test_codex_write;
    config.transport.read = test_codex_read;
    config.transport.close = test_codex_close;
    config.transport.user_data = &rejected_transport;
    config.initialize_timeout_ms = 0;
    check_null(turbo_codex_client_create(&config));
    turbo_codex_transport_release(&config.transport);
    check_true(rejected_transport.closed);
    client = test_codex_client(&transport, NULL, NULL, NULL, NULL);
    check_not_null(client);
    check_int_eq(turbo_codex_client_initialize(client, &server_info), SALTS_OK);
    check_true(transport.initialized);
    check_str_eq(json_get_string(json_object_get(server_info, "serverInfo"), "name"),
                 "codex-app-server");
    check_int_eq(turbo_codex_client_initialize(client, NULL), SALTS_EALREADY);
    turbo_free_json(&server_info);
    turbo_codex_client_destroy(client);
    check_true(transport.closed);
    test_codex_transport_cleanup(&transport);
  }

  it("runs one turn while dispatching events and an approval request") {
    test_codex_transport_t transport = {0};
    test_codex_events_t events = {0};
    int approval_calls = 0;
    turbo_codex_client_t *client =
        test_codex_client(&transport, test_codex_approve, &approval_calls, test_codex_event, &events);
    turbo_codex_run_options_t options;
    char *thread_id = NULL;
    char *turn_id = NULL;
    char *text = NULL;
    json_value_t *turn = NULL;
    check_not_null(client);
    turbo_codex_run_options_init(&options);
    options.cwd = "C:/workspace";
    check_int_eq(turbo_codex_client_run_text(client, "review the repository", &options,
                                              &thread_id, &turn_id, &text, &turn),
                 SALTS_OK);
    check_str_eq(thread_id, "thread-1");
    check_str_eq(turn_id, "turn-1");
    check_str_eq(text, "Codex completed the task");
    check_str_eq(json_get_string(turn, "status"), "completed");
    check_int_eq(approval_calls, 1);
    check_true(transport.approval_response_seen);
    check_false(transport.approval_declined);
    check_int_eq(events.deltas, 1);
    check_int_eq(events.completed, 1);
    free(thread_id);
    free(turn_id);
    free(text);
    turbo_free_json(&turn);
    turbo_codex_client_destroy(client);
    test_codex_transport_cleanup(&transport);
  }

  it("declines approvals when the host did not install an approval broker") {
    test_codex_transport_t transport = {0};
    turbo_codex_client_t *client = test_codex_client(&transport, NULL, NULL, NULL, NULL);
    char *text = NULL;
    check_not_null(client);
    check_int_eq(turbo_codex_client_run_text(client, "read only", NULL, NULL, NULL, &text, NULL),
                 SALTS_OK);
    check_true(transport.approval_response_seen);
    check_true(transport.approval_declined);
    free(text);
    turbo_codex_client_destroy(client);
    test_codex_transport_cleanup(&transport);
  }

  it("registers Codex as an exclusive delegate capability") {
    test_codex_transport_t transport = {0};
    turbo_codex_client_t *client = test_codex_client(&transport, NULL, NULL, NULL, NULL);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_codex_delegate_tool_config_t config;
    turbo_tool_execution_policy_t policy;
    const char *const *capabilities = NULL;
    size_t capability_count = 0;
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    check_not_null(client);
    check_not_null(registry);
    turbo_codex_delegate_tool_config_init(&config);
    config.client = client;
    config.cwd = "C:/workspace";
    check_int_eq(turbo_codex_delegate_tool_register(registry, &config), TURBO_TOOL_OK);
    check_int_eq(turbo_tool_registry_get_execution_policy(registry, "codex.delegate", &policy),
                 TURBO_TOOL_OK);
    check_int_eq(policy.mode, TURBO_TOOL_EXECUTION_EXCLUSIVE);
    check_int_eq(turbo_tool_registry_get_required_capabilities(
                     registry, "codex.delegate", &capabilities, &capability_count),
                 TURBO_TOOL_OK);
    check_size_eq(capability_count, 1);
    check_str_eq(capabilities[0], "delegate");
    check_int_eq(turbo_parse_json((const uint8_t *)"{\"task\":\"inspect build failure\"}",
                                  strlen("{\"task\":\"inspect build failure\"}"), &arguments),
                 0);
    check_int_eq(turbo_tool_registry_execute_json_value(registry, "codex.delegate", arguments,
                                                        &result),
                 TURBO_TOOL_OK);
    check_str_eq(json_get_string(result, "text"), "Codex completed the task");
    turbo_free_json(&result);
    turbo_free_json(&arguments);
    turbo_tool_registry_destroy(registry);
    turbo_codex_client_destroy(client);
    test_codex_transport_cleanup(&transport);
  }
}
