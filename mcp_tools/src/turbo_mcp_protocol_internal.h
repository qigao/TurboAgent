#ifndef TURBO_MCP_PROTOCOL_INTERNAL_H
#define TURBO_MCP_PROTOCOL_INTERNAL_H

#include "turbo_mcp_tool_pack.h"

#include <turbo_http.h>
#include <json_parser.h>
#include <turbo_str.h>

typedef struct turbo_mcp_client_s turbo_mcp_client_t;

typedef struct turbo_mcp_client_config_s {
  const char *endpoint;
  const char *client_name;
  const char *client_version;
  const char *bearer_token;
  int64_t timeout_ms;
  size_t max_request_bytes;
  size_t max_response_bytes;
  size_t max_headers;
  size_t max_header_bytes;
  turbo_mcp_transport_post_fn transport_post;
  void *transport_user_data;
} turbo_mcp_client_config_t;

turbo_mcp_client_t *turbo_mcp_client_create(const turbo_mcp_client_config_t *config);
void turbo_mcp_client_destroy(turbo_mcp_client_t *client);

turbo_tool_status_t turbo_mcp_client_request(
    turbo_mcp_client_t *client, const char *method, const char *name,
    json_value_t *params, const char *const *parameter_headers,
    size_t parameter_header_count, json_value_t **out_result);

const char *turbo_mcp_client_last_error(const turbo_mcp_client_t *client);
void turbo_mcp_client_set_error(turbo_mcp_client_t *client, const char *message);
tstr_t turbo_mcp_format_header(const char *name, const char *value, size_t value_length);

#endif
