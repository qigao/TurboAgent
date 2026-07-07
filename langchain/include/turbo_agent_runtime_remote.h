#ifndef TURBO_AGENT_RUNTIME_REMOTE_H
#define TURBO_AGENT_RUNTIME_REMOTE_H

#include <platform.h>

#include "http_common.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_runtime_remote_s turbo_agent_runtime_remote_t;

typedef turbo_graph_t *(*turbo_agent_runtime_remote_graph_resolver_fn)(const char *graph_name,
                                                                       void *user_data);

typedef struct turbo_agent_runtime_remote_config_s {
  turbo_agent_runtime_t *runtime;
  const turbo_agent_memory_store_t *memory_store;
  turbo_agent_runtime_remote_graph_resolver_fn graph_resolver;
  void *graph_resolver_user_data;
} turbo_agent_runtime_remote_config_t;

/**
 * @brief Create one local JSON-RPC dispatcher over an existing runtime.
 *
 * This is a pure local bridge for future remote/server adapters. It does not
 * start an HTTP endpoint or own the runtime/store.
 */
CXX_C_API turbo_agent_runtime_remote_t *turbo_agent_runtime_remote_create(
    const turbo_agent_runtime_remote_config_t *config);

/**
 * @brief Destroy one local runtime remote dispatcher.
 */
CXX_C_API void turbo_agent_runtime_remote_destroy(turbo_agent_runtime_remote_t *remote);

/**
 * @brief Dispatch one JSON-RPC 2.0 request against the runtime surface.
 *
 * The current method set is:
 * - `runtime.start`
 * - `runtime.resume`
 * - `runtime.fork`
 * - `runtime.applyCommand`
 * - `runtime.resumeThreadCommandBindGraph`
 * - `runtime.forkThreadCommandBindGraph`
 * - `runtime.getThreadState`
 * - `runtime.updateThreadState`
 * - `runtime.applyThreadStatePatch`
 * - `runtime.resumeThreadBindGraph`
 * - `runtime.forkThreadBindGraph`
 * - `runtime.getCheckpointContext`
 * - `runtime.resumeThreadStatePatchBindGraph`
 * - `runtime.forkThreadStatePatchBindGraph`
 * - `runtime.getStartupDiagnostics`
 * - `runtime.getThreadObservabilityIndex`
 * - `runtime.listObservabilityIndexesFiltered`
 * - `memory.deleteRecord`
 * - `memory.getRecord`
 * - `memory.listRecords`
 * - `memory.putRecord`
 * - `memory.queryRecordsEx`
 * - `memory.validateRecord`
 *
 * Requests and responses use plain JSON-RPC 2.0 objects. This dispatcher is a
 * contract bridge, not a second persisted runtime state source.
 */
CXX_C_API int turbo_agent_runtime_remote_dispatch_jsonrpc(
    turbo_agent_runtime_remote_t *remote, const json_value_t *request_json,
    json_value_t **out_response_json);

/**
 * @brief Dispatch one JSON-RPC 2.0 request from raw JSON text.
 *
 * This is the transport-neutral text adapter above
 * `turbo_agent_runtime_remote_dispatch_jsonrpc(...)`. The returned string must
 * be freed with `turbo_json_serialize_free(...)`.
 */
CXX_C_API int turbo_agent_runtime_remote_dispatch_jsonrpc_text(
    turbo_agent_runtime_remote_t *remote, const char *request_json_text,
    char **out_response_json_text);

/**
 * @brief Handle one HTTP-like JSON-RPC request against the remote runtime bridge.
 *
 * This does not start a listener. It only maps one `POST /v1/runtime/jsonrpc`
 * style request into the existing JSON-RPC dispatcher and returns one heap
 * allocated `http_response_t` for future HTTP/server adapters. Adapter-level
 * path/method/body failures map to HTTP status codes, while the response body
 * remains JSON.
 */
CXX_C_API http_response_t *turbo_agent_runtime_remote_handle_http_jsonrpc(
    turbo_agent_runtime_remote_t *remote, http_method_t method, const char *path,
    const char *request_json_text);

#ifdef __cplusplus
}
#endif

#endif
