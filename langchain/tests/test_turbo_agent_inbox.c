#include "tinytest.h"

#include "turbo_agent_inbox.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_thread.h>

static turbo_agent_session_t *inbox_create_session(const char *thread_id,
                                                   turbo_agent_runtime_store_t store,
                                                   const turbo_agent_inbox_config_t *inbox_config) {
  turbo_agent_session_config_t session_config = {0};
  turbo_agent_session_t *session;

  session_config.runtime_store = store;
  session_config.thread_id = thread_id;
  session = turbo_agent_session_create(&session_config);
  check_not_null(session);
  if (session) {
    check_int_eq(turbo_agent_session_inbox_configure(session, inbox_config), SALTS_OK);
  }
  return session;
}

static json_value_t *inbox_message(const char *text) {
  json_value_t *message = turbo_json_create_object();

  check_not_null(message);
  if (message) {
    turbo_json_object_set_string(message, "role", "user");
    turbo_json_object_set_string(message, "content", text);
  }
  return message;
}

typedef struct inbox_transport_capture_s {
  char *request_json;
  size_t call_count;
} inbox_transport_capture_t;

static int inbox_success_transport(const char *request_json, char **out_response_json,
                                   void *user_data) {
  static const char response[] = "{\"id\":\"resp_inbox\",\"output\":[{\"type\":\"message\","
                                 "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
                                 "\"text\":\"ok\"}]}]}";
  inbox_transport_capture_t *capture = (inbox_transport_capture_t *)user_data;
  size_t request_size;
  size_t response_size;

  if (!request_json || !out_response_json) return -1;
  free(capture->request_json);
  capture->request_json = NULL;
  request_size = strlen(request_json) + 1;
  response_size = sizeof(response);
  capture->request_json = (char *)malloc(request_size);
  *out_response_json = (char *)malloc(response_size);
  if (!capture->request_json || !*out_response_json) {
    free(capture->request_json);
    capture->request_json = NULL;
    free(*out_response_json);
    *out_response_json = NULL;
    return -1;
  }
  memcpy(capture->request_json, request_json, request_size);
  memcpy(*out_response_json, response, response_size);
  ++capture->call_count;
  return 0;
}

typedef struct inbox_enqueue_task_s {
  turbo_agent_session_t *session;
  const json_value_t *message;
  turbo_agent_inbox_kind_t kind;
  atomic_int entered;
  int result;
  char *inbox_id;
} inbox_enqueue_task_t;

typedef struct inbox_failing_store_s {
  turbo_agent_runtime_store_t base;
  int fail_next_applied_transition;
} inbox_failing_store_t;

static int inbox_failing_store_put(void *user_data, const char *collection, const char *id,
                                   const char *record_json) {
  inbox_failing_store_t *store = (inbox_failing_store_t *)user_data;

  if (store->fail_next_applied_transition && strcmp(collection, "agent_inbox_transitions") == 0 &&
      strstr(record_json, "\"status\":\"applied\"") != NULL) {
    store->fail_next_applied_transition = 0;
    return -1;
  }
  return store->base.put(store->base.user_data, collection, id, record_json);
}

static int inbox_failing_store_get(void *user_data, const char *collection, const char *id,
                                   char **out_record_json) {
  inbox_failing_store_t *store = (inbox_failing_store_t *)user_data;

  return store->base.get(store->base.user_data, collection, id, out_record_json);
}

static int inbox_failing_store_list(void *user_data, const char *collection, const char *filter_key,
                                    const char *filter_value, char **out_records_json) {
  inbox_failing_store_t *store = (inbox_failing_store_t *)user_data;

  return store->base.list(store->base.user_data, collection, filter_key, filter_value,
                          out_records_json);
}

