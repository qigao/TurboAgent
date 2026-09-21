#include "turbo_agent_harness_server_internal.h"

#include "turbo_agent_state.h"
#include <json_parser.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_deque.h>
#include <turbo_str.h>
#include <turbo_thread.h>
#include <turbo_uuid.h>
#include <turbo_vec.h>

enum {
  TURBO_AGENT_HARNESS_RPC_INVALID_REQUEST = -32600,
  TURBO_AGENT_HARNESS_RPC_METHOD_NOT_FOUND = -32601,
  TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS = -32602,
  TURBO_AGENT_HARNESS_RPC_INTERNAL = -32603,
  TURBO_AGENT_HARNESS_RPC_NOT_INITIALIZED = -32002,
  TURBO_AGENT_HARNESS_RPC_ALREADY_INITIALIZED = -32003,
  TURBO_AGENT_HARNESS_RPC_BUSY = -32004,
  TURBO_AGENT_HARNESS_RPC_NOT_FOUND = -32005,
  TURBO_AGENT_HARNESS_RPC_STREAM_FAILED = -32006,
  TURBO_AGENT_HARNESS_MAX_INTERRUPT_NODES = 64,
  TURBO_AGENT_HARNESS_CONTROL_EVENT_RESERVE = 3,
  TURBO_AGENT_HARNESS_EVENT_WAIT_SLICE_MS = 10,
  TURBO_AGENT_HARNESS_MS_TO_NS = 1000000
};

typedef enum turbo_agent_harness_connection_state_e {
  TURBO_AGENT_HARNESS_CONNECTION_NEW = 0,
  TURBO_AGENT_HARNESS_CONNECTION_INITIALIZED = 1,
  TURBO_AGENT_HARNESS_CONNECTION_READY = 2
} turbo_agent_harness_connection_state_t;

typedef struct turbo_agent_harness_event_s {
  uint64_t sequence;
  size_t bytes;
  json_value_t *message;
} turbo_agent_harness_event_t;

typedef struct turbo_agent_harness_turn_s turbo_agent_harness_turn_t;

typedef struct turbo_agent_harness_thread_s {
  tstr_t id;
  turbo_agent_harness_t *harness;
  turbo_mutex_t mutex;
  turbo_agent_harness_turn_t *turn;
} turbo_agent_harness_thread_t;

struct turbo_agent_harness_connection_s {
  atomic_size_t ref_count;
  turbo_agent_harness_server_t *server;
  turbo_mutex_t dispatch_mutex;
  turbo_mutex_t event_mutex;
  turbo_cond_t event_changed;
  turbo_deque_t events;
  size_t event_bytes;
  uint64_t next_sequence;
  uint64_t acknowledged_sequence;
  int closed;
  int stream_error;
  turbo_agent_harness_connection_state_t state;
  int experimental_api;
};

struct turbo_agent_harness_turn_s {
  tstr_t id;
  tstr_t approval_request_id;
  turbo_agent_harness_thread_t *thread;
  turbo_agent_harness_connection_t *connection;
  turbo_agent_harness_execution_t *execution;
  json_value_t *summary;
  json_value_t *state;
  int starting;
  int refreshing;
  int finalized;
  int awaiting_approval;
  int interrupt_requested;
};

typedef struct turbo_agent_harness_turn_snapshot_s {
  char id[TURBO_UUID_STRING_SIZE];
  char approval_request_id[TURBO_UUID_STRING_SIZE];
  const char *thread_id;
  const char *status;
} turbo_agent_harness_turn_snapshot_t;

struct turbo_agent_harness_server_s {
  turbo_mutex_t mutex;
  turbo_agent_harness_server_config_t config;
  turbo_vec_t threads;
};

static const uint64_t TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER = UINT64_C(9007199254740991);

static uint64_t turbo_agent_harness_saturating_add_ms(uint64_t left, uint64_t right) {
  return right > UINT64_MAX - left ? UINT64_MAX : left + right;
}

static int turbo_agent_harness_server_uuid(char out[TURBO_UUID_STRING_SIZE]) {
  turbo_uuid_t uuid;
  int rc = turbo_uuid_v7_generate(&uuid);
  if (rc != TURBO_OK) return rc;
  return turbo_uuid_format(&uuid, out, TURBO_UUID_STRING_SIZE);
}

turbo_agent_harness_connection_t *
turbo_agent_harness_connection_retain_internal(turbo_agent_harness_connection_t *connection) {
  size_t current;
  if (!connection) return NULL;
  current = atomic_load_explicit(&connection->ref_count, memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == SIZE_MAX) return NULL;
    if (atomic_compare_exchange_weak_explicit(&connection->ref_count, &current, current + 1,
                                              memory_order_relaxed, memory_order_relaxed)) {
      return connection;
    }
  }
}

static void turbo_agent_harness_event_destroy(turbo_agent_harness_event_t *event) {
  if (!event) return;
  turbo_runtime_json_destroy(event->message);
  memset(event, 0, sizeof(*event));
}

void turbo_agent_harness_connection_release_internal(turbo_agent_harness_connection_t *connection) {
  turbo_agent_harness_event_t event;
  if (!connection) return;
  if (atomic_fetch_sub_explicit(&connection->ref_count, 1, memory_order_acq_rel) != 1) return;
  atomic_thread_fence(memory_order_acquire);
  while (turbo_deque_pop_front(&connection->events, &event) == TURBO_OK) {
    turbo_agent_harness_event_destroy(&event);
  }
  turbo_deque_destroy(&connection->events);
  turbo_cond_destroy(&connection->event_changed);
  turbo_mutex_destroy(&connection->event_mutex);
  turbo_mutex_destroy(&connection->dispatch_mutex);
  free(connection);
}

static void
turbo_agent_harness_connection_set_stream_error(turbo_agent_harness_connection_t *connection,
                                                int error) {
  if (!connection || error == TURBO_OK) return;
  turbo_mutex_lock(&connection->event_mutex);
  if (connection->stream_error == TURBO_OK) connection->stream_error = error;
  turbo_cond_broadcast(&connection->event_changed);
  turbo_mutex_unlock(&connection->event_mutex);
}

static int turbo_agent_harness_connection_enqueue(turbo_agent_harness_connection_t *connection,
                                                  json_value_t *message, int wait_for_capacity) {
  turbo_agent_harness_event_t event = {0};
  char *serialized = NULL;
  size_t bytes = 0;
  size_t event_limit;
  size_t byte_limit;
  int rc = TURBO_OK;

  if (!connection || !message) return TURBO_EINVAL;
  serialized = turbo_json_serialize(message, &bytes);
  if (!serialized) return TURBO_ENOMEM;
  turbo_json_serialize_free(serialized);
  if (bytes > connection->server->config.max_single_event_bytes ||
      bytes > connection->server->config.max_event_bytes) {
    turbo_agent_harness_connection_set_stream_error(connection, TURBO_EMSGSIZE);
    return TURBO_EMSGSIZE;
  }

  event_limit = connection->server->config.max_event_count;
  byte_limit = connection->server->config.max_event_bytes;
  if (wait_for_capacity) {
    event_limit -= TURBO_AGENT_HARNESS_CONTROL_EVENT_RESERVE;
    byte_limit -= TURBO_AGENT_HARNESS_CONTROL_EVENT_RESERVE *
                  connection->server->config.max_single_event_bytes;
  }

  turbo_mutex_lock(&connection->event_mutex);
  while (!connection->closed && connection->stream_error == TURBO_OK &&
         (turbo_deque_size(&connection->events) >= event_limit ||
          connection->event_bytes >= byte_limit || bytes > byte_limit - connection->event_bytes)) {
    if (!wait_for_capacity) {
      rc = TURBO_EBUSY;
      break;
    }
    turbo_cond_wait(&connection->event_changed, &connection->event_mutex);
  }
  if (rc == TURBO_OK && connection->closed) rc = TURBO_ESHUTDOWN;
  if (rc == TURBO_OK && connection->stream_error != TURBO_OK) {
    rc = connection->stream_error;
  }
  if (rc == TURBO_OK) {
    if (connection->next_sequence >= TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER) {
      connection->stream_error = TURBO_ERANGE;
      rc = TURBO_ERANGE;
    } else {
      event.sequence = ++connection->next_sequence;
      event.bytes = bytes;
      event.message = message;
      if (turbo_deque_push_back(&connection->events, &event) != TURBO_OK) {
        connection->stream_error = TURBO_ENOMEM;
        rc = TURBO_ENOMEM;
      } else {
        connection->event_bytes += bytes;
        turbo_cond_broadcast(&connection->event_changed);
      }
    }
  }
  turbo_mutex_unlock(&connection->event_mutex);
  return rc;
}

static int turbo_agent_harness_connection_enqueue_pair(turbo_agent_harness_connection_t *connection,
                                                       json_value_t *first, json_value_t *second) {
  turbo_agent_harness_event_t pending[2] = {{0}};
  json_value_t *messages[2] = {first, second};
  size_t total_bytes = 0;
  size_t index;
  size_t pushed = 0;
  int rc = TURBO_OK;

  if (!connection || !first || !second) return TURBO_EINVAL;
  for (index = 0; index < 2; ++index) {
    char *serialized = turbo_json_serialize(messages[index], &pending[index].bytes);
    if (!serialized) return TURBO_ENOMEM;
    turbo_json_serialize_free(serialized);
    if (pending[index].bytes > connection->server->config.max_single_event_bytes ||
        pending[index].bytes > connection->server->config.max_event_bytes - total_bytes) {
      turbo_agent_harness_connection_set_stream_error(connection, TURBO_EMSGSIZE);
      return TURBO_EMSGSIZE;
    }
    total_bytes += pending[index].bytes;
    pending[index].message = messages[index];
  }

  turbo_mutex_lock(&connection->event_mutex);
  if (connection->closed) {
    rc = TURBO_ESHUTDOWN;
  } else if (connection->stream_error != TURBO_OK) {
    rc = connection->stream_error;
  } else if (turbo_deque_size(&connection->events) >
                 connection->server->config.max_event_count - 2 ||
             total_bytes > connection->server->config.max_event_bytes - connection->event_bytes) {
    rc = TURBO_EBUSY;
  } else if (connection->next_sequence > TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER - 2) {
    connection->stream_error = TURBO_ERANGE;
    rc = TURBO_ERANGE;
  }
  for (index = 0; rc == TURBO_OK && index < 2; ++index) {
    pending[index].sequence = ++connection->next_sequence;
    if (turbo_deque_push_back(&connection->events, &pending[index]) != TURBO_OK) {
      --connection->next_sequence;
      connection->stream_error = TURBO_ENOMEM;
      rc = TURBO_ENOMEM;
      break;
    }
    connection->event_bytes += pending[index].bytes;
    ++pushed;
  }
  while (rc != TURBO_OK && pushed > 0) {
    turbo_agent_harness_event_t rollback = {0};
    if (turbo_deque_pop_back(&connection->events, &rollback) != TURBO_OK) break;
    connection->event_bytes -= rollback.bytes;
    --connection->next_sequence;
    --pushed;
  }
  if (rc == TURBO_OK) turbo_cond_broadcast(&connection->event_changed);
  turbo_mutex_unlock(&connection->event_mutex);
  return rc;
}

