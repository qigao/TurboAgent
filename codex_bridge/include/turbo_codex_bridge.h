#ifndef TURBO_CODEX_BRIDGE_H
#define TURBO_CODEX_BRIDGE_H

#include <platform.h>
#include <turbo_error.h>
#include <turbo_runtime_json.h>

#include "turbo_tool_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_CODEX_BRIDGE_ABI_VERSION 1U
#define TURBO_CODEX_TRANSPORT_ABI_VERSION 1U
#define TURBO_CODEX_STDIO_TRANSPORT_ABI_VERSION 1U
#define TURBO_CODEX_DELEGATE_TOOL_ABI_VERSION 1U

typedef struct turbo_codex_client_s turbo_codex_client_t;

/** Write one complete UTF-8 JSONL frame, including its trailing LF. */
typedef int (*turbo_codex_transport_write_fn)(const uint8_t *frame, size_t frame_size,
                                               void *user_data);

/**
 * Read one UTF-8 JSON object without CR/LF.
 *
 * The callback allocates `*out_line` with malloc-compatible storage. The
 * client releases it with free(). A zero timeout is non-blocking and
 * `UINT64_MAX` waits indefinitely.
 */
typedef int (*turbo_codex_transport_read_fn)(uint64_t timeout_ms, char **out_line,
                                              void *user_data);
typedef void (*turbo_codex_transport_close_fn)(void *user_data);
typedef void (*turbo_codex_transport_user_data_free_fn)(void *user_data);

/** Bidirectional, single-owner transport used by the protocol client. */
typedef struct turbo_codex_transport_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_codex_transport_write_fn write;
  turbo_codex_transport_read_fn read;
  turbo_codex_transport_close_fn close;
  void *user_data;
  turbo_codex_transport_user_data_free_fn user_data_free;
} turbo_codex_transport_t;

/** Close, release and clear a transport that has no client owner. */
CXX_C_API void turbo_codex_transport_release(turbo_codex_transport_t *transport);

/** Observe one borrowed Codex notification on the caller's pump thread. */
typedef void (*turbo_codex_event_fn)(const char *method, const json_value_t *params,
                                     void *user_data);

/**
 * Resolve a Codex server-initiated request such as command/file approval.
 *
 * On success, return TURBO_OK and one owned JSON result through `out_result`.
 * Returning an error produces a JSON-RPC error response. When no handler is
 * configured, the client fails closed: known approval requests are declined
 * and unknown requests receive Method not found.
 */
typedef int (*turbo_codex_server_request_fn)(const char *method,
                                              const json_value_t *params,
                                              json_value_t **out_result,
                                              void *user_data);
typedef void (*turbo_codex_user_data_free_fn)(void *user_data);

typedef struct turbo_codex_client_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_codex_transport_t transport;
  const char *client_name;
  const char *client_title;
  const char *client_version;
  int experimental_api;
  size_t max_message_bytes;
  uint64_t initialize_timeout_ms;
  turbo_codex_event_fn event;
  void *event_user_data;
  turbo_codex_user_data_free_fn event_user_data_free;
  turbo_codex_server_request_fn server_request;
  void *server_request_user_data;
  turbo_codex_user_data_free_fn server_request_user_data_free;
} turbo_codex_client_config_t;

CXX_C_API void turbo_codex_client_config_init(turbo_codex_client_config_t *config);

/**
 * Create a single-owner client and take ownership of the configured transport.
 * Ownership transfers only on success. On failure, release the transport with
 * turbo_codex_transport_release(). Public calls must not overlap.
 */
CXX_C_API turbo_codex_client_t *
turbo_codex_client_create(const turbo_codex_client_config_t *config);
CXX_C_API void turbo_codex_client_destroy(turbo_codex_client_t *client);

/** Perform initialize/initialized exactly once. */
CXX_C_API int turbo_codex_client_initialize(turbo_codex_client_t *client,
                                             json_value_t **out_server_info);

/**
 * Send one protocol request and wait for its matching response.
 * Interleaved notifications and server requests are dispatched while waiting.
 */
CXX_C_API int turbo_codex_client_request(turbo_codex_client_t *client,
                                          const char *method,
                                          const json_value_t *params,
                                          uint64_t timeout_ms,
                                          json_value_t **out_result);

/** Read and dispatch one notification or server request. */
CXX_C_API int turbo_codex_client_pump(turbo_codex_client_t *client,
                                       uint64_t timeout_ms);

typedef struct turbo_codex_run_options_s {
  size_t struct_size;
  uint32_t abi_version;
  const char *thread_id;
  const char *model;
  const char *cwd;
  const char *sandbox;
  const char *approval_policy;
  int ephemeral_thread;
  uint64_t timeout_ms;
} turbo_codex_run_options_t;

CXX_C_API void turbo_codex_run_options_init(turbo_codex_run_options_t *options);

/**
 * Execute one complete Codex turn, creating a thread when `thread_id` is NULL.
 * Returned thread id, turn id and text are malloc-owned by the caller.
 */
CXX_C_API int turbo_codex_client_run_text(turbo_codex_client_t *client,
                                           const char *prompt,
                                           const turbo_codex_run_options_t *options,
                                           char **out_thread_id,
                                           char **out_turn_id,
                                           char **out_text,
                                           json_value_t **out_turn);

/** Borrowed diagnostic replaced by the next client operation. */
CXX_C_API const char *turbo_codex_client_last_error(const turbo_codex_client_t *client);

typedef struct turbo_codex_stdio_transport_config_s {
  size_t struct_size;
  uint32_t abi_version;
  const char *codex_executable;
  const char *working_directory;
  size_t max_line_bytes;
} turbo_codex_stdio_transport_config_t;

CXX_C_API void
turbo_codex_stdio_transport_config_init(turbo_codex_stdio_transport_config_t *config);

/**
 * Start `codex app-server --stdio` and return an owned transport.
 * Android and other platforms without a child-process implementation return
 * TURBO_ENOTSUP.
 */
CXX_C_API int turbo_codex_stdio_transport_create(
    const turbo_codex_stdio_transport_config_t *config,
    turbo_codex_transport_t *out_transport);

typedef struct turbo_codex_delegate_tool_config_s {
  size_t struct_size;
  uint32_t abi_version;
  turbo_codex_client_t *client;
  const char *tool_name;
  const char *model;
  const char *cwd;
  const char *sandbox;
  const char *approval_policy;
  uint64_t timeout_ms;
} turbo_codex_delegate_tool_config_t;

CXX_C_API void
turbo_codex_delegate_tool_config_init(turbo_codex_delegate_tool_config_t *config);

/**
 * Register one exclusive `codex.delegate`-style tool requiring `delegate`.
 * The registry borrows the client; it must outlive the definition.
 */
CXX_C_API turbo_tool_status_t turbo_codex_delegate_tool_register(
    turbo_tool_registry_t *registry,
    const turbo_codex_delegate_tool_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
