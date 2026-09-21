#include "turbo_mcp_protocol_internal.h"

#include "base64_utils.h"
#include "turbo_runtime_json.h"

#include <limits.h>
#include <uri_parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TURBO_MCP_DEFAULT_TIMEOUT_MS = 10000,
  TURBO_MCP_ERROR_MESSAGE_BYTES = 512
};

typedef struct turbo_mcp_http_response_owner_s {
  chttp_response response;
} turbo_mcp_http_response_owner_t;

typedef struct turbo_mcp_http_endpoint_s {
  char connection_uri[640];
  char authority[320];
  char *target;
} turbo_mcp_http_endpoint_t;

struct turbo_mcp_client_s {
  tstr_t endpoint;
  tstr_t client_name;
  tstr_t client_version;
  tstr_t bearer_authorization;
  chttp_client http;
  int http_initialized;
  turbo_mcp_transport_post_fn transport_post;
  void *transport_user_data;
  size_t max_request_bytes;
  size_t max_response_bytes;
  size_t max_headers;
  size_t max_header_bytes;
  uint64_t next_request_id;
  uint32_t timeout_ms;
  char last_error[TURBO_MCP_ERROR_MESSAGE_BYTES];
};

static native_io_backend_kind turbo_mcp_http_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  return NATIVE_IO_BACKEND_KQUEUE;
#else
  return (native_io_backend_kind)0;
#endif
}

static int turbo_mcp_http_endpoint_build(const char *url, turbo_mcp_http_endpoint_t *endpoint) {
  uri_t uri;
  int default_port;
  int port;
  int is_ipv6;
  int written;
  const char *transport_scheme;
  char *target;

  if (!url || !endpoint) return -1;
  memset(endpoint, 0, sizeof(*endpoint));
  memset(&uri, 0, sizeof(uri));
  if (!uri_parse(url, &uri) || !uri.valid || !uri.host[0] ||
      (uri.component_flags & (URI_COMPONENT_USERINFO | URI_COMPONENT_FRAGMENT)) != 0u ||
      (uri.overflow_flags & URI_OVERFLOW_PORT) != 0u) {
    return -1;
  }

  if (strcmp(uri.scheme, "https") == 0) {
    transport_scheme = "tls";
    default_port = 443;
  } else if (strcmp(uri.scheme, "http") == 0) {
    transport_scheme = "tcp";
    default_port = 80;
  } else {
    return -1;
  }

  port = (uri.component_flags & URI_COMPONENT_PORT) != 0u ? uri.port : default_port;
  if (port <= 0 || port > 65535) return -1;
  is_ipv6 = uri.host_type == URI_HOST_IPV6ADDR;

  written = is_ipv6
                ? snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                           "%s://[%s]:%d", transport_scheme, uri.host, port)
                : snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                           "%s://%s:%d", transport_scheme, uri.host, port);
  if (written < 0 || (size_t)written >= sizeof(endpoint->connection_uri)) return -1;

  if ((uri.component_flags & URI_COMPONENT_PORT) != 0u) {
    written = is_ipv6 ? snprintf(endpoint->authority, sizeof(endpoint->authority), "[%s]:%d",
                                 uri.host, port)
                      : snprintf(endpoint->authority, sizeof(endpoint->authority), "%s:%d",
                                 uri.host, port);
  } else {
    written = is_ipv6 ? snprintf(endpoint->authority, sizeof(endpoint->authority), "[%s]", uri.host)
                      : snprintf(endpoint->authority, sizeof(endpoint->authority), "%s", uri.host);
  }
  if (written < 0 || (size_t)written >= sizeof(endpoint->authority)) return -1;

  target = tstr_dup(uri.path[0] ? uri.path : "/");
  if (!target) return -1;
  if ((uri.component_flags & URI_COMPONENT_QUERY) != 0u) {
    target = tstr_cat(target, "?");
    if (target && uri.query[0]) target = tstr_cat(target, uri.query);
  }
  if (!target) return -1;
  endpoint->target = target;
  return 0;
}