static json_value_t *turbo_agent_harness_notification_create(const char *method,
                                                             json_value_t *params) {
  json_value_t *notification;
  if (!method || !params) return NULL;
  notification = turbo_json_create_object();
  if (!notification) return NULL;
  turbo_json_object_set_string(notification, "method", method);
  if (!turbo_json_object_add_checked(notification, "params", params)) {
    turbo_runtime_json_destroy(notification);
    return NULL;
  }
  return notification;
}

static int turbo_agent_harness_emit(turbo_agent_harness_connection_t *connection,
                                    const char *method, json_value_t *params,
                                    int wait_for_capacity) {
  json_value_t *message;
  int rc;
  if (!connection || !method || !params) {
    turbo_runtime_json_destroy(params);
    return TURBO_EINVAL;
  }
  message = turbo_agent_harness_notification_create(method, params);
  if (!message) {
    turbo_runtime_json_destroy(params);
    return TURBO_ENOMEM;
  }
  rc = turbo_agent_harness_connection_enqueue(connection, message, wait_for_capacity);
  if (rc != TURBO_OK) turbo_runtime_json_destroy(message);
  return rc;
}

static json_value_t *turbo_agent_harness_params_base(const turbo_agent_harness_turn_t *turn) {
  json_value_t *params = turbo_json_create_object();
  if (!params) return NULL;
  turbo_json_object_set_string(params, "threadId", turn->thread->id);
  turbo_json_object_set_string(params, "turnId", turn->id);
  return params;
}

static void turbo_agent_harness_turn_event_sink(const json_value_t *event, void *user_data) {
  turbo_agent_harness_turn_t *turn = (turbo_agent_harness_turn_t *)user_data;
  json_value_t *params;
  json_value_t *copy;
  int rc;
  if (!turn || !event || !turn->connection) return;
  params = turbo_agent_harness_params_base(turn);
  copy = turbo_json_clone(event);
  if (!params || !copy || !turbo_json_object_add_checked(params, "item", copy)) {
    turbo_runtime_json_destroy(copy);
    turbo_runtime_json_destroy(params);
    turbo_agent_harness_connection_set_stream_error(turn->connection, TURBO_ENOMEM);
    return;
  }
  rc = turbo_agent_harness_emit(turn->connection, "item/event", params, 1);
  if (rc != TURBO_OK && rc != TURBO_ESHUTDOWN) {
    turbo_agent_harness_connection_set_stream_error(turn->connection, rc);
  }
}

static void turbo_agent_harness_turn_destroy(turbo_agent_harness_turn_t *turn) {
  if (!turn) return;
  turbo_agent_harness_execution_release(turn->execution);
  turbo_runtime_json_destroy(turn->summary);
  turbo_runtime_json_destroy(turn->state);
  tstr_free(turn->approval_request_id);
  tstr_free(turn->id);
  turbo_agent_harness_connection_release_internal(turn->connection);
  free(turn);
}

static void turbo_agent_harness_thread_destroy(turbo_agent_harness_thread_t *thread) {
  turbo_agent_harness_execution_t *execution = NULL;
  if (!thread) return;
  turbo_mutex_lock(&thread->mutex);
  if (thread->turn && thread->turn->execution) {
    execution = turbo_agent_harness_execution_retain(thread->turn->execution);
  }
  turbo_mutex_unlock(&thread->mutex);
  if (execution) {
    (void)turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
    (void)turbo_agent_harness_execution_wait(execution, UINT64_MAX);
    turbo_agent_harness_execution_release(execution);
  }
  turbo_agent_harness_turn_destroy(thread->turn);
  turbo_agent_harness_release(thread->harness);
  tstr_free(thread->id);
  turbo_mutex_destroy(&thread->mutex);
  memset(thread, 0, sizeof(*thread));
}

static turbo_agent_harness_thread_t *
turbo_agent_harness_server_find_thread_locked(turbo_agent_harness_server_t *server,
                                              const char *thread_id) {
  size_t index;
  for (index = 0; index < turbo_vec_size(&server->threads); ++index) {
    turbo_agent_harness_thread_t *thread =
        (turbo_agent_harness_thread_t *)turbo_vec_at(&server->threads, index);
    if (thread && thread->id && strcmp(thread->id, thread_id) == 0) return thread;
  }
  return NULL;
}

static turbo_agent_harness_thread_t *
turbo_agent_harness_server_find_thread(turbo_agent_harness_server_t *server,
                                       const char *thread_id) {
  turbo_agent_harness_thread_t *thread;
  if (!server || !thread_id) return NULL;
  turbo_mutex_lock(&server->mutex);
  thread = turbo_agent_harness_server_find_thread_locked(server, thread_id);
  turbo_mutex_unlock(&server->mutex);
  return thread;
}

static int turbo_agent_harness_server_load_thread(turbo_agent_harness_server_t *server,
                                                  const char *thread_id,
                                                  turbo_agent_harness_thread_t **out_thread) {
  turbo_agent_harness_thread_t candidate = {0};
  turbo_agent_harness_thread_t *existing;
  const char *bound_thread_id;
  int rc;

  if (!server || !thread_id || !thread_id[0] || !out_thread) return TURBO_EINVAL;
  *out_thread = NULL;
  existing = turbo_agent_harness_server_find_thread(server, thread_id);
  if (existing) {
    *out_thread = existing;
    return TURBO_OK;
  }

  candidate.id = tstr_dup(thread_id);
  if (!candidate.id) return TURBO_ENOMEM;
  turbo_mutex_init(&candidate.mutex);
  if (!candidate.mutex) {
    tstr_free(candidate.id);
    return TURBO_ENOMEM;
  }
  rc = server->config.thread_factory(thread_id, &candidate.harness,
                                     server->config.thread_factory_user_data);
  if (rc != TURBO_OK || !candidate.harness) {
    turbo_agent_harness_thread_destroy(&candidate);
    return rc != TURBO_OK ? rc : TURBO_EIO;
  }
  bound_thread_id = turbo_agent_app_thread_id(turbo_agent_harness_app(candidate.harness));
  if (!bound_thread_id || strcmp(bound_thread_id, thread_id) != 0) {
    turbo_agent_harness_thread_destroy(&candidate);
    return TURBO_EPROTO;
  }
  rc = turbo_agent_session_inbox_configure(
      turbo_agent_app_session(turbo_agent_harness_app(candidate.harness)), &server->config.inbox);
  if (rc != TURBO_OK && rc != TURBO_EALREADY) {
    turbo_agent_harness_thread_destroy(&candidate);
    return rc;
  }

  turbo_mutex_lock(&server->mutex);
  existing = turbo_agent_harness_server_find_thread_locked(server, thread_id);
  if (existing) {
    turbo_mutex_unlock(&server->mutex);
    turbo_agent_harness_thread_destroy(&candidate);
    *out_thread = existing;
    return TURBO_OK;
  }
  if (turbo_vec_size(&server->threads) >= server->config.max_threads) {
    turbo_mutex_unlock(&server->mutex);
    turbo_agent_harness_thread_destroy(&candidate);
    return TURBO_EBUSY;
  }
  rc = turbo_vec_push(&server->threads, &candidate);
  if (rc == TURBO_OK) {
    *out_thread = (turbo_agent_harness_thread_t *)turbo_vec_at(
        &server->threads, turbo_vec_size(&server->threads) - 1);
    memset(&candidate, 0, sizeof(candidate));
  }
  turbo_mutex_unlock(&server->mutex);
  turbo_agent_harness_thread_destroy(&candidate);
  return rc == TURBO_OK ? TURBO_OK : TURBO_ENOMEM;
}

static const char *turbo_agent_harness_turn_status_text(const turbo_agent_harness_turn_t *turn) {
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_QUEUED;
  if (!turn) return "notFound";
  if (turn->finalized) {
    const char *summary_status =
        turn->summary ? turbo_json_get_string(turn->summary, "status") : NULL;
    if (turn->interrupt_requested) return "interrupted";
    if (summary_status && strcmp(summary_status, "completed") == 0) return "completed";
    if (summary_status && strcmp(summary_status, "cancelled") == 0) return "interrupted";
    if (summary_status && strcmp(summary_status, "timed_out") == 0) return "failed";
    if (summary_status && strcmp(summary_status, "declined") == 0) return "declined";
    return "failed";
  }
  if (turn->awaiting_approval) return "approvalPending";
  if (turn->starting || !turn->execution) return "inProgress";
  if (turbo_agent_harness_execution_get_status(turn->execution, &status) != TURBO_OK) {
    return "failed";
  }
  return status >= TURBO_AGENT_EXECUTION_COMPLETED ? "completing" : "inProgress";
}

static int turbo_agent_harness_turn_snapshot_locked(const turbo_agent_harness_turn_t *turn,
                                                    turbo_agent_harness_turn_snapshot_t *snapshot) {
  size_t id_size;
  size_t approval_size = 0;
  if (!turn || !snapshot) return TURBO_EINVAL;
  id_size = strlen(turn->id);
  if (turn->approval_request_id) approval_size = strlen(turn->approval_request_id);
  if (id_size >= sizeof(snapshot->id) || approval_size >= sizeof(snapshot->approval_request_id)) {
    return TURBO_ERANGE;
  }
  memset(snapshot, 0, sizeof(*snapshot));
  memcpy(snapshot->id, turn->id, id_size + 1);
  if (turn->approval_request_id) {
    memcpy(snapshot->approval_request_id, turn->approval_request_id, approval_size + 1);
  }
  snapshot->thread_id = turn->thread->id;
  snapshot->status = turbo_agent_harness_turn_status_text(turn);
  return TURBO_OK;
}

static json_value_t *
turbo_agent_harness_turn_snapshot_json(const turbo_agent_harness_turn_snapshot_t *snapshot) {
  json_value_t *turn_json;
  json_value_t *items;
  if (!snapshot) return NULL;
  turn_json = turbo_json_create_object();
  if (!turn_json) return NULL;
  turbo_json_object_set_string(turn_json, "id", snapshot->id);
  turbo_json_object_set_string(turn_json, "threadId", snapshot->thread_id);
  turbo_json_object_set_string(turn_json, "status", snapshot->status);
  items = turbo_json_create_array();
  if (!items || !turbo_json_object_add_checked(turn_json, "items", items)) {
    turbo_runtime_json_destroy(items);
    turbo_runtime_json_destroy(turn_json);
    return NULL;
  }
  if (snapshot->approval_request_id[0]) {
    turbo_json_object_set_string(turn_json, "approvalRequestId", snapshot->approval_request_id);
  } else {
    turbo_json_object_set_null(turn_json, "approvalRequestId");
  }
  return turn_json;
}

