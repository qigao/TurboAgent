#include "tinytest.h"

#include "turbo_agent_harness_server.h"
#include "turbo_agent_harness_transport.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>
#include <salts/thread.h>

typedef struct harness_server_factory_s {
  turbo_threadpool_t *executor;
  turbo_agent_transport_fn transport;
  void *transport_user_data;
  turbo_agent_session_workflow_kind_t workflow_kind;
} harness_server_factory_t;

typedef struct harness_server_gate_s {
  atomic_int entered;
  atomic_int open;
} harness_server_gate_t;

typedef struct harness_server_close_task_s {
  turbo_agent_harness_connection_t *connection;
  atomic_int entered;
  atomic_int done;
} harness_server_close_task_t;

typedef struct harness_server_event_wait_task_s {
  turbo_agent_harness_connection_t *connection;
  atomic_int entered;
  int rc;
  json_value_t *event;
  uint64_t sequence;
} harness_server_event_wait_task_t;

enum { HARNESS_SERVER_CAPTURE_MAX_FRAMES = 16 };

typedef struct harness_server_frame_capture_s {
  char *frames[HARNESS_SERVER_CAPTURE_MAX_FRAMES];
  size_t frame_count;
  int fail_writes;
} harness_server_frame_capture_t;

static void harness_server_close_connection_task(void *user_data) {
  harness_server_close_task_t *task = (harness_server_close_task_t *)user_data;
  atomic_store_explicit(&task->entered, 1, memory_order_release);
  turbo_agent_harness_connection_close(task->connection);
  atomic_store_explicit(&task->done, 1, memory_order_release);
}

static void harness_server_wait_event_task(void *user_data) {
  harness_server_event_wait_task_t *task = (harness_server_event_wait_task_t *)user_data;
  atomic_store_explicit(&task->entered, 1, memory_order_release);
  task->rc = turbo_agent_harness_connection_wait_event_json_value(task->connection, UINT64_MAX,
                                                                  &task->event, &task->sequence);
}

static int harness_server_capture_frame(const uint8_t *frame, size_t frame_size, void *user_data) {
  harness_server_frame_capture_t *capture = (harness_server_frame_capture_t *)user_data;
  char *copy;
  if (!frame || frame_size < 2 || !capture) return SALTS_EINVAL;
  if (capture->fail_writes) return SALTS_EIO;
  if (frame[frame_size - 1] != '\n' || memchr(frame, '\r', frame_size - 1) ||
      memchr(frame, '\n', frame_size - 1)) {
    return SALTS_EPROTO;
  }
  if (capture->frame_count >= HARNESS_SERVER_CAPTURE_MAX_FRAMES) return TURBO_ENOBUFS;
  copy = (char *)malloc(frame_size);
  if (!copy) return SALTS_ENOMEM;
  memcpy(copy, frame, frame_size - 1);
  copy[frame_size - 1] = '\0';
  capture->frames[capture->frame_count++] = copy;
  return SALTS_OK;
}

static void harness_server_capture_destroy(harness_server_frame_capture_t *capture) {
  size_t index;
  if (!capture) return;
  for (index = 0; index < capture->frame_count; ++index)
    free(capture->frames[index]);
  memset(capture, 0, sizeof(*capture));
}

static json_value_t *harness_server_parse_frame(const harness_server_frame_capture_t *capture,
                                                size_t index) {
  json_value_t *value = NULL;
  check_true(capture && index < capture->frame_count);
  check_int_eq(turbo_parse_json((const uint8_t *)capture->frames[index],
                                strlen(capture->frames[index]), &value),
               SALTS_OK);
  return value;
}

static int harness_server_copy_response(const char *response, char **out_response_json) {
  size_t size;
  if (!response || !out_response_json) return SALTS_EINVAL;
  size = strlen(response) + 1;
  *out_response_json = (char *)malloc(size);
  if (!*out_response_json) return SALTS_ENOMEM;
  memcpy(*out_response_json, response, size);
  return SALTS_OK;
}

static int harness_server_success_transport(const char *request_json, char **out_response_json,
                                            void *user_data) {
  static const char response[] = "{\"id\":\"resp_server\",\"output\":[{\"type\":\"message\","
                                 "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                                 "\"text\":\"ok\"}]}]}";
  (void)request_json;
  (void)user_data;
  return harness_server_copy_response(response, out_response_json);
}

