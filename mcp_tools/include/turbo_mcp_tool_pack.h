#ifndef TURBO_MCP_TOOL_PACK_H
#define TURBO_MCP_TOOL_PACK_H

#include <turbo_agent_api.h>

#include "turbo_tool_registry.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_MCP_TOOL_PACK_ABI_VERSION 1u
#define TURBO_MCP_PROTOCOL_VERSION "2026-07-28"

typedef struct turbo_mcp_tool_pack_s turbo_mcp_tool_pack_t;

/** Caller-owned transport response. Every view remains valid until release(). */
typedef struct turbo_mcp_transport_response_s {
  int status_code;
  const char *content_type;
  const uint8_t *body;
  size_t body_len;
  void *release_context;
  void (*release)(void *release_context);
} turbo_mcp_transport_response_t;

/**
 * Optional host transport adapter, primarily for centrally managed OAuth,
 * proxies, observability, and deterministic tests. Request views are borrowed
 * only for the call. A successful callback returns zero and fills response.
 */
typedef int (*turbo_mcp_transport_post_fn)(
    void *user_data, const char *endpoint, const char *const *headers,
    size_t header_count, const uint8_t *body, size_t body_len,
    turbo_mcp_transport_response_t *response);

typedef struct turbo_mcp_tool_pack_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Absolute HTTP(S) MCP endpoint. Copied by create(). */
  const char *endpoint;
  /** Stable local namespace, limited to ASCII letters, digits, '_' and '-'. */
  const char *server_id;
  const char *client_name;
  const char *client_version;
  /** Optional host-owned credential copied into the default CHTTP client. */
  const char *bearer_token;
  int64_t timeout_ms;
  size_t max_tools;
  size_t max_pages;
  size_t max_request_bytes;
  size_t max_response_bytes;
  size_t max_headers;
  size_t max_header_bytes;
  turbo_tool_execution_policy_t execution_policy;
  /** Optional borrowed adapter and context; both must outlive the pack. */
  turbo_mcp_transport_post_fn transport_post;
  void *transport_user_data;
} turbo_mcp_tool_pack_config_t;

/** Initialize bounded defaults and MCP protocol version 2026-07-28. */
CXX_C_API void turbo_mcp_tool_pack_config_init(turbo_mcp_tool_pack_config_t *config);

/**
 * Create an empty pack and its transport. Discovery is explicit through
 * refresh(), so network/protocol errors have a return status and diagnostic.
 */
CXX_C_API turbo_mcp_tool_pack_t *
turbo_mcp_tool_pack_create(const turbo_mcp_tool_pack_config_t *config);

/** Destroy the registry before releasing transport and copied credentials. */
CXX_C_API void turbo_mcp_tool_pack_destroy(turbo_mcp_tool_pack_t *pack);

/**
 * Discover every tools/list page and atomically replace the current registry.
 * Invalid x-mcp-header tool definitions are excluded; other protocol errors
 * leave the existing registry unchanged.
 *
 * This is a single-owner control-plane operation. Do not refresh concurrently
 * with registry projection or execution.
 */
CXX_C_API turbo_tool_status_t turbo_mcp_tool_pack_refresh(turbo_mcp_tool_pack_t *pack);

/** Borrowed registry; it remains valid until the next refresh or destruction. */
CXX_C_API turbo_tool_registry_t *turbo_mcp_tool_pack_registry(turbo_mcp_tool_pack_t *pack);

CXX_C_API size_t turbo_mcp_tool_pack_tool_count(const turbo_mcp_tool_pack_t *pack);
CXX_C_API size_t turbo_mcp_tool_pack_rejected_tool_count(const turbo_mcp_tool_pack_t *pack);

/** Borrowed diagnostic, replaced by the next pack operation. */
CXX_C_API const char *turbo_mcp_tool_pack_last_error(const turbo_mcp_tool_pack_t *pack);

#ifdef __cplusplus
}
#endif

#endif
