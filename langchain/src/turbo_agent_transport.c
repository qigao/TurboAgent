#include "turbo_agent_core_internal.h"
#include "turbo_agent_lifecycle_internal.h"
#include "turbo_agent_transport_internal.h"
#include "turbo_agent_sse.h"

#include "turbo_model_provider.h"

#include <http_client/http.h>
#include <uri_parser.h>
#include <tstr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_MODEL_HTTP_TIMEOUT_MS 60000u

typedef struct turbo_agent_http_endpoint_s {
  char connection_uri[640];
  char authority[320];
  char *target;
} turbo_agent_http_endpoint_t;

static char *turbo_agent_transport_copy_bytes(const void *data, size_t size) {
  char *copy;
  if (!data && size != 0u) return NULL;
  copy = (char *)malloc(size + 1u);
  if (!copy) return NULL;
  if (size != 0u) memcpy(copy, data, size);
  copy[size] = '\0';
  return copy;
}

static char *turbo_agent_transport_strdup_malloc(const char *text) {
  return text ? turbo_agent_transport_copy_bytes(text, strlen(text)) : NULL;
}

static int turbo_agent_capture_last_stream_sse(turbo_agent_t *agent, const void *data,
                                               size_t len) {
  char *copy;
  if (!agent || (!data && len != 0u)) return -1;
  copy = turbo_agent_transport_copy_bytes(data, len);
  if (!copy) return -1;
  turbo_agent_clear_last_stream_sse(agent);
  agent->last_stream_sse = copy;
  agent->last_stream_sse_len = len;
  return 0;
}

static int turbo_agent_provider_sse_to_response_json(const turbo_model_provider_t *provider,
                                                     const char *sse_data, size_t sse_len,
                                                     char **out_response_json) {
  if (!provider || !sse_data || !out_response_json) return -1;
  *out_response_json = NULL;
  if (provider == turbo_model_provider_openai_responses()) {
    return turbo_agent_responses_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_legacy_chat(provider)) {
    return turbo_agent_chat_sse_to_json(sse_data, sse_len, out_response_json);
  }
  if (turbo_model_provider_is_anthropic_messages(provider)) {
    return turbo_agent_anthropic_messages_sse_to_json(sse_data, sse_len, out_response_json);
  }
  return -1;
}

static int turbo_agent_http_endpoint_build(const turbo_agent_t *agent,
                                           turbo_agent_http_endpoint_t *endpoint) {
  uri_t uri;
  const char *transport_scheme;
  const int explicit_port_mask = URI_COMPONENT_PORT;
  int default_port;
  int port;
  int is_ipv6;
  int written;
  char *target;

  if (!agent || !agent->base_url || !agent->endpoint_path || !endpoint) return -1;
  memset(endpoint, 0, sizeof(*endpoint));
  memset(&uri, 0, sizeof(uri));
  if (!uri_parse(agent->base_url, &uri) || !uri.valid || !uri.host[0]) return -1;
  if ((uri.component_flags &
       (URI_COMPONENT_USERINFO | URI_COMPONENT_QUERY | URI_COMPONENT_FRAGMENT)) != 0u ||
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

  port = (uri.component_flags & explicit_port_mask) != 0u ? uri.port : default_port;
  if (port <= 0 || port > 65535) return -1;
  is_ipv6 = uri.host_type == URI_HOST_IPV6ADDR;

  written = is_ipv6
                ? snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                           "%s://[%s]:%d", transport_scheme, uri.host, port)
                : snprintf(endpoint->connection_uri, sizeof(endpoint->connection_uri),
                           "%s://%s:%d", transport_scheme, uri.host, port);
  if (written < 0 || (size_t)written >= sizeof(endpoint->connection_uri)) return -1;

  if ((uri.component_flags & explicit_port_mask) != 0u) {
    written = is_ipv6 ? snprintf(endpoint->authority, sizeof(endpoint->authority), "[%s]:%d",
                                 uri.host, port)
                      : snprintf(endpoint->authority, sizeof(endpoint->authority), "%s:%d",
                                 uri.host, port);
  } else {
    written = is_ipv6 ? snprintf(endpoint->authority, sizeof(endpoint->authority), "[%s]", uri.host)
                      : snprintf(endpoint->authority, sizeof(endpoint->authority), "%s", uri.host);
  }
  if (written < 0 || (size_t)written >= sizeof(endpoint->authority)) return -1;

  target = tstr_new();
  if (!target) return -1;
  if (uri.path[0]) {
    target = tstr_cat(target, uri.path);
  } else {
    target = tstr_cat(target, "/");
  }
  if (!target) return -1;
  if (tstr_len(target) == 0u || target[tstr_len(target) - 1u] != '/') {
    target = tstr_cat(target, "/");
  }
  if (target) target = tstr_cat(target, agent->endpoint_path);
  if (!target || target[0] != '/') {
    tstr_free(target);
    return -1;
  }
  endpoint->target = target;
  return 0;
}

static void turbo_agent_http_endpoint_destroy(turbo_agent_http_endpoint_t *endpoint) {
  if (!endpoint) return;
  tstr_free(endpoint->target);
  endpoint->target = NULL;
}

CXX_C_API int turbo_agent_build_http_headers_openai(const turbo_agent_t *agent,
                                                     chttp_header *headers,
                                                     size_t capacity,
                                                     size_t *out_count) {
  size_t count = 0u;
  if (!headers || !out_count) return -1;
  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    if (capacity < 1u || !agent->http_authorization) return -1;
    headers[count++] =
        (chttp_header){.name = "Authorization", .value = agent->http_authorization};
  }
  *out_count = count;
  return 0;
}