static int harness_server_review_transport(const char *request_json, char **out_response_json,
                                           void *user_data) {
  static const char planner_response[] =
      "{\"id\":\"resp_plan\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"{\\\"steps\\\":[\\\"say ok\\\"]}\"}]}]}";
  static const char executor_response[] =
      "{\"id\":\"resp_exec\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"ok\"}]}]}";
  (void)user_data;
  return harness_server_copy_response(request_json && strstr(request_json, "Execute plan step")
                                          ? executor_response
                                          : planner_response,
                                      out_response_json);
}

static int harness_server_gated_transport(const char *request_json, char **out_response_json,
                                          void *user_data) {
  harness_server_gate_t *gate = (harness_server_gate_t *)user_data;
  (void)request_json;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire)) {
    salts_thread_yield();
  }
  return harness_server_success_transport(NULL, out_response_json, NULL);
}

static int harness_server_thread_factory(const char *thread_id, turbo_agent_harness_t **out_harness,
                                         void *user_data) {
  harness_server_factory_t *factory = (harness_server_factory_t *)user_data;
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_harness_config_t harness_config;
  if (!thread_id || !out_harness || !factory) return SALTS_EINVAL;
  *out_harness = NULL;
  session_config.runtime_store = turbo_agent_runtime_store_memory_create();
  session_config.thread_id = thread_id;
  session_config.agent_config.model = "gpt-5.4";
  session_config.agent_config.transport_fn = factory->transport;
  session_config.agent_config.transport_user_data = factory->transport_user_data;
  session_config.workflow_kind = factory->workflow_kind;
  app_config.session_config = &session_config;
  turbo_agent_harness_config_init(&harness_config);
  harness_config.app_config = &app_config;
  harness_config.executor = factory->executor;
  *out_harness = turbo_agent_harness_create(&harness_config);
  return *out_harness ? SALTS_OK : SALTS_EIO;
}

static turbo_agent_harness_server_t *harness_server_create(harness_server_factory_t *factory) {
  turbo_agent_harness_server_config_t config;
  turbo_agent_harness_server_config_init(&config);
  config.thread_factory = harness_server_thread_factory;
  config.thread_factory_user_data = factory;
  config.max_threads = 4;
  config.max_event_count = 128;
  config.max_event_bytes = 1024U * 1024U;
  config.max_single_event_bytes = 64U * 1024U;
  config.max_replay_events = 64;
  return turbo_agent_harness_server_create(&config);
}

static json_value_t *harness_server_rpc(turbo_agent_harness_connection_t *connection, int id,
                                        const char *method, json_value_t *params) {
  json_value_t *request = turbo_json_create_object();
  json_value_t *response = NULL;
  check_not_null(request);
  turbo_json_object_set_number(request, "id", (double)id);
  turbo_json_object_set_string(request, "method", method);
  if (params) turbo_json_object_add(request, "params", params);
  check_int_eq(turbo_agent_harness_connection_dispatch_json_value(connection, request, &response),
               SALTS_OK);
  turbo_runtime_json_destroy(request);
  return response;
}

static void harness_server_ready(turbo_agent_harness_connection_t *connection) {
  json_value_t *client_info = turbo_json_create_object();
  json_value_t *params = turbo_json_create_object();
  json_value_t *response;
  json_value_t *notification = turbo_json_create_object();
  json_value_t *notification_params = turbo_json_create_object();
  json_value_t *notification_response = NULL;
  turbo_json_object_set_string(client_info, "name", "tinytest");
  turbo_json_object_set_string(client_info, "title", "TinyTest");
  turbo_json_object_set_string(client_info, "version", "1.0");
  turbo_json_object_add(params, "clientInfo", client_info);
  response = harness_server_rpc(connection, 1, "initialize", params);
  check_not_null(response);
  check_not_null(turbo_json_object_get(response, "result"));
  turbo_runtime_json_destroy(response);
  turbo_json_object_set_string(notification, "method", "initialized");
  turbo_json_object_add(notification, "params", notification_params);
  check_int_eq(turbo_agent_harness_connection_dispatch_json_value(connection, notification,
                                                                  &notification_response),
               SALTS_OK);
  check_null(notification_response);
  turbo_runtime_json_destroy(notification);
}