static json_value_t *turbo_agent_harness_turn_json(turbo_agent_harness_turn_t *turn) {
  turbo_agent_harness_turn_snapshot_t snapshot;
  int rc;
  turbo_mutex_lock(&turn->thread->mutex);
  rc = turbo_agent_harness_turn_snapshot_locked(turn, &snapshot);
  turbo_mutex_unlock(&turn->thread->mutex);
  return rc == TURBO_OK ? turbo_agent_harness_turn_snapshot_json(&snapshot) : NULL;
}

static json_value_t *turbo_agent_harness_thread_json(turbo_agent_harness_thread_t *thread) {
  char active_turn_id[TURBO_UUID_STRING_SIZE] = {0};
  const char *status = "idle";
  json_value_t *thread_json;
  turbo_mutex_lock(&thread->mutex);
  if (thread->turn) {
    status = turbo_agent_harness_turn_status_text(thread->turn);
    if (strlen(thread->turn->id) >= sizeof(active_turn_id)) {
      turbo_mutex_unlock(&thread->mutex);
      return NULL;
    }
    memcpy(active_turn_id, thread->turn->id, strlen(thread->turn->id) + 1);
  }
  turbo_mutex_unlock(&thread->mutex);
  thread_json = turbo_json_create_object();
  if (!thread_json) return NULL;
  turbo_json_object_set_string(thread_json, "id", thread->id);
  turbo_json_object_set_string(thread_json, "sessionId", thread->id);
  turbo_json_object_set_bool(thread_json, "loaded", 1);
  turbo_json_object_set_string(thread_json, "status", status);
  if (active_turn_id[0]) {
    turbo_json_object_set_string(thread_json, "activeTurnId", active_turn_id);
  } else {
    turbo_json_object_set_null(thread_json, "activeTurnId");
  }
  return thread_json;
}

static json_value_t *turbo_agent_harness_turn_completed_message(turbo_agent_harness_turn_t *turn,
                                                                const char *status) {
  json_value_t *params = turbo_agent_harness_params_base(turn);
  json_value_t *turn_json;
  if (!params) return NULL;
  turn_json = turbo_agent_harness_turn_json(turn);
  if (!turn_json) {
    turbo_runtime_json_destroy(params);
    return NULL;
  }
  turbo_json_object_set_string(turn_json, "status", status);
  if (!turbo_json_object_add_checked(params, "turn", turn_json)) {
    turbo_runtime_json_destroy(turn_json);
    turbo_runtime_json_destroy(params);
    return NULL;
  }
  return turbo_agent_harness_notification_create("turn/completed", params);
}

static int turbo_agent_harness_turn_emit_completed(turbo_agent_harness_turn_t *turn,
                                                   const char *status) {
  json_value_t *message = turbo_agent_harness_turn_completed_message(turn, status);
  int rc;
  if (!message) return TURBO_ENOMEM;
  rc = turbo_agent_harness_connection_enqueue(turn->connection, message, 0);
  if (rc != TURBO_OK) turbo_runtime_json_destroy(message);
  return rc;
}

static int turbo_agent_harness_turn_emit_resolution_and_completed(turbo_agent_harness_turn_t *turn,
                                                                  const char *request_id,
                                                                  const char *decision,
                                                                  const char *status) {
  json_value_t *resolved = turbo_agent_harness_params_base(turn);
  json_value_t *resolved_message = NULL;
  json_value_t *completed_message = NULL;
  int rc;
  if (resolved) {
    turbo_json_object_set_string(resolved, "requestId", request_id);
    turbo_json_object_set_string(resolved, "decision", decision);
    resolved_message = turbo_agent_harness_notification_create("serverRequest/resolved", resolved);
    if (!resolved_message) turbo_runtime_json_destroy(resolved);
  }
  completed_message = turbo_agent_harness_turn_completed_message(turn, status);
  if (!resolved_message || !completed_message) {
    turbo_runtime_json_destroy(resolved_message);
    turbo_runtime_json_destroy(completed_message);
    return TURBO_ENOMEM;
  }
  rc = turbo_agent_harness_connection_enqueue_pair(turn->connection, resolved_message,
                                                   completed_message);
  if (rc != TURBO_OK) {
    turbo_runtime_json_destroy(resolved_message);
    turbo_runtime_json_destroy(completed_message);
  }
  return rc;
}

static int turbo_agent_harness_turn_emit_approval(turbo_agent_harness_turn_t *turn,
                                                  tstr_t *out_request_id) {
  json_value_t *params = NULL;
  json_value_t *decisions = NULL;
  json_value_t *note_json = NULL;
  const char *note = turbo_agent_state_review_note(turn->state);
  const char *method = "item/review/requestApproval";
  const char *item_id = turn->id;
  char request_id[TURBO_UUID_STRING_SIZE];
  tstr_t owned_request_id = NULL;
  int rc;

  if (!out_request_id) return TURBO_EINVAL;
  *out_request_id = NULL;
  rc = turbo_agent_harness_server_uuid(request_id);
  if (rc != TURBO_OK) return rc;
  owned_request_id = tstr_dup(request_id);
  if (!owned_request_id) return TURBO_ENOMEM;
  if (note && turbo_parse_json((const uint8_t *)note, strlen(note), &note_json) == 0 && note_json &&
      turbo_json_type(note_json) == TURBO_JSON_OBJECT &&
      strcmp(turbo_json_get_string(note_json, "kind") ? turbo_json_get_string(note_json, "kind")
                                                      : "",
             "tool_approval") == 0) {
    const char *call_id = turbo_json_get_string(note_json, "call_id");
    const char *tool_name = turbo_json_get_string(note_json, "tool_name");
    const char *arguments = turbo_json_get_string(note_json, "arguments");
    method = "item/tool/requestApproval";
    if (call_id && call_id[0]) item_id = call_id;
    params = turbo_agent_harness_params_base(turn);
    if (params) {
      turbo_json_object_set_string(params, "itemId", item_id);
      if (tool_name) turbo_json_object_set_string(params, "toolName", tool_name);
      if (arguments) turbo_json_object_set_string(params, "arguments", arguments);
    }
  } else {
    params = turbo_agent_harness_params_base(turn);
    if (params) turbo_json_object_set_string(params, "itemId", item_id);
  }
  turbo_runtime_json_destroy(note_json);
  if (!params) {
    tstr_free(owned_request_id);
    return TURBO_ENOMEM;
  }
  turbo_json_object_set_string(params, "requestId", request_id);
  if (note) turbo_json_object_set_string(params, "reason", note);
  decisions = turbo_json_create_array();
  if (!decisions) {
    turbo_runtime_json_destroy(params);
    tstr_free(owned_request_id);
    return TURBO_ENOMEM;
  }
  turbo_json_array_add(decisions, turbo_json_create_string("approve_once"));
  turbo_json_array_add(decisions, turbo_json_create_string("deny"));
  turbo_json_object_add(params, "availableDecisions", decisions);
  rc = turbo_agent_harness_emit(turn->connection, method, params, 0);
  if (rc == TURBO_OK) {
    *out_request_id = owned_request_id;
  } else {
    tstr_free(owned_request_id);
  }
  return rc;
}

static int turbo_agent_harness_turn_refresh(turbo_agent_harness_thread_t *thread) {
  turbo_agent_harness_turn_t *turn;
  turbo_agent_harness_execution_t *execution = NULL;
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_QUEUED;
  json_value_t *summary = NULL;
  json_value_t *state = NULL;
  const char *summary_status;
  const char *pending_action;
  const char *protocol_status = NULL;
  tstr_t approval_request_id = NULL;
  int approval_pending = 0;
  int rc;
  int has_result = 0;

  turbo_mutex_lock(&thread->mutex);
  turn = thread->turn;
  if (!turn || turn->starting || turn->refreshing || turn->finalized || turn->awaiting_approval ||
      !turn->execution) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_OK;
  }
  has_result = turn->summary && turn->state;
  if (!has_result) {
    execution = turbo_agent_harness_execution_retain(turn->execution);
    if (!execution) {
      turbo_mutex_unlock(&thread->mutex);
      return TURBO_ERANGE;
    }
  }
  turn->refreshing = 1;
  turbo_mutex_unlock(&thread->mutex);

  if (has_result) {
    rc = TURBO_OK;
    status = TURBO_AGENT_EXECUTION_COMPLETED;
  } else {
    rc = turbo_agent_harness_execution_get_status(execution, &status);
    if (rc == TURBO_OK && status >= TURBO_AGENT_EXECUTION_COMPLETED) {
      rc = turbo_agent_harness_execution_take_result(execution, &summary, &state);
    }
    turbo_agent_harness_execution_release(execution);
  }

  turbo_mutex_lock(&thread->mutex);
  if (rc != TURBO_OK || status < TURBO_AGENT_EXECUTION_COMPLETED) {
    turn->refreshing = 0;
    turbo_mutex_unlock(&thread->mutex);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(state);
    return rc == TURBO_EBUSY ? TURBO_OK : rc;
  }
  if (!has_result) {
    turn->summary = summary;
    turn->state = state;
  }
  summary_status = turbo_json_get_string(turn->summary, "status");
  pending_action = turbo_json_get_string(turn->summary, "pending_action");
  if (summary_status && strcmp(summary_status, "interrupted") == 0 && pending_action &&
      strcmp(pending_action, "review") == 0 && !turn->interrupt_requested) {
    approval_pending = 1;
  } else {
    protocol_status =
        turn->interrupt_requested || (summary_status && strcmp(summary_status, "cancelled") == 0)
            ? "interrupted"
            : (summary_status ? summary_status : "failed");
  }
  turbo_mutex_unlock(&thread->mutex);

  rc = approval_pending ? turbo_agent_harness_turn_emit_approval(turn, &approval_request_id)
                        : turbo_agent_harness_turn_emit_completed(turn, protocol_status);
  turbo_mutex_lock(&thread->mutex);
  if (rc == TURBO_OK && approval_pending) {
    turn->approval_request_id = approval_request_id;
    turn->awaiting_approval = 1;
  } else if (rc == TURBO_OK) {
    turn->finalized = 1;
  } else {
    tstr_free(approval_request_id);
  }
  turn->refreshing = 0;
  turbo_mutex_unlock(&thread->mutex);
  if (rc != TURBO_OK && rc != TURBO_ESHUTDOWN && rc != TURBO_EBUSY) {
    turbo_agent_harness_connection_set_stream_error(turn->connection, rc);
  }
  return rc == TURBO_EBUSY ? TURBO_OK : rc;
}