static void inbox_enqueue_task(void *user_data) {
  inbox_enqueue_task_t *task = (inbox_enqueue_task_t *)user_data;

  atomic_store_explicit(&task->entered, 1, memory_order_release);
  task->result = turbo_agent_session_enqueue(task->session, task->kind, task->message,
                                             TURBO_AGENT_INBOX_WAIT_INFINITE, &task->inbox_id);
}

spec("turbo agent inbox") {
  it("persists immutable payloads through claim requeue and apply") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 2};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_session_t *session = inbox_create_session("inbox-lifecycle", store, &config);
    json_value_t *message = inbox_message("original");
    json_value_t *status = NULL;
    json_value_t *claimed = NULL;
    const json_value_t *payload;
    char *inbox_id = NULL;

    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id),
        SALTS_OK);
    check_not_null(inbox_id);
    turbo_json_object_set_string(message, "content", "mutated");

    check_int_eq(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "queued");
    payload = turbo_json_object_get(status, "payload");
    check_str_eq(turbo_json_get_string(payload, "content"), "original");
    turbo_runtime_json_destroy(status);
    status = NULL;

    check_int_eq(
        turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_STEER, "run-1", &claimed),
        SALTS_OK);
    check_str_eq(turbo_json_get_string(claimed, "inbox_id"), inbox_id);
    check_int_eq(
        turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_FOLLOW_UP, "run-1", &status),
        SALTS_EBUSY);
    check_null(status);
    turbo_runtime_json_destroy(claimed);
    claimed = NULL;

    check_int_eq(turbo_agent_session_inbox_requeue(session, inbox_id), SALTS_OK);
    check_int_eq(
        turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_STEER, "run-2", &claimed),
        SALTS_OK);
    turbo_runtime_json_destroy(claimed);
    claimed = NULL;
    check_int_eq(turbo_agent_session_inbox_mark_applied(session, inbox_id, "event-1"), SALTS_OK);
    check_int_eq(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "applied");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(status, "latest_transition"),
                                       "applied_event_id"),
                 "event-1");
    turbo_runtime_json_destroy(status);
    status = NULL;
    check_int_eq(
        turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_STEER, "run-3", &claimed),
        SALTS_ENOENT);
    check_null(claimed);

    free(inbox_id);
    turbo_runtime_json_destroy(message);
    turbo_agent_session_destroy(session);
  }

  it("keeps claimed items inside the capacity budget") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 1, 4096, 1024, 2};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_session_t *session = inbox_create_session("inbox-capacity", store, &config);
    json_value_t *first = inbox_message("first");
    json_value_t *second = inbox_message("second");
    json_value_t *claimed = NULL;
    char *first_id = NULL;
    char *second_id = NULL;

    check_int_eq(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, first, 0, &first_id),
                 SALTS_OK);
    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, second, 0, &second_id),
        SALTS_EBUSY);
    check_null(second_id);
    check_int_eq(
        turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_STEER, "run-capacity", &claimed),
        SALTS_OK);
    turbo_runtime_json_destroy(claimed);
    claimed = NULL;
    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, second, 0, &second_id),
        SALTS_EBUSY);
    check_int_eq(turbo_agent_session_inbox_mark_applied(session, first_id, "event-capacity"),
                 SALTS_OK);
    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, second, 0, &second_id),
        SALTS_OK);

    free(first_id);
    free(second_id);
    turbo_runtime_json_destroy(first);
    turbo_runtime_json_destroy(second);
    turbo_agent_session_destroy(session);
  }

  it("wakes blocked producers when the inbox closes") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 1, 4096, 1024, 2};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_session_t *session = inbox_create_session("inbox-close", store, &config);
    json_value_t *first = inbox_message("first");
    json_value_t *blocked = inbox_message("blocked");
    char *first_id = NULL;
    turbo_thread_t producer = NULL;
    inbox_enqueue_task_t task = {0};

    check_int_eq(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, first, 0, &first_id),
                 SALTS_OK);
    task.session = session;
    task.message = blocked;
    task.kind = TURBO_AGENT_INBOX_FOLLOW_UP;
    task.result = TURBO_UNKNOWN;
    atomic_init(&task.entered, 0);
    check_int_eq(turbo_thread_create(&producer, inbox_enqueue_task, &task), 0);
    while (!atomic_load_explicit(&task.entered, memory_order_acquire)) {
      turbo_thread_yield();
    }
    check_int_eq(turbo_agent_session_inbox_close(session), SALTS_OK);
    check_int_eq(turbo_thread_join(&producer), 0);
    turbo_thread_destroy(&producer);
    check_int_eq(task.result, SALTS_ECANCELED);
    check_null(task.inbox_id);
    check_int_eq(turbo_agent_session_inbox_close(session), SALTS_EALREADY);

    free(first_id);
    turbo_runtime_json_destroy(first);
    turbo_runtime_json_destroy(blocked);
    turbo_agent_session_destroy(session);
  }

  it("requeues a claim automatically when recovery finds no durable event") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 2, 4096, 1024, 2};
    turbo_agent_runtime_store_t owned_store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_store_t shared_store = owned_store;
    turbo_agent_session_t *first_session;
    turbo_agent_session_t *second_session;
    json_value_t *message = inbox_message("recover me");
    json_value_t *claimed = NULL;
    char *inbox_id = NULL;

    shared_store.user_data_free = NULL;
    first_session = inbox_create_session("inbox-recovery", shared_store, &config);
    check_int_eq(turbo_agent_session_enqueue(first_session, TURBO_AGENT_INBOX_FOLLOW_UP, message, 0,
                                             &inbox_id),
                 SALTS_OK);
    check_int_eq(turbo_agent_session_inbox_claim(first_session, TURBO_AGENT_INBOX_FOLLOW_UP,
                                                 "run-before-restart", &claimed),
                 SALTS_OK);
    turbo_runtime_json_destroy(claimed);
    claimed = NULL;
    turbo_agent_session_destroy(first_session);

    second_session = inbox_create_session("inbox-recovery", shared_store, &config);
    check_int_eq(turbo_agent_session_inbox_claim(second_session, TURBO_AGENT_INBOX_FOLLOW_UP,
                                                 "run-after-restart", &claimed),
                 SALTS_OK);
    check_str_eq(turbo_json_get_string(claimed, "inbox_id"), inbox_id);
    turbo_runtime_json_destroy(claimed);
    check_int_eq(
        turbo_agent_session_inbox_mark_applied(second_session, inbox_id, "event-after-restart"),
        SALTS_OK);

    turbo_agent_session_destroy(second_session);
    owned_store.user_data_free(owned_store.user_data);
    free(inbox_id);
    turbo_runtime_json_destroy(message);
  }

  it("injects steering before the next model request and commits its event") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 2};
    turbo_agent_session_config_t session_config = {0};
    inbox_transport_capture_t capture = {0};
    turbo_agent_session_t *session;
    json_value_t *message = inbox_message("change direction now");
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    json_value_t *status = NULL;
    const json_value_t *input;
    const json_value_t *event = NULL;
    char *inbox_id = NULL;
    size_t index;

    session_config.thread_id = "inbox-steer-safe-point";
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.agent_config.model = "gpt-5.4";
    session_config.agent_config.transport_fn = inbox_success_transport;
    session_config.agent_config.transport_user_data = &capture;
    session = turbo_agent_session_create(&session_config);
    check_not_null(session);
    check_int_eq(turbo_agent_session_inbox_configure(session, &config), SALTS_OK);
    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id),
        SALTS_OK);
    check_int_eq(turbo_agent_session_start_text(session, "initial request", NULL, &summary, &state),
                 SALTS_OK);
    check_not_null(capture.request_json);
    check_not_null(strstr(capture.request_json, "change direction now"));
    input = turbo_json_object_get(state, "input");
    check_not_null(input);
    check_size_eq(turbo_json_array_size(input), 2);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(input, 1), "content"),
                 "change direction now");
    for (index = 0; index < turbo_agent_state_event_count(state); ++index) {
      const json_value_t *candidate = turbo_agent_state_event_at(state, index);
      if (candidate && turbo_json_get_string(candidate, "kind") &&
          strcmp(turbo_json_get_string(candidate, "kind"), "inbox_message") == 0) {
        event = candidate;
        break;
      }
    }
    check_not_null(event);
    check_str_eq(turbo_json_get_string(event, "inbox_id"), inbox_id);
    check_not_null(turbo_json_get_string(event, "event_id"));
    check_int_eq(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "applied");
    check_str_eq(turbo_json_get_string(turbo_json_object_get(status, "latest_transition"),
                                       "applied_event_id"),
                 turbo_json_get_string(event, "event_id"));

    free(capture.request_json);
    free(inbox_id);
    turbo_runtime_json_destroy(status);
    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(message);
    turbo_agent_session_destroy(session);
  }

  it("starts a bounded next turn for a queued follow-up") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    turbo_agent_session_config_t session_config = {0};
    inbox_transport_capture_t capture = {0};
    turbo_agent_session_t *session;
    json_value_t *message = inbox_message("one more request");
    json_value_t *deferred_message = inbox_message("deferred request");
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    json_value_t *status = NULL;
    const json_value_t *input;
    char *inbox_id = NULL;
    char *deferred_id = NULL;

    session_config.thread_id = "inbox-follow-up";
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.agent_config.model = "gpt-5.4";
    session_config.agent_config.transport_fn = inbox_success_transport;
    session_config.agent_config.transport_user_data = &capture;
    session = turbo_agent_session_create(&session_config);
    check_not_null(session);
    check_int_eq(turbo_agent_session_inbox_configure(session, &config), SALTS_OK);
    check_int_eq(
        turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_FOLLOW_UP, message, 0, &inbox_id),
        SALTS_OK);
    check_int_eq(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_FOLLOW_UP, deferred_message,
                                             0, &deferred_id),
                 SALTS_OK);
    check_int_eq(turbo_agent_session_start_text(session, "initial request", NULL, &summary, &state),
                 SALTS_OK);
    check_size_eq(capture.call_count, 2);
    check_not_null(strstr(capture.request_json, "one more request"));
    input = turbo_json_object_get(state, "input");
    check_size_eq(turbo_json_array_size(input), 2);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(input, 1), "content"),
                 "one more request");
    check_int_eq(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "applied");
    turbo_runtime_json_destroy(status);
    status = NULL;
    check_int_eq(turbo_agent_session_inbox_status(session, deferred_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "queued");

    free(capture.request_json);
    free(inbox_id);
    free(deferred_id);
    turbo_runtime_json_destroy(status);
    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(message);
    turbo_runtime_json_destroy(deferred_message);
    turbo_agent_session_destroy(session);
  }

  it("recovers applied when history committed before the transition write") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    turbo_agent_session_config_t first_config = {0};
    turbo_agent_session_config_t second_config = {0};
    inbox_transport_capture_t capture = {0};
    inbox_failing_store_t failing_store = {0};
    turbo_agent_runtime_store_t shared_store = {0};
    turbo_agent_session_t *first_session;
    turbo_agent_session_t *second_session;
    json_value_t *message = inbox_message("durable steering");
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    json_value_t *status = NULL;
    char *inbox_id = NULL;

    failing_store.base = turbo_agent_runtime_store_memory_create();
    failing_store.fail_next_applied_transition = 1;
    shared_store.put = inbox_failing_store_put;
    shared_store.get = inbox_failing_store_get;
    shared_store.list = inbox_failing_store_list;
    shared_store.user_data = &failing_store;
    first_config.thread_id = "inbox-recover-applied";
    first_config.runtime_store = shared_store;
    first_config.agent_config.model = "gpt-5.4";
    first_config.agent_config.transport_fn = inbox_success_transport;
    first_config.agent_config.transport_user_data = &capture;
    first_session = turbo_agent_session_create(&first_config);
    check_not_null(first_session);
    check_int_eq(turbo_agent_session_inbox_configure(first_session, &config), SALTS_OK);
    check_int_eq(
        turbo_agent_session_enqueue(first_session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id),
        SALTS_OK);
    check_int_eq(turbo_agent_session_start_text(first_session, "initial", NULL, &summary, &state),
                 SALTS_EIO);
    check_int_eq(turbo_agent_session_inbox_status(first_session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "claimed");
    turbo_runtime_json_destroy(status);
    status = NULL;
    turbo_runtime_json_destroy(state);
    turbo_runtime_json_destroy(summary);
    turbo_agent_session_destroy(first_session);

    second_config.thread_id = "inbox-recover-applied";
    second_config.runtime_store = shared_store;
    second_session = turbo_agent_session_create(&second_config);
    check_not_null(second_session);
    check_int_eq(turbo_agent_session_inbox_configure(second_session, &config), SALTS_OK);
    check_int_eq(turbo_agent_session_inbox_status(second_session, inbox_id, &status), SALTS_OK);
    check_str_eq(turbo_json_get_string(status, "status"), "applied");

    free(capture.request_json);
    free(inbox_id);
    turbo_runtime_json_destroy(status);
    turbo_runtime_json_destroy(message);
    turbo_agent_session_destroy(second_session);
    failing_store.base.user_data_free(failing_store.base.user_data);
  }

  it("accepts concurrent producers without losing durable records") {
    turbo_agent_inbox_config_t config = {
        sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 2};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_session_t *session = inbox_create_session("inbox-mpsc", store, &config);
    json_value_t *first = inbox_message("producer one");
    json_value_t *second = inbox_message("producer two");
    inbox_enqueue_task_t tasks[2] = {0};
    turbo_thread_t producers[2] = {NULL, NULL};
    json_value_t *claimed = NULL;
    size_t index;

    tasks[0].session = session;
    tasks[0].message = first;
    tasks[0].kind = TURBO_AGENT_INBOX_STEER;
    tasks[1].session = session;
    tasks[1].message = second;
    tasks[1].kind = TURBO_AGENT_INBOX_STEER;
    for (index = 0; index < 2; ++index) {
      tasks[index].result = TURBO_UNKNOWN;
      atomic_init(&tasks[index].entered, 0);
      check_int_eq(turbo_thread_create(&producers[index], inbox_enqueue_task, &tasks[index]), 0);
    }
    for (index = 0; index < 2; ++index) {
      check_int_eq(turbo_thread_join(&producers[index]), 0);
      turbo_thread_destroy(&producers[index]);
      check_int_eq(tasks[index].result, SALTS_OK);
      check_not_null(tasks[index].inbox_id);
    }
    check_true(strcmp(tasks[0].inbox_id, tasks[1].inbox_id) != 0);
    for (index = 0; index < 2; ++index) {
      const char *claimed_id;
      check_int_eq(
          turbo_agent_session_inbox_claim(session, TURBO_AGENT_INBOX_STEER, "run-mpsc", &claimed),
          SALTS_OK);
      claimed_id = turbo_json_get_string(claimed, "inbox_id");
      check_not_null(claimed_id);
      check_int_eq(turbo_agent_session_inbox_mark_applied(session, claimed_id, "event-mpsc"),
                   SALTS_OK);
      turbo_runtime_json_destroy(claimed);
      claimed = NULL;
    }

    for (index = 0; index < 2; ++index) {
      free(tasks[index].inbox_id);
    }
    turbo_runtime_json_destroy(first);
    turbo_runtime_json_destroy(second);
    turbo_agent_session_destroy(session);
  }
}