static json_value_t *harness_server_text_input(const char *text) {
  json_value_t *input = turbo_json_create_array();
  json_value_t *item = turbo_json_create_object();
  turbo_json_object_set_string(item, "type", "text");
  turbo_json_object_set_string(item, "text", text);
  turbo_json_array_add(input, item);
  return input;
}

static int harness_server_events_have_method(const json_value_t *events, const char *method) {
  size_t index;
  if (!events || turbo_json_type(events) != TURBO_JSON_ARRAY) return 0;
  for (index = 0; index < turbo_json_array_size(events); ++index) {
    const json_value_t *event = turbo_json_array_get(events, index);
    const char *candidate = turbo_json_get_string(event, "method");
    if (candidate && strcmp(candidate, method) == 0) return 1;
  }
  return 0;
}

static int harness_server_wait_for_event(turbo_agent_harness_connection_t *connection,
                                         const char *method, uint64_t *inout_sequence) {
  size_t attempt;
  for (attempt = 0; attempt < 100000; ++attempt) {
    json_value_t *params = turbo_json_create_object();
    json_value_t *response;
    const json_value_t *result;
    const json_value_t *events;
    uint64_t next;
    int found;
    turbo_json_object_set_number(params, "afterSequence", (double)*inout_sequence);
    response = harness_server_rpc(connection, 100 + (int)(attempt % 1000), "event/replay", params);
    result = turbo_json_object_get(response, "result");
    check_not_null(result);
    events = turbo_json_object_get(result, "events");
    found = harness_server_events_have_method(events, method);
    {
      const json_value_t *next_json = turbo_json_object_get(result, "nextSequence");
      next = next_json && turbo_json_type(next_json) == TURBO_JSON_NUMBER
                 ? (uint64_t)turbo_json_number(next_json)
                 : 0;
    }
    if (next > *inout_sequence) {
      json_value_t *ack_params = turbo_json_create_object();
      json_value_t *ack_response;
      turbo_json_object_set_number(ack_params, "throughSequence", (double)next);
      ack_response =
          harness_server_rpc(connection, 200 + (int)(attempt % 1000), "event/ack", ack_params);
      check_not_null(turbo_json_object_get(ack_response, "result"));
      turbo_runtime_json_destroy(ack_response);
      *inout_sequence = next;
    }
    turbo_runtime_json_destroy(response);
    if (found) return 1;
    salts_thread_yield();
  }
  return 0;
}

static int harness_server_wait_for_events(turbo_agent_harness_connection_t *connection,
                                          const char *const *methods, size_t method_count,
                                          uint64_t *inout_sequence) {
  size_t attempt;
  size_t next_method = 0;
  for (attempt = 0; attempt < 100000 && next_method < method_count; ++attempt) {
    json_value_t *params = turbo_json_create_object();
    json_value_t *response;
    const json_value_t *result;
    const json_value_t *events;
    const json_value_t *next_json;
    uint64_t next;
    size_t index;
    turbo_json_object_set_number(params, "afterSequence", (double)*inout_sequence);
    response = harness_server_rpc(connection, 300 + (int)(attempt % 1000), "event/replay", params);
    result = turbo_json_object_get(response, "result");
    check_not_null(result);
    events = turbo_json_object_get(result, "events");
    for (index = 0; events && index < turbo_json_array_size(events); ++index) {
      const json_value_t *event = turbo_json_array_get(events, index);
      const char *method = turbo_json_get_string(event, "method");
      if (next_method < method_count && method && strcmp(method, methods[next_method]) == 0) {
        ++next_method;
      }
    }
    next_json = turbo_json_object_get(result, "nextSequence");
    next = next_json && turbo_json_type(next_json) == TURBO_JSON_NUMBER
               ? (uint64_t)turbo_json_number(next_json)
               : 0;
    if (next > *inout_sequence) {
      json_value_t *ack_params = turbo_json_create_object();
      json_value_t *ack_response;
      turbo_json_object_set_number(ack_params, "throughSequence", (double)next);
      ack_response =
          harness_server_rpc(connection, 400 + (int)(attempt % 1000), "event/ack", ack_params);
      check_not_null(turbo_json_object_get(ack_response, "result"));
      turbo_runtime_json_destroy(ack_response);
      *inout_sequence = next;
    }
    turbo_runtime_json_destroy(response);
    if (next_method < method_count) salts_thread_yield();
  }
  return next_method == method_count;
}