static void turbo_agent_harness_connection_refresh(turbo_agent_harness_connection_t *connection) {
  turbo_agent_harness_server_t *server = connection->server;
  turbo_agent_harness_thread_t **threads;
  size_t count = 0;
  size_t total = 0;
  size_t index;
  threads = (turbo_agent_harness_thread_t **)calloc(server->config.max_threads, sizeof(*threads));
  if (!threads) {
    turbo_agent_harness_connection_set_stream_error(connection, TURBO_ENOMEM);
    return;
  }
  turbo_mutex_lock(&server->mutex);
  total = turbo_vec_size(&server->threads);
  for (index = 0; index < total; ++index) {
    threads[index] = (turbo_agent_harness_thread_t *)turbo_vec_at(&server->threads, index);
  }
  turbo_mutex_unlock(&server->mutex);
  for (index = 0; index < total; ++index) {
    turbo_agent_harness_thread_t *thread = threads[index];
    turbo_mutex_lock(&thread->mutex);
    if (thread->turn && thread->turn->connection == connection) threads[count++] = thread;
    turbo_mutex_unlock(&thread->mutex);
  }
  for (index = 0; index < count; ++index) {
    (void)turbo_agent_harness_turn_refresh(threads[index]);
  }
  free(threads);
}

static json_value_t *turbo_agent_harness_response_base(const json_value_t *request) {
  json_value_t *response = turbo_json_create_object();
  const json_value_t *id = request ? turbo_json_object_get(request, "id") : NULL;
  if (!response) return NULL;
  if (id) {
    json_value_t *copy = turbo_json_clone(id);
    if (!copy || !turbo_json_object_add_checked(response, "id", copy)) {
      turbo_runtime_json_destroy(copy);
      turbo_runtime_json_destroy(response);
      return NULL;
    }
  } else {
    turbo_json_object_set_null(response, "id");
  }
  return response;
}

static int turbo_agent_harness_response_success(const json_value_t *request, json_value_t *result,
                                                json_value_t **out_response) {
  json_value_t *response;
  if (!result || !out_response) {
    turbo_runtime_json_destroy(result);
    return TURBO_EINVAL;
  }
  response = turbo_agent_harness_response_base(request);
  if (!response || !turbo_json_object_add_checked(response, "result", result)) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(response);
    return TURBO_ENOMEM;
  }
  *out_response = response;
  return TURBO_OK;
}

static const char *turbo_agent_harness_rpc_message(int code) {
  switch (code) {
  case TURBO_AGENT_HARNESS_RPC_INVALID_REQUEST:
    return "Invalid request";
  case TURBO_AGENT_HARNESS_RPC_METHOD_NOT_FOUND:
    return "Method not found";
  case TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS:
    return "Invalid params";
  case TURBO_AGENT_HARNESS_RPC_NOT_INITIALIZED:
    return "Not initialized";
  case TURBO_AGENT_HARNESS_RPC_ALREADY_INITIALIZED:
    return "Already initialized";
  case TURBO_AGENT_HARNESS_RPC_BUSY:
    return "Resource busy";
  case TURBO_AGENT_HARNESS_RPC_NOT_FOUND:
    return "Resource not found";
  case TURBO_AGENT_HARNESS_RPC_STREAM_FAILED:
    return "Event stream failed";
  default:
    return "Internal error";
  }
}

static int turbo_agent_harness_response_error(const json_value_t *request, int code,
                                              json_value_t **out_response) {
  json_value_t *response = turbo_agent_harness_response_base(request);
  json_value_t *error = turbo_json_create_object();
  if (!response || !error) {
    turbo_runtime_json_destroy(response);
    turbo_runtime_json_destroy(error);
    return TURBO_ENOMEM;
  }
  turbo_json_object_set_number(error, "code", (double)code);
  turbo_json_object_set_string(error, "message", turbo_agent_harness_rpc_message(code));
  turbo_json_object_add(response, "error", error);
  *out_response = response;
  return TURBO_OK;
}

static int turbo_agent_harness_rpc_from_error(int rc) {
  if (rc == TURBO_EINVAL || rc == TURBO_EPROTO || rc == TURBO_EMSGSIZE) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  if (rc == TURBO_EBUSY || rc == TURBO_EALREADY) return TURBO_AGENT_HARNESS_RPC_BUSY;
  if (rc == TURBO_ENOENT) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  if (rc == TURBO_ESHUTDOWN) return TURBO_AGENT_HARNESS_RPC_STREAM_FAILED;
  return TURBO_AGENT_HARNESS_RPC_INTERNAL;
}

static int turbo_agent_harness_json_uint64(const json_value_t *value, uint64_t minimum,
                                           uint64_t maximum, uint64_t *out_number) {
  double number;
  uint64_t converted;
  if (!value || turbo_json_type(value) != TURBO_JSON_NUMBER || !out_number) {
    return TURBO_EINVAL;
  }
  number = turbo_json_number(value);
  if (number < (double)minimum || number > (double)maximum) return TURBO_ERANGE;
  converted = (uint64_t)number;
  if ((double)converted != number) return TURBO_EINVAL;
  *out_number = converted;
  return TURBO_OK;
}

static int turbo_agent_harness_input_text(const json_value_t *params, tstr_t *out_text) {
  const json_value_t *input;
  tstr_t text;
  size_t count;
  size_t index;
  size_t total_size = 0;
  size_t offset = 0;
  if (!params || !out_text) return TURBO_EINVAL;
  *out_text = NULL;
  input = turbo_json_object_get(params, "input");
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY ||
      (count = turbo_json_array_size(input)) == 0) {
    return TURBO_EINVAL;
  }
  for (index = 0; index < count; ++index) {
    const json_value_t *item = turbo_json_array_get(input, index);
    const json_value_t *text_json;
    const char *type;
    const char *value;
    size_t value_size;
    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT ||
        !(type = turbo_json_get_string(item, "type")) || strcmp(type, "text") != 0 ||
        !(text_json = turbo_json_object_get(item, "text")) ||
        turbo_json_type(text_json) != TURBO_JSON_STRING ||
        !(value = turbo_json_string(text_json)) ||
        (value_size = turbo_json_string_len(text_json)) == 0 || strlen(value) != value_size) {
      return TURBO_ENOTSUP;
    }
    if ((index > 0 && total_size == SIZE_MAX) ||
        value_size > SIZE_MAX - total_size - (index > 0 ? 1 : 0)) {
      return TURBO_ERANGE;
    }
    total_size += value_size + (index > 0 ? 1 : 0);
  }
  text = tstr_new_len(NULL, total_size);
  if (!text) return TURBO_ENOMEM;
  for (index = 0; index < count; ++index) {
    const json_value_t *item = turbo_json_array_get(input, index);
    const json_value_t *text_json = turbo_json_object_get(item, "text");
    const char *value = turbo_json_string(text_json);
    size_t value_size = turbo_json_string_len(text_json);
    if (index > 0) text[offset++] = '\n';
    memcpy(text + offset, value, value_size);
    offset += value_size;
  }
  *out_text = text;
  return TURBO_OK;
}