static void turbo_mcp_http_endpoint_destroy(turbo_mcp_http_endpoint_t *endpoint) {
  if (!endpoint) return;
  tstr_free(endpoint->target);
  endpoint->target = NULL;
}

static chttp_client_config turbo_mcp_http_config(const turbo_mcp_client_config_t *config) {
  const cnet_client_config network = {
      .backend = turbo_mcp_http_backend(),
      .connection_capacity = 2u,
      .command_capacity = 32u,
      .request_capacity = 32u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = config->max_request_bytes + config->max_header_bytes + 8192u,
      .receive_buffer_bytes = 128u * 1024u,
      .connect_timeout_ms = (uint32_t)config->timeout_ms,
      .read_timeout_ms = (uint32_t)config->timeout_ms,
      .write_timeout_ms = (uint32_t)config->timeout_ms,
      .tls_io_buffer_bytes = 64u * 1024u,
      .tls_handshake_timeout_ms = (uint32_t)config->timeout_ms};
  return (chttp_client_config){
      .network = network,
      .request_capacity = 2u,
      .max_start_line_bytes = 8192u,
      .max_header_count = config->max_headers + 1u,
      .max_header_bytes = config->max_header_bytes + 4096u,
      .max_request_body_bytes = config->max_request_bytes,
      .max_response_body_bytes = config->max_response_bytes,
      .max_informational_responses = 4u,
      .stream_chunk_bytes = 64u * 1024u,
      .h2_input_buffer_bytes = 128u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static void turbo_mcp_http_response_release(void *context) {
  turbo_mcp_http_response_owner_t *owner = (turbo_mcp_http_response_owner_t *)context;
  if (!owner) return;
  chttp_response_destroy(&owner->response);
  free(owner);
}

void turbo_mcp_client_set_error(turbo_mcp_client_t *client, const char *message) {
  if (!client) return;
  if (!message) message = "MCP operation failed";
  snprintf(client->last_error, sizeof(client->last_error), "%s", message);
}

const char *turbo_mcp_client_last_error(const turbo_mcp_client_t *client) {
  return client ? client->last_error : "invalid MCP client";
}

static int turbo_mcp_raw_headers(const char *const *headers, size_t header_count,
                                 chttp_header *parsed, char **copies) {
  size_t index;
  for (index = 0u; index < header_count; ++index) {
    char *colon;
    char *value;
    if (!headers[index]) return -1;
    copies[index] = (char *)malloc(strlen(headers[index]) + 1u);
    if (copies[index]) memcpy(copies[index], headers[index], strlen(headers[index]) + 1u);
    if (!copies[index]) return -1;
    colon = strchr(copies[index], ':');
    if (!colon || colon == copies[index]) return -1;
    *colon = '\0';
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    parsed[index] = (chttp_header){.name = copies[index], .value = value};
  }
  return 0;
}

static void turbo_mcp_raw_headers_destroy(char **copies, size_t count) {
  size_t index;
  if (!copies) return;
  for (index = 0u; index < count; ++index) free(copies[index]);
}

static int turbo_mcp_default_post(void *user_data, const char *endpoint,
                                  const char *const *headers, size_t header_count,
                                  const uint8_t *body, size_t body_len,
                                  turbo_mcp_transport_response_t *out_response) {
  turbo_mcp_client_t *client = (turbo_mcp_client_t *)user_data;
  turbo_mcp_http_response_owner_t *owner = NULL;
  turbo_mcp_http_endpoint_t target;
  chttp_header *parsed = NULL;
  char **copies = NULL;
  chttp_options options;
  chttp_error error = {0};
  size_t parsed_count = header_count;
  int status = SALTS_EINVAL;

  if (!client || !client->http_initialized || !endpoint || !body || !out_response ||
      header_count > client->max_headers) {
    return -1;
  }
  memset(out_response, 0, sizeof(*out_response));
  memset(&target, 0, sizeof(target));
  if (turbo_mcp_http_endpoint_build(endpoint, &target) != 0) {
    turbo_mcp_client_set_error(client, "invalid MCP endpoint URL");
    return -1;
  }

  parsed = (chttp_header *)calloc(header_count + (client->bearer_authorization ? 1u : 0u),
                                  sizeof(*parsed));
  copies = (char **)calloc(header_count ? header_count : 1u, sizeof(*copies));
  owner = (turbo_mcp_http_response_owner_t *)calloc(1u, sizeof(*owner));
  if (!parsed || !copies || !owner ||
      turbo_mcp_raw_headers(headers, header_count, parsed, copies) != 0) {
    turbo_mcp_client_set_error(client, "out of memory preparing MCP HTTP request");
    goto fail;
  }

  if (client->bearer_authorization) {
    parsed[parsed_count++] =
        (chttp_header){.name = "Authorization", .value = client->bearer_authorization};
  }

  options = (chttp_options){
      .connection_uri = target.connection_uri,
      .authority = target.authority,
      .target = target.target,
      .headers = parsed,
      .header_count = parsed_count,
      .body = body,
      .body_size = body_len,
      .timeout_ms = client->timeout_ms,
      .protocol = CHTTP_HTTP_1_1};
  status = chttp_post(&client->http, &options, &owner->response, &error);
  if (status != SALTS_OK) {
    char message[TURBO_MCP_ERROR_MESSAGE_BYTES];
    snprintf(message, sizeof(message), "CHTTP MCP request failed: status=%d native=%d stage=%s",
             error.status, error.native_status, error.stage ? error.stage : "(none)");
    turbo_mcp_client_set_error(client, message);
    goto fail;
  }

  out_response->status_code = (int)owner->response.status_code;
  out_response->content_type = chttp_response_header(&owner->response, "Content-Type");
  out_response->body = (const uint8_t *)owner->response.body;
  out_response->body_len = owner->response.body_size;
  out_response->release_context = owner;
  out_response->release = turbo_mcp_http_response_release;
  owner = NULL;
  status = SALTS_OK;

fail:
  if (owner) turbo_mcp_http_response_release(owner);
  turbo_mcp_raw_headers_destroy(copies, header_count);
  free(copies);
  free(parsed);
  turbo_mcp_http_endpoint_destroy(&target);
  return status == SALTS_OK ? 0 : -1;
}

turbo_mcp_client_t *turbo_mcp_client_create(const turbo_mcp_client_config_t *config) {
  turbo_mcp_client_t *client;
  chttp_client_config http_config;

  if (!config || !config->endpoint || !config->endpoint[0] || !config->client_name ||
      !config->client_name[0] || !config->client_version || !config->client_version[0] ||
      !config->max_request_bytes || !config->max_response_bytes || !config->max_headers ||
      !config->max_header_bytes || config->timeout_ms <= 0 ||
      config->timeout_ms > (int64_t)UINT32_MAX ||
      config->max_request_bytes > SIZE_MAX - config->max_header_bytes - 8192u) {
    return NULL;
  }
  client = (turbo_mcp_client_t *)calloc(1, sizeof(*client));
  if (!client) return NULL;
  client->endpoint = tstr_dup(config->endpoint);
  client->client_name = tstr_dup(config->client_name);
  client->client_version = tstr_dup(config->client_version);
  if (!client->endpoint || !client->client_name || !client->client_version) {
    turbo_mcp_client_destroy(client);
    return NULL;
  }
  if (config->bearer_token && config->bearer_token[0]) {
    client->bearer_authorization = tstr_dup("Bearer ");
    if (client->bearer_authorization)
      client->bearer_authorization = tstr_cat(client->bearer_authorization, config->bearer_token);
    if (!client->bearer_authorization) {
      turbo_mcp_client_destroy(client);
      return NULL;
    }
  }

  client->max_request_bytes = config->max_request_bytes;
  client->max_response_bytes = config->max_response_bytes;
  client->max_headers = config->max_headers;
  client->max_header_bytes = config->max_header_bytes;
  client->next_request_id = 1u;
  client->timeout_ms = (uint32_t)config->timeout_ms;
  client->transport_post = config->transport_post;
  client->transport_user_data = config->transport_user_data;
  if (client->transport_post) return client;

  http_config = turbo_mcp_http_config(config);
  if (http_config.network.backend == (native_io_backend_kind)0 ||
      chttp_client_init(&client->http, &http_config) != SALTS_OK) {
    turbo_mcp_client_destroy(client);
    return NULL;
  }
  client->http_initialized = 1;
  client->transport_post = turbo_mcp_default_post;
  client->transport_user_data = client;
  return client;
}

void turbo_mcp_client_destroy(turbo_mcp_client_t *client) {
  if (!client) return;
  if (client->http_initialized) {
    (void)chttp_client_destroy(&client->http, 0u);
    client->http_initialized = 0;
  }
  tstr_free(client->bearer_authorization);
  tstr_free(client->endpoint);
  tstr_free(client->client_name);
  tstr_free(client->client_version);
  free(client);
}

static int turbo_mcp_json_add_owned(json_value_t *object, const char *key,
                                    json_value_t **value) {
  if (!object || !key || !value || !*value ||
      !turbo_json_object_add_checked(object, key, *value)) {
    return -1;
  }
  *value = NULL;
  return 0;
}

static int turbo_mcp_add_request_meta(turbo_mcp_client_t *client, json_value_t *params) {
  json_value_t *meta = NULL;
  json_value_t *info = NULL;
  json_value_t *capabilities = NULL;
  json_value_t *value = NULL;

  if (!client || !params || turbo_json_type(params) != TURBO_JSON_OBJECT ||
      turbo_json_object_get(params, "_meta")) {
    return -1;
  }
  meta = turbo_json_create_object();
  info = turbo_json_create_object();
  capabilities = turbo_json_create_object();
  value = turbo_json_create_string(TURBO_MCP_PROTOCOL_VERSION);
  if (!meta || !info || !capabilities || !value ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/protocolVersion", &value) != 0) {
    goto fail;
  }
  value = turbo_json_create_string(client->client_name);
  if (!value || turbo_mcp_json_add_owned(info, "name", &value) != 0) goto fail;
  value = turbo_json_create_string(client->client_version);
  if (!value || turbo_mcp_json_add_owned(info, "version", &value) != 0 ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/clientInfo", &info) != 0 ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/clientCapabilities",
                               &capabilities) != 0 ||
      turbo_mcp_json_add_owned(params, "_meta", &meta) != 0) {
    goto fail;
  }
  return 0;

fail:
  turbo_free_json(&value);
  turbo_free_json(&meta);
  turbo_free_json(&info);
  turbo_free_json(&capabilities);
  return -1;
}

static int turbo_mcp_header_plain_safe(const char *value, size_t length) {
  size_t index;
  if (!value || (length > 0 && (value[0] == ' ' || value[0] == '\t' ||
                               value[length - 1] == ' ' || value[length - 1] == '\t'))) {
    return 0;
  }
  if (length >= 11 && !memcmp(value, "=?base64?", 9) &&
      !memcmp(value + length - 2, "?=", 2)) {
    return 0;
  }
  for (index = 0; index < length; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (!((byte >= 0x21 && byte <= 0x7e) || byte == 0x20 || byte == 0x09)) return 0;
  }
  return 1;
}

static tstr_t turbo_mcp_encode_header_value(const char *value, size_t length) {
  char *encoded = NULL;
  tstr_t result;
  if (turbo_mcp_header_plain_safe(value, length)) return tstr_new_len(value, length);
  if (tn_base64_encode((const uint8_t *)value, length, &encoded) != 0 || !encoded) return NULL;
  result = tstr_dup("=?base64?");
  if (result) result = tstr_cat(result, encoded);
  if (result) result = tstr_cat(result, "?=");
  free(encoded);
  return result;
}

tstr_t turbo_mcp_format_header(const char *name, const char *value, size_t value_length) {
  tstr_t encoded;
  tstr_t header;
  if (!name || !name[0] || !value) return NULL;
  encoded = turbo_mcp_encode_header_value(value, value_length);
  if (!encoded) return NULL;
  header = tstr_dup(name);
  if (header) header = tstr_cat(header, ": ");
  if (header) header = tstr_cat_str(header, encoded);
  tstr_free(encoded);
  return header;
}

static int turbo_mcp_content_type_is(const char *content_type, const char *expected) {
  size_t length = strlen(expected);
  if (!content_type || tstr_ncasecmp(content_type, expected, length) != 0) return 0;
  return content_type[length] == '\0' || content_type[length] == ';' ||
         content_type[length] == ' ' || content_type[length] == '\t';
}

static int turbo_mcp_response_matches(const json_value_t *root, uint64_t request_id,
                                      json_value_t **out_result,
                                      turbo_mcp_client_t *client) {
  json_value_t *id;
  json_value_t *result;
  json_value_t *error;
  json_value_t *message;
  const char *version;
  size_t number_length = 0;
  const char *number_text;
  char expected[32];

  if (!root || turbo_json_type(root) != TURBO_JSON_OBJECT) return 0;
  version = turbo_json_get_string(root, "jsonrpc");
  id = turbo_json_object_get(root, "id");
  if (!version || strcmp(version, "2.0") != 0 || !id ||
      turbo_json_type(id) != TURBO_JSON_NUMBER) {
    return 0;
  }
  number_text = turbo_json_number_text(id, &number_length);
  snprintf(expected, sizeof(expected), "%llu", (unsigned long long)request_id);
  if (!number_text || strlen(expected) != number_length ||
      memcmp(number_text, expected, number_length) != 0) {
    return 0;
  }
  error = turbo_json_object_get(root, "error");
  result = turbo_json_object_get(root, "result");
  if (error) {
    message = turbo_json_type(error) == TURBO_JSON_OBJECT
                  ? turbo_json_object_get(error, "message")
                  : NULL;
    turbo_mcp_client_set_error(
        client, message && turbo_json_type(message) == TURBO_JSON_STRING
                    ? turbo_json_string(message)
                    : "MCP JSON-RPC error");
    return -1;
  }
  if (!result) {
    turbo_mcp_client_set_error(client, "MCP JSON-RPC response has neither result nor error");
    return -1;
  }
  *out_result = turbo_json_clone(result);
  if (!*out_result) {
    turbo_mcp_client_set_error(client, "out of memory cloning MCP result");
    return -1;
  }
  return 1;
}

static turbo_tool_status_t turbo_mcp_parse_json_response(
    turbo_mcp_client_t *client, const uint8_t *body, size_t body_len,
    uint64_t request_id, json_value_t **out_result) {
  json_value_t *root = NULL;
  int match;
  if (!body || !body_len || turbo_parse_json(body, body_len, &root) != 0 || !root) {
    turbo_mcp_client_set_error(client, "invalid MCP JSON response");
    turbo_free_json(&root);
    return TURBO_TOOL_ERROR;
  }
  match = turbo_mcp_response_matches(root, request_id, out_result, client);
  turbo_free_json(&root);
  if (match == 1) return TURBO_TOOL_OK;
  if (match == 0) turbo_mcp_client_set_error(client, "MCP JSON-RPC response id mismatch");
  return TURBO_TOOL_ERROR;
}

static turbo_tool_status_t turbo_mcp_parse_sse_response(
    turbo_mcp_client_t *client, const uint8_t *body, size_t body_len,
    uint64_t request_id, json_value_t **out_result) {
  size_t offset = 0;
  tstr_t event_data = tstr_new();
  turbo_tool_status_t status = TURBO_TOOL_ERROR;
  if (!event_data) return TURBO_TOOL_OUT_OF_MEMORY;

  while (offset <= body_len) {
    size_t line_start = offset;
    size_t line_len;
    while (offset < body_len && body[offset] != '\n') ++offset;
    line_len = offset - line_start;
    if (line_len && body[line_start + line_len - 1] == '\r') --line_len;
    if (offset < body_len) ++offset;

    if (line_len == 0 || offset > body_len) {
      if (tstr_len(event_data)) {
        json_value_t *event = NULL;
        if (turbo_parse_json((const uint8_t *)event_data, tstr_len(event_data), &event) == 0 &&
            event) {
          int match = turbo_mcp_response_matches(event, request_id, out_result, client);
          turbo_free_json(&event);
          if (match == 1) {
            status = TURBO_TOOL_OK;
            break;
          }
          if (match < 0) break;
        }
        tstr_clear(event_data);
      }
      if (offset >= body_len) break;
      continue;
    }
    if (line_len >= 5 && !memcmp(body + line_start, "data:", 5)) {
      size_t data_start = line_start + 5;
      size_t data_len = line_len - 5;
      if (data_len && body[data_start] == ' ') {
        ++data_start;
        --data_len;
      }
      if (tstr_len(event_data)) event_data = tstr_cat(event_data, "\n");
      if (event_data) event_data = tstr_cat_len(event_data, (const char *)body + data_start,
                                                data_len);
      if (!event_data || tstr_len(event_data) > client->max_response_bytes) {
        status = TURBO_TOOL_OUTPUT_LIMIT;
        turbo_mcp_client_set_error(client, "MCP SSE event exceeds response limit");
        break;
      }
    }
  }
  if (status != TURBO_TOOL_OK && !client->last_error[0])
    turbo_mcp_client_set_error(client, "MCP SSE stream contained no matching response");
  tstr_free(event_data);
  return status;
}

turbo_tool_status_t turbo_mcp_client_request(
    turbo_mcp_client_t *client, const char *method, const char *name,
    json_value_t *params, const char *const *parameter_headers,
    size_t parameter_header_count, json_value_t **out_result) {
  json_value_t *request = NULL;
  json_value_t *field = NULL;
  char *request_json = NULL;
  size_t request_json_len = 0;
  uint64_t request_id;
  tstr_t method_header = NULL;
  tstr_t name_value = NULL;
  tstr_t name_header = NULL;
  const char **headers = NULL;
  size_t header_count = 0;
  size_t header_bytes = 0;
  size_t index;
  turbo_mcp_transport_response_t response;
  turbo_tool_status_t status = TURBO_TOOL_ERROR;

  if (!out_result) {
    turbo_free_json(&params);
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  *out_result = NULL;
  if (!client || !method || !method[0] || !params ||
      turbo_json_type(params) != TURBO_JSON_OBJECT ||
      (parameter_header_count && !parameter_headers) ||
      parameter_header_count > client->max_headers - 4) {
    turbo_free_json(&params);
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  client->last_error[0] = '\0';
  if (client->next_request_id == 0 || client->next_request_id > (uint64_t)INT64_MAX) {
    turbo_free_json(&params);
    turbo_mcp_client_set_error(client, "MCP request id space exhausted");
    return TURBO_TOOL_ERROR;
  }
  request_id = client->next_request_id++;
  if (turbo_mcp_add_request_meta(client, params) != 0) {
    turbo_free_json(&params);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  request = turbo_json_create_object();
  field = turbo_json_create_string("2.0");
  if (!request || !field || turbo_mcp_json_add_owned(request, "jsonrpc", &field) != 0) {
    goto request_oom;
  }
  field = turbo_json_create_uint64(request_id);
  if (!field || turbo_mcp_json_add_owned(request, "id", &field) != 0) goto request_oom;
  field = turbo_json_create_string(method);
  if (!field || turbo_mcp_json_add_owned(request, "method", &field) != 0 ||
      turbo_mcp_json_add_owned(request, "params", &params) != 0) {
    goto request_oom;
  }
  request_json = turbo_json_serialize(request, &request_json_len);
  turbo_free_json(&request);
  if (!request_json) return TURBO_TOOL_OUT_OF_MEMORY;
  if (request_json_len > client->max_request_bytes) {
    turbo_json_serialize_free(request_json);
    turbo_mcp_client_set_error(client, "MCP request exceeds configured byte limit");
    return TURBO_TOOL_OUTPUT_LIMIT;
  }

  headers = (const char **)calloc(4 + parameter_header_count, sizeof(*headers));
  method_header = tstr_dup("Mcp-Method: ");
  if (method_header) method_header = tstr_cat(method_header, method);
  if (!headers || !method_header) {
    status = TURBO_TOOL_OUT_OF_MEMORY;
    goto cleanup;
  }
  headers[header_count++] = "Content-Type: application/json";
  headers[header_count++] = "Accept: application/json, text/event-stream";
  headers[header_count++] = "MCP-Protocol-Version: " TURBO_MCP_PROTOCOL_VERSION;
  headers[header_count++] = method_header;
  if (name) {
    const size_t name_len = strlen(name);
    name_value = turbo_mcp_encode_header_value(name, name_len);
    name_header = tstr_dup("Mcp-Name: ");
    if (name_header && name_value) name_header = tstr_cat_str(name_header, name_value);
    if (!name_value || !name_header) {
      status = TURBO_TOOL_OUT_OF_MEMORY;
      goto cleanup;
    }
    {
      const char **resized = (const char **)realloc(
          headers, (5 + parameter_header_count) * sizeof(*headers));
      if (!resized) {
        status = TURBO_TOOL_OUT_OF_MEMORY;
        goto cleanup;
      }
      headers = resized;
    }
    headers[header_count++] = name_header;
  }
  for (index = 0; index < parameter_header_count; ++index)
    headers[header_count++] = parameter_headers[index];
  if (header_count > client->max_headers) {
    status = TURBO_TOOL_OUTPUT_LIMIT;
    turbo_mcp_client_set_error(client, "MCP request exceeds header count limit");
    goto cleanup;
  }
  for (index = 0; index < header_count; ++index) {
    size_t length = strlen(headers[index]);
    if (header_bytes > client->max_header_bytes ||
        length > client->max_header_bytes - header_bytes) {
      status = TURBO_TOOL_OUTPUT_LIMIT;
      turbo_mcp_client_set_error(client, "MCP request exceeds header byte limit");
      goto cleanup;
    }
    header_bytes += length;
  }

  memset(&response, 0, sizeof(response));
  if (client->transport_post(client->transport_user_data, client->endpoint, headers,
                             header_count, (const uint8_t *)request_json, request_json_len,
                             &response) != 0) {
    if (!client->last_error[0]) turbo_mcp_client_set_error(client, "MCP transport failed");
    goto cleanup;
  }
  if (response.body_len > client->max_response_bytes) {
    status = TURBO_TOOL_OUTPUT_LIMIT;
    turbo_mcp_client_set_error(client, "MCP response exceeds configured byte limit");
  } else if (response.status_code < 200 || response.status_code >= 300) {
    turbo_mcp_client_set_error(client, response.status_code == 401 || response.status_code == 403
                                           ? "MCP authorization failed"
                                           : "MCP endpoint returned non-success HTTP status");
  } else if (turbo_mcp_content_type_is(response.content_type, "application/json")) {
    status = turbo_mcp_parse_json_response(client, response.body, response.body_len,
                                           request_id, out_result);
  } else if (turbo_mcp_content_type_is(response.content_type, "text/event-stream")) {
    status = turbo_mcp_parse_sse_response(client, response.body, response.body_len,
                                          request_id, out_result);
  } else {
    turbo_mcp_client_set_error(client, "MCP response has unsupported Content-Type");
  }
  if (response.release) response.release(response.release_context);

cleanup:
  free(headers);
  tstr_free(method_header);
  tstr_free(name_value);
  tstr_free(name_header);
  turbo_json_serialize_free(request_json);
  return status;

request_oom:
  turbo_free_json(&field);
  turbo_free_json(&request);
  turbo_free_json(&params);
  return TURBO_TOOL_OUT_OF_MEMORY;
}