static tstr harness_server_wait_for_approval(turbo_agent_harness_connection_t *connection,
                                               uint64_t *inout_sequence) {
  size_t attempt;
  for (attempt = 0; attempt < 100000; ++attempt) {
    json_value_t *params = turbo_json_create_object();
    json_value_t *response;
    const json_value_t *result;
    const json_value_t *events;
    const json_value_t *next_json;
    uint64_t next;
    tstr request_id = NULL;
    size_t index;
    turbo_json_object_set_number(params, "afterSequence", (double)*inout_sequence);
    response = harness_server_rpc(connection, 500 + (int)(attempt % 1000), "event/replay", params);
    result = turbo_json_object_get(response, "result");
    check_not_null(result);
    events = turbo_json_object_get(result, "events");
    for (index = 0; events && index < turbo_json_array_size(events); ++index) {
      const json_value_t *event = turbo_json_array_get(events, index);
      const char *method = turbo_json_get_string(event, "method");
      const json_value_t *event_params = turbo_json_object_get(event, "params");
      if (method && strcmp(method, "item/review/requestApproval") == 0) {
        request_id = tstr_dup(turbo_json_get_string(event_params, "requestId"));
        break;
      }
    }
    next_json = turbo_json_object_get(result, "nextSequence");
    next = next_json && turbo_json_type(next_json) == TURBO_JSON_NUMBER
               ? (uint64_t)turbo_json_number(next_json)
               : 0;
    if (next > *inout_sequence) {
      json_value_t *ack_params = turbo_json_create_object();
      json_value_t *ack_response;
      turbo_json_object_set_number(ack_params, "throughSequence", (double)next);
      ack_response =
          harness_server_rpc(connection, 600 + (int)(attempt % 1000), "event/ack", ack_params);
      check_not_null(turbo_json_object_get(ack_response, "result"));
      turbo_runtime_json_destroy(ack_response);
      *inout_sequence = next;
    }
    turbo_runtime_json_destroy(response);
    if (request_id) return request_id;
    salts_thread_yield();
  }
  return NULL;
}

