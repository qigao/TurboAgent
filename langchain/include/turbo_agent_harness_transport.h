#ifndef TURBO_AGENT_HARNESS_TRANSPORT_H
#define TURBO_AGENT_HARNESS_TRANSPORT_H

#include <turbo_agent_api.h>

#include "turbo_agent_harness_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_HARNESS_JSONL_TRANSPORT_ABI_VERSION 1U

typedef struct turbo_agent_harness_jsonl_transport_s turbo_agent_harness_jsonl_transport_t;

/**
 * Synchronously write one complete JSON-lines frame.
 *
 * `bytes` includes the trailing LF and contains no embedded line delimiter.
 * The callback borrows the frame only for the call and must either consume all
 * bytes or return an error. The transport serializes callback invocations; the
 * callback must not re-enter the same transport.
 */
typedef int (*turbo_agent_harness_jsonl_write_fn)(const uint8_t *frame, size_t frame_size,
                                                  void *user_data);

typedef void (*turbo_agent_harness_jsonl_user_data_free_fn)(void *user_data);

typedef struct turbo_agent_harness_jsonl_transport_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_agent_harness_connection_t *connection;
  turbo_agent_harness_jsonl_write_fn write;
  void *write_user_data;
  turbo_agent_harness_jsonl_user_data_free_fn write_user_data_free;
  size_t max_event_batch;
} turbo_agent_harness_jsonl_transport_config_t;

/** Initialize ABI v1 with a bounded default event batch. */
CXX_C_API void turbo_agent_harness_jsonl_transport_config_init(
    turbo_agent_harness_jsonl_transport_config_t *config);

/**
 * Create a JSON-lines adapter that retains `config.connection`.
 *
 * The caller still closes its connection reference; the retained object stays
 * valid until transport destroy. Writer user data is borrowed when
 * `write_user_data_free` is NULL; otherwise ownership transfers to the adapter
 * and the callback runs during destroy after all calls are quiescent.
 */
CXX_C_API turbo_agent_harness_jsonl_transport_t *turbo_agent_harness_jsonl_transport_create(
    const turbo_agent_harness_jsonl_transport_config_t *config);

/** Destroy a quiescent adapter; its retained connection is released, not closed. */
CXX_C_API void
turbo_agent_harness_jsonl_transport_destroy(turbo_agent_harness_jsonl_transport_t *transport);

/**
 * Dispatch one input JSON line and write its response frame when present.
 *
 * The input excludes CR/LF. Successful notifications produce no output.
 */
CXX_C_API int
turbo_agent_harness_jsonl_transport_dispatch_line(turbo_agent_harness_jsonl_transport_t *transport,
                                                  const char *request_json_line);

/**
 * Wait for one event, then drain up to `max_event_batch` JSONL frames.
 *
 * Each event is acknowledged only after its frame is written successfully. A
 * writer error leaves the current event replayable. One pump call must not run
 * concurrently with another pump on the same connection.
 *
 * @param timeout_ms Timeout for the first event; later events are non-blocking.
 * @param out_event_count Receives successfully written event count.
 */
CXX_C_API int
turbo_agent_harness_jsonl_transport_pump_events(turbo_agent_harness_jsonl_transport_t *transport,
                                                uint64_t timeout_ms, size_t *out_event_count);

#ifdef __cplusplus
}
#endif

#endif