static int turbo_agent_harness_interrupt_nodes(const json_value_t *params, const char ***out_nodes,
                                               size_t *out_count) {
  const json_value_t *array = params ? turbo_json_object_get(params, "interruptBeforeNodes") : NULL;
  const char **nodes = NULL;
  size_t count;
  size_t index;
  if (!out_nodes || !out_count) return TURBO_EINVAL;
  *out_nodes = NULL;
  *out_count = 0;
  if (!array) return TURBO_OK;
  if (turbo_json_type(array) != TURBO_JSON_ARRAY ||
      (count = turbo_json_array_size(array)) > TURBO_AGENT_HARNESS_MAX_INTERRUPT_NODES) {
    return TURBO_EINVAL;
  }
  if (count == 0) return TURBO_OK;
  nodes = (const char **)calloc(count, sizeof(*nodes));
  if (!nodes) return TURBO_ENOMEM;
  for (index = 0; index < count; ++index) {
    nodes[index] = turbo_json_string(turbo_json_array_get(array, index));
    if (!nodes[index] || !nodes[index][0]) {
      free(nodes);
      return TURBO_EINVAL;
    }
  }
  *out_nodes = nodes;
  *out_count = count;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_initialize(turbo_agent_harness_connection_t *connection,
                                                   const json_value_t *params,
                                                   json_value_t **out_result) {
  const json_value_t *client_info;
  const json_value_t *capabilities;
  const char *name;
  const char *version;
  json_value_t *result;
  json_value_t *server_info;
  json_value_t *server_capabilities;
  if (connection->state != TURBO_AGENT_HARNESS_CONNECTION_NEW) {
    return TURBO_AGENT_HARNESS_RPC_ALREADY_INITIALIZED;
  }
  client_info = params ? turbo_json_object_get(params, "clientInfo") : NULL;
  if (!client_info || turbo_json_type(client_info) != TURBO_JSON_OBJECT ||
      !(name = turbo_json_get_string(client_info, "name")) || !name[0] ||
      !(version = turbo_json_get_string(client_info, "version")) || !version[0]) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  capabilities = turbo_json_object_get(params, "capabilities");
  if (capabilities && turbo_json_type(capabilities) != TURBO_JSON_OBJECT) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  connection->experimental_api =
      capabilities ? (turbo_json_get_bool(capabilities, "experimentalApi", false) ? 1 : 0) : 0;
  result = turbo_json_create_object();
  server_info = turbo_json_create_object();
  server_capabilities = turbo_json_create_object();
  if (!result || !server_info || !server_capabilities) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(server_info);
    turbo_runtime_json_destroy(server_capabilities);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_set_string(server_info, "name", "turbo_agent_harness_server");
  turbo_json_object_set_string(server_info, "version", "1.0");
  turbo_json_object_set_number(server_info, "protocolVersion", 1.0);
  turbo_json_object_set_bool(server_capabilities, "supportsThreadStart", 1);
  turbo_json_object_set_bool(server_capabilities, "supportsThreadResume", 1);
  turbo_json_object_set_bool(server_capabilities, "supportsThreadFork", 0);
  turbo_json_object_set_bool(server_capabilities, "supportsTurnSteer", 1);
  turbo_json_object_set_bool(server_capabilities, "supportsTurnInterrupt", 1);
  turbo_json_object_set_bool(server_capabilities, "supportsApproval", 1);
  turbo_json_object_set_bool(server_capabilities, "supportsEventReplay", 1);
  turbo_json_object_set_number(server_capabilities, "maxLoadedThreads",
                               (double)connection->server->config.max_threads);
  turbo_json_object_set_number(server_capabilities, "maxEventCount",
                               (double)connection->server->config.max_event_count);
  turbo_json_object_add(result, "serverInfo", server_info);
  turbo_json_object_add(result, "capabilities", server_capabilities);
  connection->state = TURBO_AGENT_HARNESS_CONNECTION_INITIALIZED;
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_thread_load(turbo_agent_harness_connection_t *connection,
                                                    const json_value_t *params, int create_new,
                                                    json_value_t **out_result) {
  char generated_id[TURBO_UUID_STRING_SIZE];
  const char *thread_id;
  turbo_agent_harness_thread_t *thread;
  json_value_t *thread_json;
  json_value_t *result;
  json_value_t *notify_thread;
  json_value_t *notify_params;
  int rc;
  if (create_new) {
    if (params && turbo_json_object_size(params) != 0) {
      return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
    }
    rc = turbo_agent_harness_server_uuid(generated_id);
    if (rc != TURBO_OK) return TURBO_AGENT_HARNESS_RPC_INTERNAL;
    thread_id = generated_id;
  } else {
    thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
    if (!thread_id || !thread_id[0]) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  rc = turbo_agent_harness_server_load_thread(connection->server, thread_id, &thread);
  if (rc != TURBO_OK) return turbo_agent_harness_rpc_from_error(rc);
  thread_json = turbo_agent_harness_thread_json(thread);
  result = turbo_json_create_object();
  notify_params = turbo_json_create_object();
  notify_thread = turbo_agent_harness_thread_json(thread);
  if (!thread_json || !result || !notify_params || !notify_thread) {
    turbo_runtime_json_destroy(thread_json);
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(notify_params);
    turbo_runtime_json_destroy(notify_thread);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_add(result, "thread", thread_json);
  turbo_json_object_add(notify_params, "thread", notify_thread);
  rc = turbo_agent_harness_emit(connection, "thread/started", notify_params, 0);
  if (rc != TURBO_OK) {
    turbo_runtime_json_destroy(result);
    return TURBO_AGENT_HARNESS_RPC_STREAM_FAILED;
  }
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_thread_get(turbo_agent_harness_connection_t *connection,
                                                   const json_value_t *params,
                                                   json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  turbo_agent_harness_thread_t *thread;
  json_value_t *result;
  json_value_t *thread_json;
  if (!thread_id || !thread_id[0]) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  (void)turbo_agent_harness_turn_refresh(thread);
  result = turbo_json_create_object();
  thread_json = turbo_agent_harness_thread_json(thread);
  if (!result || !thread_json) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(thread_json);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_add(result, "thread", thread_json);
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_thread_list(turbo_agent_harness_connection_t *connection,
                                                    const json_value_t *params,
                                                    json_value_t **out_result) {
  json_value_t *result;
  json_value_t *threads;
  turbo_agent_harness_thread_t **snapshot;
  size_t count;
  size_t index;
  if (params && turbo_json_object_size(params) != 0) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  result = turbo_json_create_object();
  threads = turbo_json_create_array();
  if (!result || !threads) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(threads);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  snapshot = (turbo_agent_harness_thread_t **)calloc(connection->server->config.max_threads,
                                                     sizeof(*snapshot));
  if (!snapshot) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(threads);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_mutex_lock(&connection->server->mutex);
  count = turbo_vec_size(&connection->server->threads);
  for (index = 0; index < count; ++index) {
    snapshot[index] =
        (turbo_agent_harness_thread_t *)turbo_vec_at(&connection->server->threads, index);
  }
  turbo_mutex_unlock(&connection->server->mutex);
  for (index = 0; index < count; ++index) {
    json_value_t *thread_json = turbo_agent_harness_thread_json(snapshot[index]);
    if (!thread_json || !turbo_json_array_add_checked(threads, thread_json)) {
      turbo_runtime_json_destroy(thread_json);
      free(snapshot);
      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(threads);
      return TURBO_AGENT_HARNESS_RPC_INTERNAL;
    }
  }
  free(snapshot);
  turbo_json_object_add(result, "threads", threads);
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_turn_start(turbo_agent_harness_connection_t *connection,
                                                   const json_value_t *params,
                                                   json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  const json_value_t *deadline_json;
  const char **interrupt_nodes = NULL;
  size_t interrupt_count = 0;
  uint64_t deadline_mono_ms = 0;
  turbo_agent_harness_thread_t *thread;
  turbo_agent_harness_turn_t *turn = NULL;
  turbo_agent_harness_turn_t *previous = NULL;
  turbo_agent_harness_run_options_t options;
  turbo_graph_run_options_t graph_options = {0};
  tstr_t text = NULL;
  json_value_t *notify_params = NULL;
  json_value_t *result = NULL;
  json_value_t *turn_json = NULL;
  char turn_id[TURBO_UUID_STRING_SIZE];
  int rc;

  if (!thread_id || !thread_id[0]) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  rc = turbo_agent_harness_input_text(params, &text);
  if (rc != TURBO_OK) {
    return rc == TURBO_ENOMEM ? TURBO_AGENT_HARNESS_RPC_INTERNAL
                              : TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  deadline_json = turbo_json_object_get(params, "deadlineMonoMs");
  if (deadline_json) {
    if (turbo_agent_harness_json_uint64(deadline_json, 1,
                                        TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER,
                                        &deadline_mono_ms) != TURBO_OK) {
      tstr_free(text);
      return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
    }
  }
  rc = turbo_agent_harness_interrupt_nodes(params, &interrupt_nodes, &interrupt_count);
  if (rc != TURBO_OK) {
    tstr_free(text);
    return rc == TURBO_ENOMEM ? TURBO_AGENT_HARNESS_RPC_INTERNAL
                              : TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  rc = turbo_agent_harness_server_uuid(turn_id);
  if (rc != TURBO_OK) {
    free(interrupt_nodes);
    tstr_free(text);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turn = (turbo_agent_harness_turn_t *)calloc(1, sizeof(*turn));
  if (!turn) {
    free(interrupt_nodes);
    tstr_free(text);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turn->id = tstr_dup(turn_id);
  turn->thread = thread;
  turn->connection = turbo_agent_harness_connection_retain_internal(connection);
  turn->starting = 1;
  if (!turn->id || !turn->connection) {
    free(interrupt_nodes);
    tstr_free(text);
    turbo_agent_harness_turn_destroy(turn);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }

  turbo_mutex_lock(&thread->mutex);
  if (thread->turn &&
      (!thread->turn->finalized || thread->turn->starting || thread->turn->refreshing)) {
    turbo_mutex_unlock(&thread->mutex);
    free(interrupt_nodes);
    tstr_free(text);
    turbo_agent_harness_turn_destroy(turn);
    return TURBO_AGENT_HARNESS_RPC_BUSY;
  }
  previous = thread->turn;
  thread->turn = turn;
  turbo_mutex_unlock(&thread->mutex);
  turbo_agent_harness_turn_destroy(previous);

  notify_params = turbo_agent_harness_params_base(turn);
  if (!notify_params) {
    rc = TURBO_ENOMEM;
    goto fail;
  }
  turn_json = turbo_agent_harness_turn_json(turn);
  if (!turn_json) {
    turbo_runtime_json_destroy(notify_params);
    rc = TURBO_ENOMEM;
    goto fail;
  }
  turbo_json_object_add(notify_params, "turn", turn_json);
  rc = turbo_agent_harness_emit(connection, "turn/started", notify_params, 0);
  if (rc != TURBO_OK) goto fail;

  turbo_agent_harness_run_options_init(&options);
  graph_options.interrupt_before_nodes = interrupt_nodes;
  graph_options.interrupt_before_count = interrupt_count;
  options.graph_options = interrupt_count ? &graph_options : NULL;
  options.event_sink = turbo_agent_harness_turn_event_sink;
  options.event_sink_user_data = turn;
  options.deadline_mono_ms = deadline_mono_ms;
  rc = turbo_agent_harness_start_text(thread->harness, text, &options, &turn->execution);
  free(interrupt_nodes);
  interrupt_nodes = NULL;
  if (rc != TURBO_OK) goto fail;
  turbo_mutex_lock(&thread->mutex);
  turn->starting = 0;
  turbo_mutex_unlock(&thread->mutex);
  result = turbo_json_create_object();
  if (!result) {
    rc = TURBO_ENOMEM;
    goto fail_after_submit;
  }
  turn_json = turbo_agent_harness_turn_json(turn);
  if (!turn_json) {
    turbo_runtime_json_destroy(result);
    rc = TURBO_ENOMEM;
    goto fail_after_submit;
  }
  turbo_json_object_add(result, "turn", turn_json);
  *out_result = result;
  tstr_free(text);
  return TURBO_OK;

fail_after_submit:
  (void)turbo_agent_harness_execution_cancel(turn->execution, TURBO_CANCEL_SHUTDOWN);
  (void)turbo_agent_harness_execution_wait(turn->execution, UINT64_MAX);
fail: {
  int operation_rc = rc;
  int emit_rc;
  free(interrupt_nodes);
  turbo_mutex_lock(&thread->mutex);
  turn->starting = 1;
  turbo_mutex_unlock(&thread->mutex);
  emit_rc = turbo_agent_harness_turn_emit_completed(turn, "failed");
  turbo_mutex_lock(&thread->mutex);
  turn->starting = 0;
  turn->finalized = 1;
  turbo_mutex_unlock(&thread->mutex);
  if (emit_rc != TURBO_OK && emit_rc != TURBO_ESHUTDOWN) {
    turbo_agent_harness_connection_set_stream_error(connection, emit_rc);
  }
  tstr_free(text);
  return turbo_agent_harness_rpc_from_error(operation_rc);
}
}

static int turbo_agent_harness_dispatch_turn_get(turbo_agent_harness_connection_t *connection,
                                                 const json_value_t *params,
                                                 json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  const char *turn_id = params ? turbo_json_get_string(params, "turnId") : NULL;
  turbo_agent_harness_thread_t *thread;
  json_value_t *result;
  json_value_t *turn_json;
  turbo_agent_harness_turn_snapshot_t snapshot;
  if (!thread_id || !turn_id) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  (void)turbo_agent_harness_turn_refresh(thread);
  turbo_mutex_lock(&thread->mutex);
  if (!thread->turn || strcmp(thread->turn->id, turn_id) != 0) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  }
  if (turbo_agent_harness_turn_snapshot_locked(thread->turn, &snapshot) != TURBO_OK) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_mutex_unlock(&thread->mutex);
  turn_json = turbo_agent_harness_turn_snapshot_json(&snapshot);
  result = turbo_json_create_object();
  if (!result || !turn_json) {
    turbo_runtime_json_destroy(result);
    turbo_runtime_json_destroy(turn_json);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_add(result, "turn", turn_json);
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_turn_interrupt(turbo_agent_harness_connection_t *connection,
                                                       const json_value_t *params,
                                                       json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  const char *turn_id = params ? turbo_json_get_string(params, "turnId") : NULL;
  turbo_agent_harness_thread_t *thread;
  turbo_agent_harness_turn_t *turn;
  turbo_agent_harness_execution_t *execution;
  tstr_t approval_request_id = NULL;
  int rc;
  if (!thread_id || !turn_id) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  (void)turbo_agent_harness_turn_refresh(thread);
  turbo_mutex_lock(&thread->mutex);
  turn = thread->turn;
  if (!turn || strcmp(turn->id, turn_id) != 0 || turn->connection != connection ||
      turn->finalized) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  }
  if (turn->refreshing || turn->starting) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_AGENT_HARNESS_RPC_BUSY;
  }
  if (turn->awaiting_approval) {
    approval_request_id = tstr_dup(turn->approval_request_id);
    if (!approval_request_id) {
      turbo_mutex_unlock(&thread->mutex);
      return TURBO_AGENT_HARNESS_RPC_INTERNAL;
    }
    turn->starting = 1;
    turbo_mutex_unlock(&thread->mutex);
    rc = turbo_agent_harness_turn_emit_resolution_and_completed(turn, approval_request_id,
                                                                "cancelled", "interrupted");
    turbo_mutex_lock(&thread->mutex);
    if (rc == TURBO_OK) {
      turn->awaiting_approval = 0;
      turn->interrupt_requested = 1;
      turn->finalized = 1;
      if (turn->summary) {
        turbo_json_object_set_string(turn->summary, "status", "cancelled");
      }
    }
    turn->starting = 0;
    turbo_mutex_unlock(&thread->mutex);
    tstr_free(approval_request_id);
    if (rc != TURBO_OK) return turbo_agent_harness_rpc_from_error(rc);
    *out_result = turbo_json_create_object();
    return *out_result ? TURBO_OK : TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  if (!turn->execution) {
    turbo_mutex_unlock(&thread->mutex);
    return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  }
  turn->interrupt_requested = 1;
  execution = turbo_agent_harness_execution_retain(turn->execution);
  turbo_mutex_unlock(&thread->mutex);
  if (!execution) return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  rc = turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_USER);
  turbo_agent_harness_execution_release(execution);
  if (rc != TURBO_OK) {
    turbo_mutex_lock(&thread->mutex);
    if (thread->turn == turn && !turn->finalized) turn->interrupt_requested = 0;
    turbo_mutex_unlock(&thread->mutex);
    return turbo_agent_harness_rpc_from_error(rc);
  }
  *out_result = turbo_json_create_object();
  return *out_result ? TURBO_OK : TURBO_AGENT_HARNESS_RPC_INTERNAL;
}

static int turbo_agent_harness_dispatch_turn_steer(turbo_agent_harness_connection_t *connection,
                                                   const json_value_t *params,
                                                   json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  const char *expected_turn_id = params ? turbo_json_get_string(params, "expectedTurnId") : NULL;
  turbo_agent_harness_thread_t *thread;
  json_value_t *message;
  json_value_t *result;
  tstr_t text = NULL;
  char *inbox_id = NULL;
  int rc;
  if (!thread_id || !expected_turn_id) return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  rc = turbo_agent_harness_input_text(params, &text);
  if (rc != TURBO_OK) {
    return rc == TURBO_ENOMEM ? TURBO_AGENT_HARNESS_RPC_INTERNAL
                              : TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  (void)turbo_agent_harness_turn_refresh(thread);
  turbo_mutex_lock(&thread->mutex);
  if (!thread->turn || strcmp(thread->turn->id, expected_turn_id) != 0 ||
      thread->turn->connection != connection || thread->turn->finalized ||
      thread->turn->awaiting_approval || thread->turn->refreshing || thread->turn->starting) {
    turbo_mutex_unlock(&thread->mutex);
    tstr_free(text);
    return TURBO_AGENT_HARNESS_RPC_BUSY;
  }
  turbo_mutex_unlock(&thread->mutex);
  message = turbo_json_create_object();
  if (!message) {
    tstr_free(text);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_set_string(message, "role", "user");
  turbo_json_object_set_string(message, "content", text);
  rc =
      turbo_agent_session_enqueue(turbo_agent_app_session(turbo_agent_harness_app(thread->harness)),
                                  TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id);
  turbo_runtime_json_destroy(message);
  tstr_free(text);
  if (rc != TURBO_OK) return turbo_agent_harness_rpc_from_error(rc);
  result = turbo_json_create_object();
  if (!result) {
    free(inbox_id);
    return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  turbo_json_object_set_string(result, "turnId", expected_turn_id);
  turbo_json_object_set_string(result, "inboxId", inbox_id);
  free(inbox_id);
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_approval(turbo_agent_harness_connection_t *connection,
                                                 const json_value_t *params,
                                                 json_value_t **out_result) {
  const char *thread_id = params ? turbo_json_get_string(params, "threadId") : NULL;
  const char *turn_id = params ? turbo_json_get_string(params, "turnId") : NULL;
  const char *request_id = params ? turbo_json_get_string(params, "requestId") : NULL;
  const char *decision = params ? turbo_json_get_string(params, "decision") : NULL;
  turbo_agent_harness_thread_t *thread;
  turbo_agent_harness_turn_t *turn;
  turbo_agent_harness_execution_t *old_execution;
  turbo_agent_harness_execution_t *new_execution = NULL;
  turbo_agent_harness_run_options_t options;
  turbo_agent_session_exec_options_t session_options = {TURBO_SESSION_SCOPE_THREAD,
                                                        TURBO_SESSION_INPUT_COMMAND, NULL};
  json_value_t *command = NULL;
  json_value_t *resolved = NULL;
  int rc = TURBO_OK;
  if (!thread_id || !turn_id || !request_id || !decision ||
      (strcmp(decision, "approve_once") != 0 && strcmp(decision, "deny") != 0)) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  thread = turbo_agent_harness_server_find_thread(connection->server, thread_id);
  if (!thread) return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  (void)turbo_agent_harness_turn_refresh(thread);
  if (strcmp(decision, "approve_once") == 0) {
    command = turbo_json_create_object();
    if (!command) return TURBO_AGENT_HARNESS_RPC_INTERNAL;
    turbo_json_object_set_string(command, "kind", "approve_review");
    turbo_json_object_set_bool(command, "approved", 1);
  }
  turbo_mutex_lock(&thread->mutex);
  turn = thread->turn;
  if (!turn || strcmp(turn->id, turn_id) != 0 || turn->connection != connection ||
      !turn->awaiting_approval || turn->starting || !turn->approval_request_id ||
      strcmp(turn->approval_request_id, request_id) != 0) {
    turbo_mutex_unlock(&thread->mutex);
    turbo_runtime_json_destroy(command);
    return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  }
  turn->starting = 1;
  if (strcmp(decision, "deny") == 0) {
    turbo_mutex_unlock(&thread->mutex);
    rc = turbo_agent_harness_turn_emit_resolution_and_completed(turn, request_id, decision,
                                                                "declined");
    turbo_mutex_lock(&thread->mutex);
    if (rc == TURBO_OK) {
      turn->awaiting_approval = 0;
      turn->finalized = 1;
      if (turn->summary) {
        turbo_json_object_set_string(turn->summary, "status", "declined");
      }
    }
    turn->starting = 0;
    turbo_mutex_unlock(&thread->mutex);
  } else {
    turbo_mutex_unlock(&thread->mutex);
    resolved = turbo_agent_harness_params_base(turn);
    if (resolved) {
      turbo_json_object_set_string(resolved, "requestId", request_id);
      turbo_json_object_set_string(resolved, "decision", decision);
    }
    if (!resolved) {
      turbo_mutex_lock(&thread->mutex);
      turn->starting = 0;
      turbo_mutex_unlock(&thread->mutex);
      turbo_runtime_json_destroy(command);
      return TURBO_AGENT_HARNESS_RPC_INTERNAL;
    }
    rc = turbo_agent_harness_emit(connection, "serverRequest/resolved", resolved, 0);
    if (rc != TURBO_OK) {
      turbo_mutex_lock(&thread->mutex);
      turn->starting = 0;
      turbo_mutex_unlock(&thread->mutex);
      turbo_runtime_json_destroy(command);
      return turbo_agent_harness_rpc_from_error(rc);
    }
    turbo_mutex_lock(&thread->mutex);
    old_execution = turn->execution;
    turn->execution = NULL;
    turbo_mutex_unlock(&thread->mutex);
    turbo_agent_harness_run_options_init(&options);
    options.session_options = &session_options;
    options.event_sink = turbo_agent_harness_turn_event_sink;
    options.event_sink_user_data = turn;
    rc = turbo_agent_harness_resume(thread->harness, command, &options, &new_execution);
    turbo_runtime_json_destroy(command);
    turbo_mutex_lock(&thread->mutex);
    if (rc == TURBO_OK) {
      turn->execution = new_execution;
      turbo_agent_harness_execution_release(old_execution);
      turbo_runtime_json_destroy(turn->summary);
      turbo_runtime_json_destroy(turn->state);
      turn->summary = NULL;
      turn->state = NULL;
      tstr_free(turn->approval_request_id);
      turn->approval_request_id = NULL;
      turn->awaiting_approval = 0;
    } else {
      turn->execution = old_execution;
      turn->awaiting_approval = 0;
      tstr_free(turn->approval_request_id);
      turn->approval_request_id = NULL;
      if (turn->summary) turbo_json_object_set_string(turn->summary, "status", "failed");
    }
    if (rc == TURBO_OK) turn->starting = 0;
    turbo_mutex_unlock(&thread->mutex);
    if (rc != TURBO_OK) {
      int emit_rc = turbo_agent_harness_turn_emit_completed(turn, "failed");
      turbo_mutex_lock(&thread->mutex);
      turn->starting = 0;
      turn->finalized = 1;
      turbo_mutex_unlock(&thread->mutex);
      if (emit_rc != TURBO_OK && emit_rc != TURBO_ESHUTDOWN) {
        turbo_agent_harness_connection_set_stream_error(connection, emit_rc);
      }
    }
  }
  if (rc != TURBO_OK) return turbo_agent_harness_rpc_from_error(rc);
  if (strcmp(decision, "deny") == 0) {
    *out_result = turbo_json_create_object();
    return *out_result ? TURBO_OK : TURBO_AGENT_HARNESS_RPC_INTERNAL;
  }
  *out_result = turbo_json_create_object();
  return *out_result ? TURBO_OK : TURBO_AGENT_HARNESS_RPC_INTERNAL;
}

static int turbo_agent_harness_dispatch_event_replay(turbo_agent_harness_connection_t *connection,
                                                     const json_value_t *params,
                                                     json_value_t **out_result) {
  const json_value_t *after_json = params ? turbo_json_object_get(params, "afterSequence") : NULL;
  const json_value_t *limit_json = params ? turbo_json_object_get(params, "limit") : NULL;
  turbo_agent_harness_event_t **selected;
  json_value_t *result = NULL;
  json_value_t *events = NULL;
  uint64_t after = 0;
  uint64_t next = 0;
  uint64_t latest = 0;
  size_t limit = connection->server->config.max_replay_events;
  size_t selected_count = 0;
  size_t index;
  int stream_error;

  turbo_agent_harness_connection_refresh(connection);
  if (after_json) {
    if (turbo_agent_harness_json_uint64(after_json, 0, TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER,
                                        &after) != TURBO_OK) {
      return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
    }
  }
  if (limit_json) {
    uint64_t value;
    if (turbo_agent_harness_json_uint64(limit_json, 1, (uint64_t)limit, &value) != TURBO_OK) {
      return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
    }
    limit = (size_t)value;
  }
  selected = (turbo_agent_harness_event_t **)calloc(limit, sizeof(*selected));
  if (!selected) return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  turbo_mutex_lock(&connection->event_mutex);
  stream_error = connection->stream_error;
  latest = connection->next_sequence;
  if (after < connection->acknowledged_sequence) {
    turbo_mutex_unlock(&connection->event_mutex);
    free(selected);
    return TURBO_AGENT_HARNESS_RPC_NOT_FOUND;
  }
  for (index = 0; index < turbo_deque_size(&connection->events) && selected_count < limit;
       ++index) {
    turbo_agent_harness_event_t *event =
        (turbo_agent_harness_event_t *)turbo_deque_at(&connection->events, index);
    if (event && event->sequence > after) selected[selected_count++] = event;
  }
  next = selected_count ? selected[selected_count - 1]->sequence : after;
  turbo_mutex_unlock(&connection->event_mutex);
  if (stream_error != TURBO_OK) {
    free(selected);
    return TURBO_AGENT_HARNESS_RPC_STREAM_FAILED;
  }
  result = turbo_json_create_object();
  events = turbo_json_create_array();
  if (!result || !events) goto fail;
  for (index = 0; index < selected_count; ++index) {
    json_value_t *copy = turbo_json_clone(selected[index]->message);
    json_value_t *copy_params = copy ? turbo_json_object_get(copy, "params") : NULL;
    if (!copy || !copy_params || turbo_json_type(copy_params) != TURBO_JSON_OBJECT) {
      turbo_runtime_json_destroy(copy);
      goto fail;
    }
    turbo_json_object_set_number(copy_params, "sequence", (double)selected[index]->sequence);
    if (!turbo_json_array_add_checked(events, copy)) {
      turbo_runtime_json_destroy(copy);
      goto fail;
    }
  }
  turbo_json_object_add(result, "events", events);
  events = NULL;
  turbo_json_object_set_number(result, "nextSequence", (double)next);
  turbo_json_object_set_bool(result, "hasMore", next < latest ? 1 : 0);
  free(selected);
  *out_result = result;
  return TURBO_OK;
fail:
  free(selected);
  turbo_runtime_json_destroy(events);
  turbo_runtime_json_destroy(result);
  return TURBO_AGENT_HARNESS_RPC_INTERNAL;
}

static int turbo_agent_harness_dispatch_event_ack(turbo_agent_harness_connection_t *connection,
                                                  const json_value_t *params,
                                                  json_value_t **out_result) {
  const json_value_t *through_json =
      params ? turbo_json_object_get(params, "throughSequence") : NULL;
  uint64_t through;
  json_value_t *result;
  if (turbo_agent_harness_json_uint64(through_json, 0, TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER,
                                      &through) != TURBO_OK) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  if (turbo_agent_harness_connection_ack_events(connection, through) != TURBO_OK) {
    return TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS;
  }
  result = turbo_json_create_object();
  if (!result) return TURBO_AGENT_HARNESS_RPC_INTERNAL;
  turbo_json_object_set_number(result, "acknowledgedSequence", (double)through);
  *out_result = result;
  return TURBO_OK;
}

static int turbo_agent_harness_dispatch_method(turbo_agent_harness_connection_t *connection,
                                               const char *method, const json_value_t *params,
                                               json_value_t **out_result) {
  if (strcmp(method, "initialize") == 0) {
    return turbo_agent_harness_dispatch_initialize(connection, params, out_result);
  }
  if (connection->state != TURBO_AGENT_HARNESS_CONNECTION_READY) {
    return TURBO_AGENT_HARNESS_RPC_NOT_INITIALIZED;
  }
  if (strcmp(method, "thread/start") == 0) {
    return turbo_agent_harness_dispatch_thread_load(connection, params, 1, out_result);
  }
  if (strcmp(method, "thread/resume") == 0) {
    return turbo_agent_harness_dispatch_thread_load(connection, params, 0, out_result);
  }
  if (strcmp(method, "thread/get") == 0) {
    return turbo_agent_harness_dispatch_thread_get(connection, params, out_result);
  }
  if (strcmp(method, "thread/list") == 0) {
    return turbo_agent_harness_dispatch_thread_list(connection, params, out_result);
  }
  if (strcmp(method, "turn/start") == 0) {
    return turbo_agent_harness_dispatch_turn_start(connection, params, out_result);
  }
  if (strcmp(method, "turn/get") == 0) {
    return turbo_agent_harness_dispatch_turn_get(connection, params, out_result);
  }
  if (strcmp(method, "turn/interrupt") == 0) {
    return turbo_agent_harness_dispatch_turn_interrupt(connection, params, out_result);
  }
  if (strcmp(method, "turn/steer") == 0) {
    return turbo_agent_harness_dispatch_turn_steer(connection, params, out_result);
  }
  if (strcmp(method, "approval/respond") == 0) {
    return turbo_agent_harness_dispatch_approval(connection, params, out_result);
  }
  if (strcmp(method, "event/replay") == 0) {
    return turbo_agent_harness_dispatch_event_replay(connection, params, out_result);
  }
  if (strcmp(method, "event/ack") == 0) {
    return turbo_agent_harness_dispatch_event_ack(connection, params, out_result);
  }
  return TURBO_AGENT_HARNESS_RPC_METHOD_NOT_FOUND;
}

void turbo_agent_harness_server_config_init(turbo_agent_harness_server_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_HARNESS_SERVER_ABI_VERSION;
  config->max_threads = 32;
  config->max_event_count = 1024;
  config->max_event_bytes = 8U * 1024U * 1024U;
  config->max_single_event_bytes = 256U * 1024U;
  config->max_replay_events = 128;
  config->inbox.struct_size = sizeof(config->inbox);
  config->inbox.abi_version = TURBO_AGENT_INBOX_ABI_VERSION;
  config->inbox.max_items = 64;
  config->inbox.max_total_bytes = 256U * 1024U;
  config->inbox.max_item_bytes = 16U * 1024U;
  config->inbox.max_follow_ups_per_execution = 8;
}

static int
turbo_agent_harness_server_config_valid(const turbo_agent_harness_server_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_AGENT_HARNESS_SERVER_ABI_VERSION && config->thread_factory &&
         config->max_threads && config->max_threads <= TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER &&
         config->max_event_count >= 8 &&
         config->max_event_count <= TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER &&
         config->max_event_bytes && config->max_single_event_bytes &&
         config->max_single_event_bytes <= config->max_event_bytes &&
         config->max_single_event_bytes <=
             SIZE_MAX / (TURBO_AGENT_HARNESS_CONTROL_EVENT_RESERVE + 1) &&
         config->max_event_bytes >=
             (TURBO_AGENT_HARNESS_CONTROL_EVENT_RESERVE + 1) * config->max_single_event_bytes &&
         config->max_replay_events &&
         config->max_replay_events <= TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER &&
         config->max_replay_events <= config->max_event_count &&
         config->inbox.struct_size >= sizeof(config->inbox) &&
         config->inbox.abi_version == TURBO_AGENT_INBOX_ABI_VERSION && config->inbox.max_items &&
         config->inbox.max_total_bytes && config->inbox.max_item_bytes &&
         config->inbox.max_item_bytes <= config->inbox.max_total_bytes &&
         config->inbox.max_follow_ups_per_execution;
}

turbo_agent_harness_server_t *
turbo_agent_harness_server_create(const turbo_agent_harness_server_config_t *config) {
  turbo_agent_harness_server_t *server;
  if (!turbo_agent_harness_server_config_valid(config)) return NULL;
  server = (turbo_agent_harness_server_t *)calloc(1, sizeof(*server));
  if (!server) return NULL;
  server->config = *config;
  turbo_mutex_init(&server->mutex);
  if (!server->mutex ||
      turbo_vec_init(&server->threads, sizeof(turbo_agent_harness_thread_t)) != TURBO_OK ||
      turbo_vec_reserve(&server->threads, config->max_threads) != TURBO_OK) {
    turbo_agent_harness_server_destroy(server);
    return NULL;
  }
  return server;
}

void turbo_agent_harness_server_destroy(turbo_agent_harness_server_t *server) {
  size_t index;
  if (!server) return;
  for (index = 0; index < turbo_vec_size(&server->threads); ++index) {
    turbo_agent_harness_thread_t *thread =
        (turbo_agent_harness_thread_t *)turbo_vec_at(&server->threads, index);
    turbo_agent_harness_thread_destroy(thread);
  }
  turbo_vec_destroy(&server->threads);
  turbo_mutex_destroy(&server->mutex);
  if (server->config.thread_factory_user_data_free) {
    server->config.thread_factory_user_data_free(server->config.thread_factory_user_data);
  }
  free(server);
}

turbo_agent_harness_connection_t *
turbo_agent_harness_server_open_connection(turbo_agent_harness_server_t *server) {
  turbo_agent_harness_connection_t *connection;
  if (!server) return NULL;
  connection = (turbo_agent_harness_connection_t *)calloc(1, sizeof(*connection));
  if (!connection) return NULL;
  atomic_init(&connection->ref_count, 1);
  connection->server = server;
  turbo_mutex_init(&connection->dispatch_mutex);
  turbo_mutex_init(&connection->event_mutex);
  turbo_cond_init(&connection->event_changed);
  if (!connection->dispatch_mutex || !connection->event_mutex || !connection->event_changed ||
      turbo_deque_init(&connection->events, sizeof(turbo_agent_harness_event_t)) != TURBO_OK ||
      turbo_deque_reserve(&connection->events, server->config.max_event_count) != TURBO_OK) {
    turbo_agent_harness_connection_release_internal(connection);
    return NULL;
  }
  return connection;
}

void turbo_agent_harness_connection_close(turbo_agent_harness_connection_t *connection) {
  size_t count;
  size_t index;
  if (!connection) return;
  turbo_mutex_lock(&connection->event_mutex);
  connection->closed = 1;
  turbo_cond_broadcast(&connection->event_changed);
  turbo_mutex_unlock(&connection->event_mutex);
  turbo_mutex_lock(&connection->server->mutex);
  count = turbo_vec_size(&connection->server->threads);
  turbo_mutex_unlock(&connection->server->mutex);
  for (index = 0; index < count; ++index) {
    turbo_agent_harness_thread_t *thread;
    turbo_agent_harness_turn_t *turn;
    turbo_agent_harness_execution_t *execution = NULL;
    turbo_mutex_lock(&connection->server->mutex);
    thread = (turbo_agent_harness_thread_t *)turbo_vec_at(&connection->server->threads, index);
    turbo_mutex_unlock(&connection->server->mutex);
    turbo_mutex_lock(&thread->mutex);
    turn = thread->turn;
    if (turn && turn->connection == connection && !turn->finalized) {
      turn->interrupt_requested = 1;
      if (turn->execution) {
        execution = turbo_agent_harness_execution_retain(turn->execution);
      } else {
        turn->awaiting_approval = 0;
        turn->finalized = 1;
      }
    }
    turbo_mutex_unlock(&thread->mutex);
    if (execution) {
      (void)turbo_agent_harness_execution_cancel(execution, TURBO_CANCEL_SHUTDOWN);
      (void)turbo_agent_harness_execution_wait(execution, UINT64_MAX);
      turbo_agent_harness_execution_release(execution);
      turbo_mutex_lock(&thread->mutex);
      if (thread->turn == turn) {
        turn->awaiting_approval = 0;
        turn->starting = 0;
        turn->finalized = 1;
      }
      turbo_mutex_unlock(&thread->mutex);
    }
  }
  turbo_agent_harness_connection_release_internal(connection);
}

int turbo_agent_harness_connection_wait_event_json_value(
    turbo_agent_harness_connection_t *connection, uint64_t timeout_ms,
    json_value_t **out_event_json, uint64_t *out_sequence) {
  turbo_agent_harness_connection_t *retained;
  uint64_t deadline_ms = UINT64_MAX;
  int timed = timeout_ms != UINT64_MAX;
  int rc = TURBO_OK;

  if (!out_event_json || !out_sequence) return TURBO_EINVAL;
  *out_event_json = NULL;
  *out_sequence = 0;
  retained = turbo_agent_harness_connection_retain_internal(connection);
  if (!retained) return TURBO_ESHUTDOWN;
  if (timed) {
    deadline_ms = turbo_agent_harness_saturating_add_ms(turbo_monotonic_ms(), timeout_ms);
  }

  for (;;) {
    turbo_agent_harness_event_t *front;
    json_value_t *source = NULL;
    uint64_t sequence = 0;
    uint64_t wait_ms = TURBO_AGENT_HARNESS_EVENT_WAIT_SLICE_MS;

    turbo_agent_harness_connection_refresh(retained);
    turbo_mutex_lock(&retained->event_mutex);
    if (retained->closed) {
      rc = TURBO_ESHUTDOWN;
    } else if (retained->stream_error != TURBO_OK) {
      rc = retained->stream_error;
    } else if ((front = (turbo_agent_harness_event_t *)turbo_deque_front(&retained->events)) !=
               NULL) {
      source = front->message;
      sequence = front->sequence;
    } else if (timed) {
      uint64_t now_ms = turbo_monotonic_ms();
      if (now_ms >= deadline_ms) {
        rc = TURBO_ETIMEDOUT;
      } else if (deadline_ms - now_ms < wait_ms) {
        wait_ms = deadline_ms - now_ms;
      }
    }

    if (source || rc != TURBO_OK) {
      turbo_mutex_unlock(&retained->event_mutex);
    } else {
      (void)turbo_cond_timedwait(&retained->event_changed, &retained->event_mutex,
                                 wait_ms * TURBO_AGENT_HARNESS_MS_TO_NS);
      turbo_mutex_unlock(&retained->event_mutex);
      continue;
    }

    if (source) {
      json_value_t *copy = turbo_json_clone(source);
      json_value_t *params = copy ? turbo_json_object_get(copy, "params") : NULL;
      if (!copy) {
        rc = TURBO_ENOMEM;
      } else if (!params || turbo_json_type(params) != TURBO_JSON_OBJECT) {
        turbo_runtime_json_destroy(copy);
        rc = TURBO_EPROTO;
      } else {
        turbo_json_object_set_number(params, "sequence", (double)sequence);
        *out_event_json = copy;
        *out_sequence = sequence;
      }
    }
    break;
  }

  turbo_agent_harness_connection_release_internal(retained);
  return rc;
}

int turbo_agent_harness_connection_ack_events(turbo_agent_harness_connection_t *connection,
                                              uint64_t sequence) {
  turbo_agent_harness_connection_t *retained;
  int rc = TURBO_OK;

  if (sequence > TURBO_AGENT_HARNESS_MAX_EXACT_JSON_INTEGER) return TURBO_EINVAL;
  retained = turbo_agent_harness_connection_retain_internal(connection);
  if (!retained) return TURBO_ESHUTDOWN;

  turbo_mutex_lock(&retained->event_mutex);
  if (retained->closed) {
    rc = TURBO_ESHUTDOWN;
  } else if (sequence > retained->next_sequence) {
    rc = TURBO_EINVAL;
  }
  while (rc == TURBO_OK) {
    turbo_agent_harness_event_t *front =
        (turbo_agent_harness_event_t *)turbo_deque_front(&retained->events);
    turbo_agent_harness_event_t released;
    if (!front || front->sequence > sequence) break;
    if (turbo_deque_pop_front(&retained->events, &released) != TURBO_OK) {
      rc = TURBO_EIO;
      break;
    }
    retained->event_bytes -= released.bytes;
    retained->acknowledged_sequence = released.sequence;
    turbo_cond_broadcast(&retained->event_changed);
    turbo_mutex_unlock(&retained->event_mutex);
    turbo_agent_harness_event_destroy(&released);
    turbo_mutex_lock(&retained->event_mutex);
    if (retained->closed) rc = TURBO_ESHUTDOWN;
  }
  if (rc == TURBO_OK && sequence > retained->acknowledged_sequence) {
    retained->acknowledged_sequence = sequence;
  }
  turbo_mutex_unlock(&retained->event_mutex);
  turbo_agent_harness_connection_release_internal(retained);
  return rc;
}

int turbo_agent_harness_connection_dispatch_json_value(turbo_agent_harness_connection_t *connection,
                                                       const json_value_t *request_json,
                                                       json_value_t **out_response_json) {
  const json_value_t *params;
  const json_value_t *id;
  const char *method;
  json_value_t *result = NULL;
  int rpc_rc;
  int rc;
  if (!out_response_json) return TURBO_EINVAL;
  *out_response_json = NULL;
  if (!connection || !request_json) return TURBO_EINVAL;
  turbo_mutex_lock(&connection->dispatch_mutex);
  turbo_mutex_lock(&connection->event_mutex);
  if (connection->closed) {
    turbo_mutex_unlock(&connection->event_mutex);
    turbo_mutex_unlock(&connection->dispatch_mutex);
    return TURBO_ESHUTDOWN;
  }
  turbo_mutex_unlock(&connection->event_mutex);
  if (turbo_json_type(request_json) != TURBO_JSON_OBJECT ||
      !(method = turbo_json_get_string(request_json, "method")) || !method[0]) {
    rc = turbo_agent_harness_response_error(request_json, TURBO_AGENT_HARNESS_RPC_INVALID_REQUEST,
                                            out_response_json);
    turbo_mutex_unlock(&connection->dispatch_mutex);
    return rc;
  }
  params = turbo_json_object_get(request_json, "params");
  if (params && turbo_json_type(params) != TURBO_JSON_OBJECT &&
      turbo_json_type(params) != TURBO_JSON_NULL) {
    rc = turbo_agent_harness_response_error(request_json, TURBO_AGENT_HARNESS_RPC_INVALID_PARAMS,
                                            out_response_json);
    turbo_mutex_unlock(&connection->dispatch_mutex);
    return rc;
  }
  if (params && turbo_json_type(params) == TURBO_JSON_NULL) params = NULL;
  id = turbo_json_object_get(request_json, "id");
  if (strcmp(method, "initialized") == 0) {
    if (id || connection->state != TURBO_AGENT_HARNESS_CONNECTION_INITIALIZED ||
        (params && turbo_json_object_size(params) != 0)) {
      rc = turbo_agent_harness_response_error(request_json,
                                              connection->state ==
                                                      TURBO_AGENT_HARNESS_CONNECTION_READY
                                                  ? TURBO_AGENT_HARNESS_RPC_ALREADY_INITIALIZED
                                                  : TURBO_AGENT_HARNESS_RPC_INVALID_REQUEST,
                                              out_response_json);
    } else {
      connection->state = TURBO_AGENT_HARNESS_CONNECTION_READY;
      rc = TURBO_OK;
    }
    turbo_mutex_unlock(&connection->dispatch_mutex);
    return rc;
  }
  if (!id) {
    rc = turbo_agent_harness_response_error(request_json, TURBO_AGENT_HARNESS_RPC_INVALID_REQUEST,
                                            out_response_json);
    turbo_mutex_unlock(&connection->dispatch_mutex);
    return rc;
  }
  rpc_rc = turbo_agent_harness_dispatch_method(connection, method, params, &result);
  if (rpc_rc == TURBO_OK && result) {
    rc = turbo_agent_harness_response_success(request_json, result, out_response_json);
  } else {
    turbo_runtime_json_destroy(result);
    if (rpc_rc == TURBO_OK) rpc_rc = TURBO_AGENT_HARNESS_RPC_INTERNAL;
    rc = turbo_agent_harness_response_error(request_json, rpc_rc, out_response_json);
  }
  turbo_mutex_unlock(&connection->dispatch_mutex);
  return rc;
}

int turbo_agent_harness_connection_dispatch_text(turbo_agent_harness_connection_t *connection,
                                                 const char *request_json_text,
                                                 char **out_response_json_text) {
  json_value_t *request = NULL;
  json_value_t *response = NULL;
  char *serialized;
  int rc;
  if (!out_response_json_text) return TURBO_EINVAL;
  *out_response_json_text = NULL;
  if (!connection || !request_json_text || !request_json_text[0]) return TURBO_EINVAL;
  if (turbo_parse_json((const uint8_t *)request_json_text, strlen(request_json_text), &request) !=
          0 ||
      !request) {
    turbo_runtime_json_destroy(request);
    return TURBO_EPROTO;
  }
  rc = turbo_agent_harness_connection_dispatch_json_value(connection, request, &response);
  turbo_runtime_json_destroy(request);
  if (rc != TURBO_OK || !response) {
    turbo_runtime_json_destroy(response);
    return rc;
  }
  serialized = turbo_json_serialize(response, NULL);
  turbo_runtime_json_destroy(response);
  if (!serialized) return TURBO_ENOMEM;
  *out_response_json_text = serialized;
  return TURBO_OK;
}