spec("turbo agent harness server") {
  it("enforces the connection handshake and streams one completed turn") {
    static const char *const expected_events[] = {"thread/started", "turn/started",
                                                  "turn/completed"};
    turbo_threadpool_config_t pool_config = {.num_threads = 2, .queue_capacity = 8};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_factory_t factory = {pool, harness_server_success_transport, NULL,
                                        TURBO_AGENT_SESSION_WORKFLOW_LOOP};
    turbo_agent_harness_server_t *server = harness_server_create(&factory);
    turbo_agent_harness_connection_t *connection =
        turbo_agent_harness_server_open_connection(server);
    json_value_t *response;
    json_value_t *params;
    const json_value_t *result;
    const json_value_t *thread_json;
    const json_value_t *turn_json;
    tstr thread_id;
    tstr turn_id;
    uint64_t sequence = 0;

    check_not_null(pool);
    check_not_null(server);
    check_not_null(connection);
    response = harness_server_rpc(connection, 0, "thread/list", turbo_json_create_object());
    check_int_eq(turbo_json_get_int(turbo_json_object_get(response, "error"), "code", 0), -32002);
    turbo_runtime_json_destroy(response);

    harness_server_ready(connection);
    params = turbo_json_create_object();
    turbo_json_object_set_number(params, "afterSequence", 0.5);
    response = harness_server_rpc(connection, 2, "event/replay", params);
    check_int_eq(turbo_json_get_int(turbo_json_object_get(response, "error"), "code", 0), -32602);
    turbo_runtime_json_destroy(response);
    response = harness_server_rpc(connection, 2, "thread/start", turbo_json_create_object());
    result = turbo_json_object_get(response, "result");
    thread_json = turbo_json_object_get(result, "thread");
    thread_id = tstr_dup(turbo_json_get_string(thread_json, "id"));
    check_not_null(thread_id);
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("hello"));
    response = harness_server_rpc(connection, 3, "turn/start", params);
    result = turbo_json_object_get(response, "result");
    turn_json = turbo_json_object_get(result, "turn");
    turn_id = tstr_dup(turbo_json_get_string(turn_json, "id"));
    check_not_null(turn_id);
    check_str_eq(turbo_json_get_string(turn_json, "status"), "inProgress");
    turbo_runtime_json_destroy(response);

    check_true(harness_server_wait_for_events(connection, expected_events,
                                              sizeof(expected_events) / sizeof(expected_events[0]),
                                              &sequence));

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 4, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "completed");
    turbo_runtime_json_destroy(response);

    tstr_free(turn_id);
    tstr_free(thread_id);
    turbo_agent_harness_connection_close(connection);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }

  it("serializes turns and accepts steer before cooperative interruption") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_gate_t gate;
    harness_server_factory_t factory = {pool, harness_server_gated_transport, &gate,
                                        TURBO_AGENT_SESSION_WORKFLOW_LOOP};
    turbo_agent_harness_server_t *server;
    turbo_agent_harness_connection_t *connection;
    turbo_agent_harness_connection_t *observer;
    json_value_t *response;
    json_value_t *params;
    tstr thread_id;
    tstr turn_id;
    uint64_t sequence = 0;

    atomic_init(&gate.entered, 0);
    atomic_init(&gate.open, 0);
    server = harness_server_create(&factory);
    connection = turbo_agent_harness_server_open_connection(server);
    harness_server_ready(connection);
    response = harness_server_rpc(connection, 10, "thread/start", turbo_json_create_object());
    thread_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "thread"), "id"));
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("first"));
    response = harness_server_rpc(connection, 11, "turn/start", params);
    turn_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "id"));
    turbo_runtime_json_destroy(response);
    while (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      salts_thread_yield();
    }

    observer = turbo_agent_harness_server_open_connection(server);
    check_not_null(observer);
    harness_server_ready(observer);
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    response = harness_server_rpc(observer, 16, "thread/resume", params);
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "expectedTurnId", turn_id);
    turbo_json_object_add(params, "input", harness_server_text_input("hijack"));
    response = harness_server_rpc(observer, 17, "turn/steer", params);
    check_int_eq(turbo_json_get_int(turbo_json_object_get(response, "error"), "code", 0), -32004);
    turbo_runtime_json_destroy(response);
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(observer, 18, "turn/interrupt", params);
    check_int_eq(turbo_json_get_int(turbo_json_object_get(response, "error"), "code", 0), -32005);
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("second"));
    response = harness_server_rpc(connection, 12, "turn/start", params);
    check_int_eq(turbo_json_get_int(turbo_json_object_get(response, "error"), "code", 0), -32004);
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "expectedTurnId", turn_id);
    turbo_json_object_add(params, "input", harness_server_text_input("change direction"));
    response = harness_server_rpc(connection, 13, "turn/steer", params);
    check_str_eq(turbo_json_get_string(turbo_json_object_get(response, "result"), "turnId"),
                 turn_id);
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 14, "turn/interrupt", params);
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    atomic_store_explicit(&gate.open, 1, memory_order_release);
    check_true(harness_server_wait_for_event(connection, "turn/completed", &sequence));

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 15, "turn/get", params);
    check_str_eq(
        turbo_json_get_string(
            turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "status"),
        "interrupted");
    turbo_runtime_json_destroy(response);

    tstr_free(turn_id);
    tstr_free(thread_id);
    turbo_agent_harness_connection_close(observer);
    turbo_agent_harness_connection_close(connection);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }

  it("resolves review approval and resumes the same logical turn") {
    static const char *const expected_events[] = {"serverRequest/resolved", "turn/completed"};
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_factory_t factory = {pool, harness_server_review_transport, NULL,
                                        TURBO_AGENT_SESSION_WORKFLOW_REVIEW};
    turbo_agent_harness_server_t *server = harness_server_create(&factory);
    turbo_agent_harness_connection_t *connection =
        turbo_agent_harness_server_open_connection(server);
    json_value_t *response;
    json_value_t *params;
    json_value_t *interrupts;
    const json_value_t *turn_json;
    tstr thread_id;
    tstr turn_id;
    tstr request_id;
    uint64_t sequence = 0;

    harness_server_ready(connection);
    response = harness_server_rpc(connection, 20, "thread/start", turbo_json_create_object());
    thread_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "thread"), "id"));
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    interrupts = turbo_json_create_array();
    turbo_json_array_add(interrupts, turbo_json_create_string("review"));
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("review me"));
    turbo_json_object_add(params, "interruptBeforeNodes", interrupts);
    response = harness_server_rpc(connection, 21, "turn/start", params);
    turn_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "id"));
    turbo_runtime_json_destroy(response);

    request_id = harness_server_wait_for_approval(connection, &sequence);
    check_not_null(request_id);
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 22, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "approvalPending");
    turbo_runtime_json_destroy(response);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    turbo_json_object_set_string(params, "requestId", request_id);
    turbo_json_object_set_string(params, "decision", "approve_once");
    response = harness_server_rpc(connection, 23, "approval/respond", params);
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    check_true(harness_server_wait_for_events(connection, expected_events,
                                              sizeof(expected_events) / sizeof(expected_events[0]),
                                              &sequence));

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 24, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "completed");
    turbo_runtime_json_destroy(response);

    tstr_free(request_id);
    tstr_free(turn_id);
    tstr_free(thread_id);
    response = harness_server_rpc(connection, 25, "thread/start", turbo_json_create_object());
    thread_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "thread"), "id"));
    turbo_runtime_json_destroy(response);
    params = turbo_json_create_object();
    interrupts = turbo_json_create_array();
    turbo_json_array_add(interrupts, turbo_json_create_string("review"));
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("deny me"));
    turbo_json_object_add(params, "interruptBeforeNodes", interrupts);
    response = harness_server_rpc(connection, 26, "turn/start", params);
    turn_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "id"));
    turbo_runtime_json_destroy(response);
    request_id = harness_server_wait_for_approval(connection, &sequence);
    check_not_null(request_id);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    turbo_json_object_set_string(params, "requestId", request_id);
    turbo_json_object_set_string(params, "decision", "deny");
    response = harness_server_rpc(connection, 27, "approval/respond", params);
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    check_true(harness_server_wait_for_events(connection, expected_events,
                                              sizeof(expected_events) / sizeof(expected_events[0]),
                                              &sequence));
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 28, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "declined");
    turbo_runtime_json_destroy(response);

    tstr_free(request_id);
    tstr_free(turn_id);
    tstr_free(thread_id);
    response = harness_server_rpc(connection, 29, "thread/start", turbo_json_create_object());
    thread_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "thread"), "id"));
    turbo_runtime_json_destroy(response);
    params = turbo_json_create_object();
    interrupts = turbo_json_create_array();
    turbo_json_array_add(interrupts, turbo_json_create_string("review"));
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("interrupt review"));
    turbo_json_object_add(params, "interruptBeforeNodes", interrupts);
    response = harness_server_rpc(connection, 30, "turn/start", params);
    turn_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "id"));
    turbo_runtime_json_destroy(response);
    request_id = harness_server_wait_for_approval(connection, &sequence);
    check_not_null(request_id);

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 31, "turn/interrupt", params);
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    check_true(harness_server_wait_for_events(connection, expected_events,
                                              sizeof(expected_events) / sizeof(expected_events[0]),
                                              &sequence));
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(connection, 32, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "interrupted");
    turbo_runtime_json_destroy(response);

    tstr_free(request_id);
    tstr_free(turn_id);
    tstr_free(thread_id);
    turbo_agent_harness_connection_close(connection);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }

  it("cancels and joins an active turn when its owner connection closes") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_gate_t gate;
    harness_server_factory_t factory = {pool, harness_server_gated_transport, &gate,
                                        TURBO_AGENT_SESSION_WORKFLOW_LOOP};
    turbo_agent_harness_server_t *server = harness_server_create(&factory);
    turbo_agent_harness_connection_t *owner = turbo_agent_harness_server_open_connection(server);
    turbo_agent_harness_connection_t *observer = turbo_agent_harness_server_open_connection(server);
    harness_server_close_task_t close_task = {0};
    salts_thread_t closer = NULL;
    json_value_t *response;
    json_value_t *params;
    const json_value_t *turn_json;
    tstr thread_id;
    tstr turn_id;

    atomic_init(&gate.entered, 0);
    atomic_init(&gate.open, 0);
    harness_server_ready(owner);
    harness_server_ready(observer);
    response = harness_server_rpc(owner, 40, "thread/start", turbo_json_create_object());
    thread_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "thread"), "id"));
    turbo_runtime_json_destroy(response);
    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_add(params, "input", harness_server_text_input("block"));
    response = harness_server_rpc(owner, 41, "turn/start", params);
    turn_id = tstr_dup(turbo_json_get_string(
        turbo_json_object_get(turbo_json_object_get(response, "result"), "turn"), "id"));
    turbo_runtime_json_destroy(response);
    while (!atomic_load_explicit(&gate.entered, memory_order_acquire)) {
      salts_thread_yield();
    }

    close_task.connection = owner;
    atomic_init(&close_task.entered, 0);
    atomic_init(&close_task.done, 0);
    check_int_eq(salts_thread_create(&closer, harness_server_close_connection_task, &close_task),
                 0);
    while (!atomic_load_explicit(&close_task.entered, memory_order_acquire)) {
      salts_thread_yield();
    }
    atomic_store_explicit(&gate.open, 1, memory_order_release);
    check_int_eq(salts_thread_join(&closer), 0);
    salts_thread_destroy(&closer);
    check_true(atomic_load_explicit(&close_task.done, memory_order_acquire));

    params = turbo_json_create_object();
    turbo_json_object_set_string(params, "threadId", thread_id);
    turbo_json_object_set_string(params, "turnId", turn_id);
    response = harness_server_rpc(observer, 42, "turn/get", params);
    turn_json = turbo_json_object_get(turbo_json_object_get(response, "result"), "turn");
    check_str_eq(turbo_json_get_string(turn_json, "status"), "interrupted");
    turbo_runtime_json_destroy(response);

    tstr_free(turn_id);
    tstr_free(thread_id);
    turbo_agent_harness_connection_close(observer);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }

  it("wakes a blocking journal consumer and releases only acknowledged events") {
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_factory_t factory = {pool, harness_server_success_transport, NULL,
                                        TURBO_AGENT_SESSION_WORKFLOW_LOOP};
    turbo_agent_harness_server_t *server = harness_server_create(&factory);
    turbo_agent_harness_connection_t *connection =
        turbo_agent_harness_server_open_connection(server);
    harness_server_event_wait_task_t wait_task = {0};
    salts_thread_t waiter = NULL;
    json_value_t *response;
    json_value_t *event = NULL;
    uint64_t sequence = 0;

    check_not_null(pool);
    check_not_null(server);
    check_not_null(connection);
    harness_server_ready(connection);
    wait_task.connection = connection;
    atomic_init(&wait_task.entered, 0);
    check_int_eq(salts_thread_create(&waiter, harness_server_wait_event_task, &wait_task), 0);
    while (!atomic_load_explicit(&wait_task.entered, memory_order_acquire))
      salts_thread_yield();

    response = harness_server_rpc(connection, 50, "thread/start", turbo_json_create_object());
    check_not_null(turbo_json_object_get(response, "result"));
    turbo_runtime_json_destroy(response);
    check_int_eq(salts_thread_join(&waiter), 0);
    salts_thread_destroy(&waiter);
    check_int_eq(wait_task.rc, SALTS_OK);
    check_not_null(wait_task.event);
    check_str_eq(turbo_json_get_string(wait_task.event, "method"), "thread/started");
    check_true(wait_task.sequence == 1);
    check_true((uint64_t)turbo_json_number(turbo_json_object_get(
                   turbo_json_object_get(wait_task.event, "params"), "sequence")) ==
               wait_task.sequence);
    turbo_runtime_json_destroy(wait_task.event);

    check_int_eq(
        turbo_agent_harness_connection_wait_event_json_value(connection, 0, &event, &sequence),
        SALTS_OK);
    check_true(sequence == wait_task.sequence);
    turbo_runtime_json_destroy(event);
    check_int_eq(turbo_agent_harness_connection_ack_events(connection, sequence), SALTS_OK);
    event = NULL;
    sequence = 0;
    check_int_eq(
        turbo_agent_harness_connection_wait_event_json_value(connection, 0, &event, &sequence),
        SALTS_ETIMEDOUT);
    check_null(event);
    check_int_eq(turbo_agent_harness_connection_ack_events(connection, 2), SALTS_EINVAL);

    turbo_agent_harness_connection_close(connection);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }

  it("frames JSONL responses and replays an event after writer failure") {
    static const char initialize_line[] =
        "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{"
        "\"name\":\"tinytest\",\"title\":\"TinyTest\",\"version\":\"1.0\"}}}";
    static const char initialized_line[] = "{\"method\":\"initialized\",\"params\":{}}";
    static const char thread_start_line[] = "{\"id\":2,\"method\":\"thread/start\",\"params\":{}}";
    turbo_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 4};
    turbo_threadpool_t *pool = turbo_threadpool_create_with_config(&pool_config);
    harness_server_factory_t factory = {pool, harness_server_success_transport, NULL,
                                        TURBO_AGENT_SESSION_WORKFLOW_LOOP};
    turbo_agent_harness_server_t *server = harness_server_create(&factory);
    turbo_agent_harness_connection_t *connection =
        turbo_agent_harness_server_open_connection(server);
    harness_server_frame_capture_t capture = {0};
    turbo_agent_harness_jsonl_transport_config_t config;
    turbo_agent_harness_jsonl_transport_t *transport;
    json_value_t *frame;
    const json_value_t *params;
    size_t event_count = 0;

    turbo_agent_harness_jsonl_transport_config_init(&config);
    config.connection = connection;
    config.write = harness_server_capture_frame;
    config.write_user_data = &capture;
    config.max_event_batch = 8;
    transport = turbo_agent_harness_jsonl_transport_create(&config);
    check_not_null(pool);
    check_not_null(server);
    check_not_null(connection);
    check_not_null(transport);

    check_int_eq(turbo_agent_harness_jsonl_transport_dispatch_line(transport, initialize_line),
                 SALTS_OK);
    check_true(capture.frame_count == 1);
    frame = harness_server_parse_frame(&capture, 0);
    check_not_null(turbo_json_object_get(frame, "result"));
    turbo_runtime_json_destroy(frame);
    check_int_eq(turbo_agent_harness_jsonl_transport_dispatch_line(transport, initialized_line),
                 SALTS_OK);
    check_true(capture.frame_count == 1);
    check_int_eq(turbo_agent_harness_jsonl_transport_dispatch_line(transport, thread_start_line),
                 SALTS_OK);
    check_true(capture.frame_count == 2);
    check_int_eq(turbo_agent_harness_jsonl_transport_pump_events(transport, 1000, &event_count),
                 SALTS_OK);
    check_true(event_count == 1);
    check_true(capture.frame_count == 3);
    frame = harness_server_parse_frame(&capture, 2);
    check_str_eq(turbo_json_get_string(frame, "method"), "thread/started");
    params = turbo_json_object_get(frame, "params");
    check_true((uint64_t)turbo_json_number(turbo_json_object_get(params, "sequence")) == 1);
    turbo_runtime_json_destroy(frame);

    check_int_eq(turbo_agent_harness_jsonl_transport_dispatch_line(transport, thread_start_line),
                 SALTS_OK);
    check_true(capture.frame_count == 4);
    capture.fail_writes = 1;
    event_count = 99;
    check_int_eq(turbo_agent_harness_jsonl_transport_pump_events(transport, 1000, &event_count),
                 SALTS_EIO);
    check_true(event_count == 0);
    check_true(capture.frame_count == 4);
    capture.fail_writes = 0;
    check_int_eq(turbo_agent_harness_jsonl_transport_pump_events(transport, 0, &event_count),
                 SALTS_OK);
    check_true(event_count == 1);
    check_true(capture.frame_count == 5);
    frame = harness_server_parse_frame(&capture, 4);
    params = turbo_json_object_get(frame, "params");
    check_str_eq(turbo_json_get_string(frame, "method"), "thread/started");
    check_true((uint64_t)turbo_json_number(turbo_json_object_get(params, "sequence")) == 2);
    turbo_runtime_json_destroy(frame);

    event_count = 99;
    check_int_eq(turbo_agent_harness_jsonl_transport_pump_events(transport, 0, &event_count),
                 SALTS_ETIMEDOUT);
    check_true(event_count == 0);
    check_int_eq(turbo_agent_harness_jsonl_transport_dispatch_line(
                     transport, "{\"method\":\"initialized\"}\n"),
                 SALTS_EPROTO);

    turbo_agent_harness_connection_close(connection);
    event_count = 99;
    check_int_eq(turbo_agent_harness_jsonl_transport_pump_events(transport, 0, &event_count),
                 SALTS_ESHUTDOWN);
    check_true(event_count == 0);
    turbo_agent_harness_jsonl_transport_destroy(transport);
    harness_server_capture_destroy(&capture);
    turbo_agent_harness_server_destroy(server);
    turbo_threadpool_destroy(pool);
  }
}
