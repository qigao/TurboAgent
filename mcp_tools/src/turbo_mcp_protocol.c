#include "turbo_mcp_protocol_internal.h"

#include "base64_utils.h"
#include "turbo_runtime_json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TURBO_MCP_DEFAULT_TIMEOUT_MS = 10000,
  TURBO_MCP_ERROR_MESSAGE_BYTES = 512
};

typedef struct turbo_mcp_http_response_owner_s {
  http_response_t *response;
  char *content_type;
} turbo_mcp_http_response_owner_t;

struct turbo_mcp_client_s {
  tstr_t endpoint;
  tstr_t client_name;
  tstr_t client_version;
  turbo_http_t *http;
  turbo_mcp_transport_post_fn transport_post;
  void *transport_user_data;
  size_t max_request_bytes;
  size_t max_response_bytes;
  size_t max_headers;
  size_t max_header_bytes;
  uint64_t next_request_id;
  char last_error[TURBO_MCP_ERROR_MESSAGE_BYTES];
};

static void turbo_mcp_http_response_release(void *context) {
  turbo_mcp_http_response_owner_t *owner = (turbo_mcp_http_response_owner_t *)context;
  if (!owner) return;
  free(owner->content_type);
  http_response_free(owner->response);
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

static int turbo_mcp_default_post(void *user_data, const char *endpoint,
                                  const char *const *headers, size_t header_count,
                                  const uint8_t *body, size_t body_len,
                                  turbo_mcp_transport_response_t *out_response) {
  turbo_mcp_client_t *client = (turbo_mcp_client_t *)user_data;
  turbo_mcp_http_response_owner_t *owner;
  http_response_t *response;

  if (!client || !client->http || !endpoint || !body || !out_response ||
      header_count > (size_t)INT_MAX) {
    return -1;
  }
  memset(out_response, 0, sizeof(*out_response));
  response = turbo_http_request_sync(client->http, HTTP_POST, endpoint,
                                     (const char **)headers,
                                     (int)header_count, (const char *)body, body_len);
  if (!response) {
    turbo_mcp_client_set_error(client, "TurboHTTP could not allocate a response");
    return -1;
  }
  if (response->error_code != HTTP_ERROR_NONE) {
    turbo_mcp_client_set_error(client, response->error ? response->error
                                                       : http_error_to_str(response->error_code));
    http_response_free(response);
    return -1;
  }

  owner = (turbo_mcp_http_response_owner_t *)calloc(1, sizeof(*owner));
  if (!owner) {
    http_response_free(response);
    turbo_mcp_client_set_error(client, "out of memory retaining MCP response");
    return -1;
  }
  owner->response = response;
  owner->content_type = http_response_get_header(response, "Content-Type");
  out_response->status_code = response->status_code;
  out_response->content_type = owner->content_type;
  out_response->body = (const uint8_t *)response->body;
  out_response->body_len = response->body_len;
  out_response->release_context = owner;
  out_response->release = turbo_mcp_http_response_release;
  return 0;
}

turbo_mcp_client_t *turbo_mcp_client_create(const turbo_mcp_client_config_t *config) {
  turbo_mcp_client_t *client;
  turbo_http_options_t options;

  if (!config || !config->endpoint || !config->endpoint[0] || !config->client_name ||
      !config->client_name[0] || !config->client_version || !config->client_version[0] ||
      !config->max_request_bytes || !config->max_response_bytes || !config->max_headers ||
      !config->max_header_bytes || config->timeout_ms <= 0) {
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
  client->max_request_bytes = config->max_request_bytes;
  client->max_response_bytes = config->max_response_bytes;
  client->max_headers = config->max_headers;
  client->max_header_bytes = config->max_header_bytes;
  client->next_request_id = 1;
  client->transport_post = config->transport_post;
  client->transport_user_data = config->transport_user_data;
  if (client->transport_post) return client;

  memset(&options, 0, sizeof(options));
  if (turbo_http_options_init(&options, sizeof(options)) != TURBO_OK) {
    turbo_mcp_client_destroy(client);
    return NULL;
  }
  options.timeout_ms = config->timeout_ms ? config->timeout_ms : TURBO_MCP_DEFAULT_TIMEOUT_MS;
  options.follow_redirects = 0;
  if (turbo_http_create_sync(&options, &client->http) != TURBO_OK || !client->http) {
    turbo_mcp_client_destroy(client);
    return NULL;
  }
  turbo_http_set_max_response_size(client->http, client->max_response_bytes);
  turbo_http_set_max_response_header_size(client->http, client->max_header_bytes);
  if (config->bearer_token && config->bearer_token[0] &&
      turbo_http_set_bearer_token(client->http, config->bearer_token) != TURBO_OK) {
    turbo_mcp_client_destroy(client);
    return NULL;
  }
  client->transport_post = turbo_mcp_default_post;
  client->transport_user_data = client;
  return client;
}

void turbo_mcp_client_destroy(turbo_mcp_client_t *client) {
  if (!client) return;
  turbo_http_destroy(client->http);
  tstr_free(client->endpoint);
  tstr_free(client->client_name);
  tstr_free(client->client_version);
  free(client);
}

static int turbo_mcp_json_add_owned(json_value_t *object, const char *key,
                                    json_value_t **value) {
  if (!object || !key || !value || !*value ||
      !json_object_add_checked(object, key, *value)) {
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

  if (!client || !params || json_type(params) != JSON_OBJECT ||
      json_object_get(params, "_meta")) {
    return -1;
  }
  meta = json_create_object();
  info = json_create_object();
  capabilities = json_create_object();
  value = json_create_string(TURBO_MCP_PROTOCOL_VERSION);
  if (!meta || !info || !capabilities || !value ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/protocolVersion", &value) != 0) {
    goto fail;
  }
  value = json_create_string(client->client_name);
  if (!value || turbo_mcp_json_add_owned(info, "name", &value) != 0) goto fail;
  value = json_create_string(client->client_version);
  if (!value || turbo_mcp_json_add_owned(info, "version", &value) != 0 ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/clientInfo", &info) != 0 ||
      turbo_mcp_json_add_owned(meta, "io.modelcontextprotocol/clientCapabilities",
                               &capabilities) != 0 ||
      turbo_mcp_json_add_owned(params, "_meta", &meta) != 0) {
    goto fail;
  }
  return 0;

fail:
  turbo_runtime_json_reset(&value);
  turbo_runtime_json_reset(&meta);
  turbo_runtime_json_reset(&info);
  turbo_runtime_json_reset(&capabilities);
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

  if (!root || json_type(root) != JSON_OBJECT) return 0;
  version = json_get_string(root, "jsonrpc");
  id = json_object_get(root, "id");
  if (!version || strcmp(version, "2.0") != 0 || !id ||
      json_type(id) != JSON_NUMBER) {
    return 0;
  }
  number_text = json_number_text(id, &number_length);
  snprintf(expected, sizeof(expected), "%llu", (unsigned long long)request_id);
  if (!number_text || strlen(expected) != number_length ||
      memcmp(number_text, expected, number_length) != 0) {
    return 0;
  }
  error = json_object_get(root, "error");
  result = json_object_get(root, "result");
  if (error) {
    message = json_type(error) == JSON_OBJECT
                  ? json_object_get(error, "message")
                  : NULL;
    turbo_mcp_client_set_error(
        client, message && json_type(message) == JSON_STRING
                    ? json_string(message)
                    : "MCP JSON-RPC error");
    return -1;
  }
  if (!result) {
    turbo_mcp_client_set_error(client, "MCP JSON-RPC response has neither result nor error");
    return -1;
  }
  *out_result = json_clone(result);
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
  if (!body || !body_len || turbo_runtime_json_parse(body, body_len, &root) != 0 || !root) {
    turbo_mcp_client_set_error(client, "invalid MCP JSON response");
    turbo_runtime_json_reset(&root);
    return TURBO_TOOL_ERROR;
  }
  match = turbo_mcp_response_matches(root, request_id, out_result, client);
  turbo_runtime_json_reset(&root);
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
        if (turbo_runtime_json_parse((const uint8_t *)event_data, tstr_len(event_data), &event) == 0 &&
            event) {
          int match = turbo_mcp_response_matches(event, request_id, out_result, client);
          turbo_runtime_json_reset(&event);
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
    turbo_runtime_json_reset(&params);
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  *out_result = NULL;
  if (!client || !method || !method[0] || !params ||
      json_type(params) != JSON_OBJECT ||
      (parameter_header_count && !parameter_headers) ||
      parameter_header_count > client->max_headers - 4) {
    turbo_runtime_json_reset(&params);
    return TURBO_TOOL_INVALID_ARGUMENT;
  }
  client->last_error[0] = '\0';
  if (client->next_request_id == 0 || client->next_request_id > (uint64_t)INT64_MAX) {
    turbo_runtime_json_reset(&params);
    turbo_mcp_client_set_error(client, "MCP request id space exhausted");
    return TURBO_TOOL_ERROR;
  }
  request_id = client->next_request_id++;
  if (turbo_mcp_add_request_meta(client, params) != 0) {
    turbo_runtime_json_reset(&params);
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  request = json_create_object();
  field = json_create_string("2.0");
  if (!request || !field || turbo_mcp_json_add_owned(request, "jsonrpc", &field) != 0) {
    goto request_oom;
  }
  field = json_create_uint64(request_id);
  if (!field || turbo_mcp_json_add_owned(request, "id", &field) != 0) goto request_oom;
  field = json_create_string(method);
  if (!field || turbo_mcp_json_add_owned(request, "method", &field) != 0 ||
      turbo_mcp_json_add_owned(request, "params", &params) != 0) {
    goto request_oom;
  }
  request_json = json_serialize(request, &request_json_len);
  turbo_runtime_json_reset(&request);
  if (!request_json) return TURBO_TOOL_OUT_OF_MEMORY;
  if (request_json_len > client->max_request_bytes) {
    json_serialize_free(request_json);
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
  json_serialize_free(request_json);
  return status;

request_oom:
  turbo_runtime_json_reset(&field);
  turbo_runtime_json_reset(&request);
  turbo_runtime_json_reset(&params);
  return TURBO_TOOL_OUT_OF_MEMORY;
}