CXX_C_API int turbo_agent_build_http_headers_anthropic(const turbo_agent_t *agent,
                                                       chttp_header *headers,
                                                       size_t capacity,
                                                       size_t *out_count) {
  size_t count = 0u;
  if (!headers || !out_count) return -1;
  if (capacity < 1u) return -1;
  headers[count++] =
      (chttp_header){.name = "anthropic-version", .value = "2023-06-01"};
  if (agent && agent->api_key && agent->api_key[0] != '\0') {
    if (capacity <= count) return -1;
    headers[count++] = (chttp_header){.name = "x-api-key", .value = agent->api_key};
  }
  *out_count = count;
  return 0;
}

static char *turbo_agent_http_transport_error_detail(int call_status,
                                                     const chttp_response *response,
                                                     const chttp_error *error) {
  const char *stage = error && error->stage ? error->stage : "(none)";
  const char *body = response && response->body ? (const char *)response->body : NULL;
  size_t body_size = response ? response->body_size : 0u;
  size_t preview = body_size > 240u ? 240u : body_size;
  int needed;
  char *buffer;

  needed = snprintf(NULL, 0,
                    "http transport failed: call_status=%d http_status=%u "
                    "status=%d native_status=%d stage=%s%s%.*s%s",
                    call_status, response ? response->status_code : 0u,
                    error ? error->status : 0, error ? error->native_status : 0, stage,
                    preview ? " body_preview=\"" : "", (int)preview, body ? body : "",
                    preview ? "\"" : "");
  if (needed < 0) return NULL;
  buffer = (char *)malloc((size_t)needed + 1u);
  if (!buffer) return NULL;
  (void)snprintf(buffer, (size_t)needed + 1u,
                 "http transport failed: call_status=%d http_status=%u "
                 "status=%d native_status=%d stage=%s%s%.*s%s",
                 call_status, response ? response->status_code : 0u,
                 error ? error->status : 0, error ? error->native_status : 0, stage,
                 preview ? " body_preview=\"" : "", (int)preview, body ? body : "",
                 preview ? "\"" : "");
  return buffer;
}

CXX_C_API int turbo_agent_http_transport(const char *request_json, char **out_response_json,
                                         void *user_data) {
  turbo_agent_t *agent = (turbo_agent_t *)user_data;
  turbo_agent_http_endpoint_t endpoint;
  chttp_header headers[8];
  chttp_options options;
  chttp_response response = {0};
  chttp_error error = {0};
  size_t header_count = 0u;
  size_t provider_count = 0u;
  int status;
  char *copy = NULL;

  if (!agent || !agent->http_client || !request_json || !out_response_json) return -1;
  *out_response_json = NULL;
  turbo_agent_clear_last_stream_sse(agent);

  if (turbo_agent_http_endpoint_build(agent, &endpoint) != 0) {
    *out_response_json =
        turbo_agent_transport_strdup_malloc("invalid model base URL for CHTTP transport");
    return -1;
  }

  headers[header_count++] = (chttp_header){.name = "Content-Type", .value = "application/json"};
  headers[header_count++] =
      (chttp_header){.name = "Accept",
                     .value = agent->stream_response ? "text/event-stream" : "application/json"};
  headers[header_count++] =
      (chttp_header){.name = "User-Agent", .value = "TurboAgent/1.0"};

  if (agent->provider && agent->provider->build_http_headers) {
    if (agent->provider->build_http_headers(agent, headers + header_count,
                                            sizeof(headers) / sizeof(headers[0]) - header_count,
                                            &provider_count) != 0 ||
        provider_count > sizeof(headers) / sizeof(headers[0]) - header_count) {
      turbo_agent_http_endpoint_destroy(&endpoint);
      *out_response_json =
          turbo_agent_transport_strdup_malloc("failed to build provider HTTP headers");
      return -1;
    }
    header_count += provider_count;
  }

  options = (chttp_options){
      .connection_uri = endpoint.connection_uri,
      .authority = endpoint.authority,
      .target = endpoint.target,
      .headers = headers,
      .header_count = header_count,
      .body = request_json,
      .body_size = strlen(request_json),
      .timeout_ms = TURBO_AGENT_MODEL_HTTP_TIMEOUT_MS,
      .tls = NULL,
      .protocol = CHTTP_HTTP_1_1};

  status = chttp_post(agent->http_client, &options, &response, &error);
  turbo_agent_http_endpoint_destroy(&endpoint);

  if (status != SALTS_OK || response.status_code < 200u || response.status_code >= 300u ||
      response.body == NULL) {
    *out_response_json = turbo_agent_http_transport_error_detail(status, &response, &error);
    chttp_response_destroy(&response);
    return -1;
  }

  if (agent->stream_response) {
    if (turbo_agent_capture_last_stream_sse(agent, response.body, response.body_size) != 0) {
      chttp_response_destroy(&response);
      *out_response_json = turbo_agent_transport_strdup_malloc("failed to capture SSE stream");
      return -1;
    }
    if (!agent->provider ||
        turbo_agent_provider_sse_to_response_json(agent->provider, agent->last_stream_sse,
                                                  agent->last_stream_sse_len,
                                                  out_response_json) != 0) {
      chttp_response_destroy(&response);
      *out_response_json = turbo_agent_transport_strdup_malloc(
          "failed to aggregate SSE stream into a JSON model response");
      return -1;
    }
    chttp_response_destroy(&response);
    return 0;
  }

  copy = turbo_agent_transport_copy_bytes(response.body, response.body_size);
  chttp_response_destroy(&response);
  if (!copy) return -1;
  *out_response_json = copy;
  return 0;
}
