#ifndef TURBO_AGENT_HARNESS_SERVER_H
#define TURBO_AGENT_HARNESS_SERVER_H

#include <turbo_agent_api.h>

#include "turbo_agent_harness.h"
#include "turbo_agent_inbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_HARNESS_SERVER_ABI_VERSION 1U

typedef struct turbo_agent_harness_server_s turbo_agent_harness_server_t;
typedef struct turbo_agent_harness_connection_s turbo_agent_harness_connection_t;

/**
 * Create a Harness bound to exactly `thread_id`.
 *
 * The callback returns one owned Harness reference through `out_harness`.
 * Server validation requires `turbo_agent_app_thread_id(harness->app)` to equal
 * `thread_id`. The factory decides how Runtime Store, model, tools, policy,
 * Wasm sandbox and executor are shared or isolated.
 */
typedef int (*turbo_agent_harness_thread_factory_fn)(const char *thread_id,
                                                     turbo_agent_harness_t **out_harness,
                                                     void *user_data);

typedef void (*turbo_agent_harness_server_user_data_free_fn)(void *user_data);

/**
 * Bounded configuration for one transport-neutral Harness control plane.
 *
 * `max_threads` bounds loaded thread records. Each connection owns a journal
 * bounded by event count and serialized bytes. `max_replay_events` bounds one
 * `event/replay` response. The inbox configuration is applied to every
 * factory-created thread so `turn/steer` has deterministic capacity semantics.
 */
typedef struct turbo_agent_harness_server_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_harness_thread_factory_fn thread_factory;
  void *thread_factory_user_data;
  turbo_agent_harness_server_user_data_free_fn thread_factory_user_data_free;
  size_t max_threads;
  size_t max_event_count;
  size_t max_event_bytes;
  size_t max_single_event_bytes;
  size_t max_replay_events;
  turbo_agent_inbox_config_t inbox;
} turbo_agent_harness_server_config_t;

/** Initialize `config` with ABI v1 and bounded defaults. */
CXX_C_API void turbo_agent_harness_server_config_init(turbo_agent_harness_server_config_t *config);

/**
 * Create a server from a validated configuration.
 *
 * @return Owned server, or NULL for invalid configuration/allocation failure.
 */
CXX_C_API turbo_agent_harness_server_t *
turbo_agent_harness_server_create(const turbo_agent_harness_server_config_t *config);

/**
 * Destroy a quiescent server.
 *
 * Close every connection first. Destruction cancels and waits for active
 * executions before releasing their Harness objects. No dispatch call may be
 * concurrent with destruction.
 *
 * @param server Owned server; NULL is accepted.
 */
CXX_C_API void turbo_agent_harness_server_destroy(turbo_agent_harness_server_t *server);

/**
 * Open one independent protocol connection.
 *
 * The connection initially accepts only `initialize`; after its response the
 * client must send an `initialized` notification. The server must outlive the
 * connection.
 *
 * @return Owned connection, or NULL for allocation failure.
 */
CXX_C_API turbo_agent_harness_connection_t *
turbo_agent_harness_server_open_connection(turbo_agent_harness_server_t *server);

/**
 * Close one connection and wake blocked event producers.
 *
 * Active turns keep an internal connection reference until the server reaches
 * quiescence. The caller must first make dispatch calls on this connection
 * quiescent; close is not concurrent with dispatch. Calls after close are not
 * valid because close releases the caller-owned reference. Close cancels and
 * waits for turns owned by the connection so their event sinks cannot retain
 * a stale protocol owner.
 *
 * @param connection Owned connection; NULL is accepted.
 */
CXX_C_API void turbo_agent_harness_connection_close(turbo_agent_harness_connection_t *connection);

/**
 * Dispatch one headerless JSON-RPC-style request or notification.
 *
 * Requests return one caller-owned TurboParser JSON response. Successful
 * notifications return `SALTS_OK` with `*out_response_json == NULL`.
 * Protocol failures are represented as JSON `error` responses and still
 * return `SALTS_OK`; C errors mean the dispatcher could not produce a valid
 * response.
 *
 * @param connection Live connection.
 * @param request_json Borrowed TurboParser JSON object.
 * @param out_response_json Receives an owned response, or NULL for a
 * successful notification; destroy it with `turbo_runtime_json_destroy()`.
 * @return `SALTS_OK`, `SALTS_EINVAL`, `SALTS_ENOMEM`, or `SALTS_ESHUTDOWN`.
 */
CXX_C_API int
turbo_agent_harness_connection_dispatch_json_value(turbo_agent_harness_connection_t *connection,
                                                   const json_value_t *request_json,
                                                   json_value_t **out_response_json);

/**
 * JSON text adapter over `dispatch_json_value` using TurboParser.
 *
 * A successful notification returns `SALTS_OK` and NULL output. A non-NULL
 * output is released with `json_serialize_free()`.
 *
 * @param connection Live connection.
 * @param request_json_text Borrowed UTF-8 JSON text.
 * @param out_response_json_text Receives serialized response or NULL.
 * @return Dispatcher status; malformed JSON returns `SALTS_EPROTO`.
 */
CXX_C_API int
turbo_agent_harness_connection_dispatch_text(turbo_agent_harness_connection_t *connection,
                                             const char *request_json_text,
                                             char **out_response_json_text);

/**
 * Wait for and clone the oldest unacknowledged server event.
 *
 * Event delivery is single-consumer per connection: do not run this function,
 * `event/replay`, `event/ack`, or a JSONL event pump concurrently with another
 * event consumer. Request dispatch may run concurrently. A successful result
 * remains in the journal until `turbo_agent_harness_connection_ack_events()`.
 * `timeout_ms == UINT64_MAX` waits indefinitely; zero performs a non-blocking
 * check. Connection close wakes an established wait.
 *
 * @param connection Live connection.
 * @param timeout_ms Relative timeout in milliseconds.
 * @param out_event_json Receives an owned notification clone whose params
 * contain `sequence`; destroy it with `turbo_runtime_json_destroy()`.
 * @param out_sequence Receives the exact journal sequence.
 * @return `SALTS_OK`, `SALTS_ETIMEDOUT`, a stream error, or
 * `SALTS_ESHUTDOWN`.
 */
CXX_C_API int turbo_agent_harness_connection_wait_event_json_value(
    turbo_agent_harness_connection_t *connection, uint64_t timeout_ms,
    json_value_t **out_event_json, uint64_t *out_sequence);

/**
 * Acknowledge and release all journal events through `sequence`.
 *
 * @return `SALTS_OK`, `SALTS_EINVAL` for a future sequence, or
 * `SALTS_ESHUTDOWN` after close.
 */
CXX_C_API int
turbo_agent_harness_connection_ack_events(turbo_agent_harness_connection_t *connection,
                                          uint64_t sequence);

#ifdef __cplusplus
}
#endif

#endif
