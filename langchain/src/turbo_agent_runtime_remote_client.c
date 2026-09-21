#include "turbo_agent_runtime_remote_client.h"
#include "turbo_agent_state.h"

#include <uri_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct turbo_agent_runtime_remote_call_result_s {
  int error_code;
  unsigned int http_status;
  const char *error_message;
} turbo_agent_runtime_remote_call_result_t;

struct turbo_agent_runtime_remote_client_s {
  chttp_client *http_client;
  int owns_http_client;
  char connection_uri[640];
  char authority[320];
  char *target;
  uint64_t next_request_id;
};

static native_io_backend_kind turbo_agent_runtime_remote_client_backend(void) {
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

static int turbo_agent_runtime_remote_client_method_retryable(const char *method) {
  if (!method) return 0;
  return strncmp(method, "runtime.get", 11) == 0 ||
         strncmp(method, "runtime.list", 12) == 0 ||
         strncmp(method, "runtime.load", 12) == 0 ||
         strncmp(method, "runtime.replay", 14) == 0 ||
         strncmp(method, "memory.get", 10) == 0 ||
         strncmp(method, "memory.list", 11) == 0 ||
         strncmp(method, "memory.query", 12) == 0 ||
         strcmp(method, "memory.validateRecord") == 0;
}

static char *turbo_agent_runtime_remote_client_text_copy(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text);
  copy = (char *)malloc(length + 1u);
  if (!copy) return NULL;
  memcpy(copy, text, length + 1u);
  return copy;
}

static int turbo_agent_runtime_remote_client_endpoint(
    const char *url, char *connection_uri, size_t connection_capacity,
    char *authority, size_t authority_capacity, char **out_target) {
  uri_t uri;
  const char *transport_scheme;
  int default_port;
  int port;
  int is_ipv6;
  int written;
  char *target;
  size_t path_length;
  size_t query_length;
  size_t target_length;

  if (!url || !connection_uri || !authority || !out_target) return -1;
  *out_target = NULL;
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
                ? snprintf(connection_uri, connection_capacity, "%s://[%s]:%d",
                           transport_scheme, uri.host, port)
                : snprintf(connection_uri, connection_capacity, "%s://%s:%d",
                           transport_scheme, uri.host, port);
  if (written < 0 || (size_t)written >= connection_capacity) return -1;

  if ((uri.component_flags & URI_COMPONENT_PORT) != 0u) {
    written = is_ipv6 ? snprintf(authority, authority_capacity, "[%s]:%d", uri.host, port)
                      : snprintf(authority, authority_capacity, "%s:%d", uri.host, port);
  } else {
    written = is_ipv6 ? snprintf(authority, authority_capacity, "[%s]", uri.host)
                      : snprintf(authority, authority_capacity, "%s", uri.host);
  }
  if (written < 0 || (size_t)written >= authority_capacity) return -1;

  path_length = strlen(uri.path[0] ? uri.path : "/");
  query_length = (uri.component_flags & URI_COMPONENT_QUERY) != 0u ? strlen(uri.query) : 0u;
  if (path_length > SIZE_MAX - query_length - 2u) return -1;
  target_length = path_length +
                  ((uri.component_flags & URI_COMPONENT_QUERY) != 0u ? 1u + query_length : 0u);
  target = (char *)malloc(target_length + 1u);
  if (!target) return -1;
  memcpy(target, uri.path[0] ? uri.path : "/", path_length);
  if ((uri.component_flags & URI_COMPONENT_QUERY) != 0u) {
    target[path_length] = '?';
    memcpy(target + path_length + 1u, uri.query, query_length);
  }
  target[target_length] = '\0';
  *out_target = target;
  return 0;
}

static chttp_client_config turbo_agent_runtime_remote_client_http_config(void) {
  const cnet_client_config network = {
      .backend = turbo_agent_runtime_remote_client_backend(),
      .connection_capacity = 2u,
      .command_capacity = 32u,
      .request_capacity = 32u,
      .completion_batch_capacity = 8u,
      .event_capacity = 32u,
      .max_send_bytes = 16u * 1024u * 1024u,
      .receive_buffer_bytes = 256u * 1024u,
      .connect_timeout_ms = 60000u,
      .read_timeout_ms = 60000u,
      .write_timeout_ms = 60000u,
      .tls_io_buffer_bytes = 64u * 1024u,
      .tls_handshake_timeout_ms = 60000u};
  return (chttp_client_config){
      .network = network,
      .request_capacity = 2u,
      .max_start_line_bytes = 8192u,
      .max_header_count = 32u,
      .max_header_bytes = 64u * 1024u,
      .max_request_body_bytes = 16u * 1024u * 1024u,
      .max_response_body_bytes = 64u * 1024u * 1024u,
      .max_informational_responses = 4u,
      .stream_chunk_bytes = 64u * 1024u,
      .h2_input_buffer_bytes = 128u * 1024u,
      .h2_hpack_dynamic_table_bytes = 4096u,
      .h2_max_settings_count = 16u};
}

static int turbo_agent_runtime_remote_client_add_run_options_json(
    json_value_t *params_json, const turbo_graph_run_options_t *options) {
  json_value_t *options_json;
  json_value_t *interrupt_nodes_json;
  size_t i;

  if (!params_json) {
    return -1;
  }
  if (!options) {
    return 0;
  }

  options_json = turbo_json_create_object();
  if (!options_json) {
    return -1;
  }

  if (options->max_steps > 0) {
    turbo_json_object_set_number(options_json, "max_steps", (double)options->max_steps);
  }
  if (options->start_node && options->start_node[0] != '\0') {
    turbo_json_object_set_string(options_json, "start_node", options->start_node);
  }
  if (options->skip_initial_interrupt) {
    turbo_json_object_set_bool(options_json, "skip_initial_interrupt", true);
  }
  if (options->interrupt_before_nodes && options->interrupt_before_count > 0) {
    interrupt_nodes_json = turbo_json_create_array();
    if (!interrupt_nodes_json) {
      turbo_free_json(&options_json);
      return -1;
    }
    for (i = 0; i < options->interrupt_before_count; ++i) {
      const char *node_name = options->interrupt_before_nodes[i];
      if (!node_name || !node_name[0]) {
        continue;
      }
      turbo_json_array_add(interrupt_nodes_json, turbo_json_create_string(node_name));
    }
    turbo_json_object_add(options_json, "interrupt_before_nodes", interrupt_nodes_json);
  }

  turbo_json_object_add(params_json, "options", options_json);
  return 0;
}

static json_value_t *turbo_agent_runtime_remote_client_run_params_json(
    const char *graph_name, const char *thread_id,
    const json_value_t *state_json_value, const char *checkpoint_id,
    const turbo_graph_run_options_t *options) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *state_json = NULL;

  if (!params_json || !graph_name || !graph_name[0]) {
    turbo_free_json(&params_json);
    return NULL;
  }

  turbo_json_object_set_string(params_json, "graph_name", graph_name);
  if (thread_id && thread_id[0] != '\0') {
    turbo_json_object_set_string(params_json, "thread_id", thread_id);
  }
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(params_json, "checkpoint_id", checkpoint_id);
  }
  if (state_json_value) {
    state_json = turbo_json_clone(state_json_value);
    if (!state_json) {
      turbo_free_json(&params_json);
      return NULL;
    }
    turbo_json_object_add(params_json, "state", state_json);
  }
  if (turbo_agent_runtime_remote_client_add_run_options_json(params_json, options) != 0) {
    turbo_free_json(&params_json);
    return NULL;
  }
  return params_json;
}

static json_value_t *turbo_agent_runtime_remote_client_thread_command_params_json(
    const char *graph_name, const char *thread_id,
    const json_value_t *command_json_value,
    const turbo_graph_run_options_t *options) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *command_json = NULL;

  if (!params_json || !graph_name || !graph_name[0] || !thread_id || !thread_id[0] || !command_json_value) {
    turbo_free_json(&params_json);
    return NULL;
  }

  command_json = turbo_json_clone(command_json_value);
  if (!command_json) {
    turbo_free_json(&params_json);
    return NULL;
  }

  turbo_json_object_set_string(params_json, "graph_name", graph_name);
  turbo_json_object_set_string(params_json, "thread_id", thread_id);
  turbo_json_object_add(params_json, "command", command_json);
  if (turbo_agent_runtime_remote_client_add_run_options_json(params_json, options) != 0) {
    turbo_free_json(&params_json);
    return NULL;
  }
  return params_json;
}

static int turbo_agent_runtime_remote_client_extract_object(
    const json_value_t *result_json, const char *field_name, json_value_t **out_object_json) {
  const json_value_t *field_json;

  if (out_object_json) {
    *out_object_json = NULL;
  }
  if (!result_json || turbo_json_type(result_json) != TURBO_JSON_OBJECT || !field_name ||
      !field_name[0] || !out_object_json) {
    return -1;
  }

  field_json = turbo_json_object_get(result_json, field_name);
  if (!field_json) {
    return -1;
  }
  *out_object_json = turbo_json_clone(field_json);
  return *out_object_json ? 0 : -1;
}

static int turbo_agent_runtime_remote_client_extract_summary_state(
    const json_value_t *result_json, json_value_t **out_summary_json,
    json_value_t **out_state) {
  json_value_t *summary_json = NULL;
  json_value_t *state_json = NULL;

  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  if (out_state) {
    *out_state = NULL;
  }
  if (!result_json || turbo_json_type(result_json) != TURBO_JSON_OBJECT) {
    return -1;
  }

  if (out_summary_json) {
    if (turbo_agent_runtime_remote_client_extract_object(result_json, "summary", &summary_json) != 0) {
      turbo_free_json(&summary_json);
      return -1;
    }
    *out_summary_json = summary_json;
    summary_json = NULL;
  }

  if (out_state) {
    if (turbo_agent_runtime_remote_client_extract_object(result_json, "state", &state_json) != 0) {
      if (out_summary_json) {
        turbo_free_json(out_summary_json);
      }
      turbo_free_json(&state_json);
      if (out_summary_json) {
        *out_summary_json = NULL;
      }
      return -1;
    }
    *out_state = turbo_json_clone(state_json);
    turbo_free_json(&state_json);
    if (!*out_state) {
      if (out_summary_json) {
        turbo_free_json(out_summary_json);
      }
      if (out_summary_json) {
        *out_summary_json = NULL;
      }
      return -1;
    }
  }

  return 0;
}

static int turbo_agent_runtime_remote_client_extract_json_value_object(
    const json_value_t *result_json, const char *field_name,
    json_value_t **out_json_value) {
  json_value_t *object_json = NULL;
  int rc;

  if (out_json_value) {
    *out_json_value = NULL;
  }
  if (!result_json || !field_name || !field_name[0] || !out_json_value) {
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_extract_object(result_json, field_name, &object_json);
  if (rc != 0) {
    turbo_free_json(&object_json);
    return rc;
  }
  *out_json_value = turbo_json_clone(object_json);
  turbo_free_json(&object_json);
  return *out_json_value ? 0 : -1;
}

static json_value_t *turbo_agent_runtime_remote_client_build_memory_query_params_json(
    const turbo_agent_memory_query_options_t *options) {
  json_value_t *params_json = turbo_json_create_object();

  if (!params_json) {
    return NULL;
  }
  if (options) {
    if (options->namespace_prefix && options->namespace_prefix[0] != '\0') {
      turbo_json_object_set_string(params_json, "namespace_prefix", options->namespace_prefix);
    }
    if (options->kind && options->kind[0] != '\0') {
      turbo_json_object_set_string(params_json, "kind", options->kind);
    }
    if (options->key_prefix && options->key_prefix[0] != '\0') {
      turbo_json_object_set_string(params_json, "key_prefix", options->key_prefix);
    }
    if (options->text_substring && options->text_substring[0] != '\0') {
      turbo_json_object_set_string(params_json, "text_substring", options->text_substring);
    }
    if (options->id_prefix && options->id_prefix[0] != '\0') {
      turbo_json_object_set_string(params_json, "id_prefix", options->id_prefix);
    }
    if (options->metadata_scope && options->metadata_scope[0] != '\0') {
      turbo_json_object_set_string(params_json, "metadata_scope", options->metadata_scope);
    }
    if (options->metadata_path_prefix && options->metadata_path_prefix[0] != '\0') {
      turbo_json_object_set_string(params_json, "metadata_path_prefix",
                                   options->metadata_path_prefix);
    }
    if (options->created_after && options->created_after[0] != '\0') {
      turbo_json_object_set_string(params_json, "created_after", options->created_after);
    }
    if (options->created_before && options->created_before[0] != '\0') {
      turbo_json_object_set_string(params_json, "created_before", options->created_before);
    }
    if (options->sort_by && options->sort_by[0] != '\0') {
      turbo_json_object_set_string(params_json, "sort_by", options->sort_by);
    }
    if (options->sort_order && options->sort_order[0] != '\0') {
      turbo_json_object_set_string(params_json, "sort_order", options->sort_order);
    }
    if (options->limit > 0) {
      turbo_json_object_set_number(params_json, "limit", (double)options->limit);
    }
  }
  return params_json;
}

static json_value_t *turbo_agent_runtime_remote_client_build_error_json(
    const turbo_agent_runtime_remote_call_result_t *call_result,
    int transport_error, const char *fallback_message) {
  json_value_t *error_json = turbo_json_create_object();
  const char *message = fallback_message ? fallback_message : "RPC call failed";

  if (!error_json) return NULL;
  if (call_result && call_result->error_message && call_result->error_message[0] != '\0') {
    message = call_result->error_message;
  }
  turbo_json_object_set_number(error_json, "code",
                               call_result ? (double)call_result->error_code : 0.0);
  turbo_json_object_set_string(error_json, "message", message);
  turbo_json_object_set_number(error_json, "http_status",
                               call_result ? (double)call_result->http_status : 0.0);
  turbo_json_object_set_bool(error_json, "transport_error", transport_error ? true : false);
  return error_json;
}

CXX_C_API turbo_agent_runtime_remote_client_t *turbo_agent_runtime_remote_client_create(
    const turbo_agent_runtime_remote_client_config_t *config) {
  turbo_agent_runtime_remote_client_t *client;
  chttp_client_config http_config;

  if (!config || !config->url || !config->url[0]) return NULL;

  client = (turbo_agent_runtime_remote_client_t *)calloc(1u, sizeof(*client));
  if (!client) return NULL;
  if (turbo_agent_runtime_remote_client_endpoint(
          config->url, client->connection_uri, sizeof(client->connection_uri),
          client->authority, sizeof(client->authority), &client->target) != 0) {
    free(client);
    return NULL;
  }

  if (config->http_client) {
    client->http_client = config->http_client;
    client->owns_http_client = 0;
  } else {
    client->http_client = (chttp_client *)calloc(1u, sizeof(*client->http_client));
    if (!client->http_client) {
      free(client->target);
      free(client);
      return NULL;
    }
    http_config = turbo_agent_runtime_remote_client_http_config();
    if (http_config.network.backend == (native_io_backend_kind)0 ||
        chttp_client_init(client->http_client, &http_config) != SALTS_OK) {
      free(client->http_client);
      free(client->target);
      free(client);
      return NULL;
    }
    client->owns_http_client = 1;
  }

  client->next_request_id = 1u;
  return client;
}

CXX_C_API void turbo_agent_runtime_remote_client_destroy(
    turbo_agent_runtime_remote_client_t *client) {
  if (!client) return;
  if (client->owns_http_client && client->http_client) {
    (void)chttp_client_destroy(client->http_client, 0u);
    free(client->http_client);
  }
  free(client->target);
  free(client);
}

static int turbo_agent_runtime_remote_client_response_id(
    const json_value_t *response_json, uint64_t expected_id) {
  const json_value_t *id;
  const char *text;
  char expected[32];
  size_t length = 0u;
  int written;

  if (!response_json || json_type(response_json) != JSON_OBJECT) return -1;
  id = json_object_get(response_json, "id");
  if (!id || json_type(id) != JSON_NUMBER) return -1;
  text = json_number_text(id, &length);
  written = snprintf(expected, sizeof(expected), "%llu",
                     (unsigned long long)expected_id);
  if (!text || written < 0 || (size_t)written != length ||
      memcmp(text, expected, length) != 0) {
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_runtime_remote_client_call_json(
    turbo_agent_runtime_remote_client_t *client, const char *method,
    const json_value_t *params_json, json_value_t **out_result_json,
    json_value_t **out_error_json) {
  json_value_t *request_json = NULL;
  json_value_t *response_json = NULL;
  json_value_t *owned_params = NULL;
  json_value_t *result_value;
  json_value_t *remote_error;
  json_value_t *error_code;
  const char *error_message;
  char *request_text = NULL;
  size_t request_size = 0u;
  uint64_t request_id;
  chttp_header headers[2] = {
      {.name = "Content-Type", .value = "application/json"},
      {.name = "Accept", .value = "application/json"}};
  chttp_options options;
  chttp_response response = {0};
  chttp_error error = {0};
  turbo_agent_runtime_remote_call_result_t call_result = {0};
  int status = SALTS_EINVAL;
  int attempt;
  int max_attempts;

  if (out_result_json) *out_result_json = NULL;
  if (out_error_json) *out_error_json = NULL;
  if (!client || !client->http_client || !method || !method[0] ||
      (!out_result_json && !out_error_json) || client->next_request_id == 0u) {
    return -1;
  }

  request_id = client->next_request_id++;
  request_json = json_create_object();
  if (!request_json) goto local_fail;
  json_object_set_string(request_json, "jsonrpc", "2.0");
  json_object_set_string(request_json, "method", method);
  if (!json_object_add_checked(request_json, "id", json_create_uint64(request_id))) {
    goto local_fail;
  }
  if (params_json) {
    owned_params = json_clone(params_json);
    if (!owned_params ||
        !json_object_add_checked(request_json, "params", owned_params)) {
      json_free(owned_params);
      owned_params = NULL;
      goto local_fail;
    }
    owned_params = NULL;
  }

  request_text = json_serialize(request_json, &request_size);
  json_free(request_json);
  request_json = NULL;
  if (!request_text) goto local_fail;

  options = (chttp_options){
      .connection_uri = client->connection_uri,
      .authority = client->authority,
      .target = client->target,
      .headers = headers,
      .header_count = sizeof(headers) / sizeof(headers[0]),
      .body = request_text,
      .body_size = request_size,
      .timeout_ms = 60000u,
      .protocol = CHTTP_HTTP_1_1};

  max_attempts = turbo_agent_runtime_remote_client_method_retryable(method) ? 3 : 2;
  for (attempt = 0; attempt < max_attempts; ++attempt) {
    memset(&response, 0, sizeof(response));
    memset(&error, 0, sizeof(error));
    status = chttp_post(client->http_client, &options, &response, &error);
    if (status == SALTS_OK || attempt + 1 >= max_attempts) break;
    chttp_response_destroy(&response);
  }
  json_serialize_free(request_text);
  request_text = NULL;

  if (status != SALTS_OK) {
    call_result.error_code = error.status ? error.status : status;
    call_result.http_status = response.status_code;
    call_result.error_message = error.stage;
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          &call_result, 1, "RPC transport failed");
    }
    chttp_response_destroy(&response);
    return -1;
  }

  if (response.status_code < 200u || response.status_code >= 300u ||
      !response.body || response.body_size == 0u) {
    call_result.http_status = response.status_code;
    call_result.error_message = "RPC endpoint returned non-success HTTP status";
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          &call_result, 1, "RPC endpoint failed");
    }
    chttp_response_destroy(&response);
    return -1;
  }

  response_json = json_parse((const char *)response.body, response.body_size);
  if (!response_json ||
      turbo_agent_runtime_remote_client_response_id(response_json, request_id) != 0) {
    call_result.http_status = response.status_code;
    call_result.error_message = "Failed to parse JSON-RPC response";
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          &call_result, 1, "Failed to parse JSON-RPC response");
    }
    json_free(response_json);
    chttp_response_destroy(&response);
    return -1;
  }

  remote_error = json_object_get(response_json, "error");
  result_value = json_object_get(response_json, "result");
  if (remote_error && json_type(remote_error) == JSON_OBJECT) {
    error_code = json_object_get(remote_error, "code");
    error_message = json_get_string(remote_error, "message");
    call_result.error_code =
        error_code && json_type(error_code) == JSON_NUMBER ? (int)json_number(error_code) : 0;
    call_result.http_status = response.status_code;
    call_result.error_message = error_message;
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          &call_result, 0, "JSON-RPC method failed");
    }
    json_free(response_json);
    chttp_response_destroy(&response);
    return 0;
  }

  if (!result_value) {
    call_result.http_status = response.status_code;
    call_result.error_message = "JSON-RPC response has neither result nor error";
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          &call_result, 1, "Malformed JSON-RPC response");
    }
    json_free(response_json);
    chttp_response_destroy(&response);
    return -1;
  }

  if (out_result_json) {
    *out_result_json = json_clone(result_value);
    if (!*out_result_json) {
      if (out_error_json) {
        call_result.http_status = response.status_code;
        *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
            &call_result, 1, "Failed to clone JSON-RPC result");
      }
      json_free(response_json);
      chttp_response_destroy(&response);
      return -1;
    }
  }

  json_free(response_json);
  chttp_response_destroy(&response);
  return 0;

local_fail:
  json_free(owned_params);
  json_free(request_json);
  json_serialize_free(request_text);
  if (out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Failed to build RPC request");
  }
  return -1;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_startup_diagnostics(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    json_value_t **out_diagnostics_json, json_value_t **out_error_json) {
  json_value_t *params_json = NULL;
  int rc;

  if (out_diagnostics_json) {
    *out_diagnostics_json = NULL;
  }
  if (!client || !graph_name || !graph_name[0] || !out_diagnostics_json) {
    return -1;
  }

  params_json = turbo_json_create_object();
  if (!params_json) {
    return -1;
  }
  turbo_json_object_set_string(params_json, "graph_name", graph_name);
  rc = turbo_agent_runtime_remote_client_call_json(
      client, "runtime.getStartupDiagnostics", params_json, out_diagnostics_json,
      out_error_json);
  turbo_free_json(&params_json);
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_start_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    const char *thread_id, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json;
  json_value_t *result_json = NULL;
  int rc;

  params_json =
      turbo_agent_runtime_remote_client_run_params_json(graph_name, thread_id, state, NULL, options);
  if (!params_json) {
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.start params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.start", params_json, &result_json,
                                                   out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_summary_state(result_json, out_summary_json,
                                                               out_state);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.start result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_thread_timeline_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_timeline, json_value_t **out_error_json) {
  json_value_t *index_json = NULL;
  int rc;

  if (out_timeline) {
    *out_timeline = NULL;
  }
  if (!out_timeline) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_observability_index(
      client, thread_id, &index_json, out_error_json);
  if (rc != 0 || !index_json) {
    turbo_free_json(&index_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_json_value_object(index_json, "thread_timeline",
                                                             out_timeline);
  turbo_free_json(&index_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getThreadObservabilityIndex.thread_timeline");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_branch_tree(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_branch_tree_json, json_value_t **out_error_json) {
  json_value_t *index_json = NULL;
  int rc;

  if (out_branch_tree_json) {
    *out_branch_tree_json = NULL;
  }
  if (!out_branch_tree_json) {
    return -1;
  }
  rc = turbo_agent_runtime_remote_client_get_thread_observability_index(
      client, thread_id, &index_json, out_error_json);
  if (rc != 0 || !index_json) {
    turbo_free_json(&index_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(index_json, "branch_tree",
                                                        out_branch_tree_json);
  turbo_free_json(&index_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getThreadObservabilityIndex.branch_tree");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_thread_observability_index(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_index_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_index_json) {
    *out_index_json = NULL;
  }
  if (!params_json || !thread_id || !thread_id[0] || !out_index_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getThreadObservabilityIndex params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "thread_id", thread_id);
  rc = turbo_agent_runtime_remote_client_call_json(
      client, "runtime.getThreadObservabilityIndex", params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "index", out_index_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getThreadObservabilityIndex result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_list_observability_indexes(
    turbo_agent_runtime_remote_client_t *client, json_value_t **out_indexes_json,
    json_value_t **out_error_json) {
  return turbo_agent_runtime_remote_client_list_observability_indexes_filtered(
      client, NULL, out_indexes_json, out_error_json);
}

CXX_C_API int turbo_agent_runtime_remote_client_list_observability_indexes_filtered(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *filters_json,
    json_value_t **out_indexes_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  json_value_t *filters_clone = NULL;
  int rc;

  if (out_indexes_json) {
    *out_indexes_json = NULL;
  }
  if (!params_json || !out_indexes_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.listObservabilityIndexesFiltered params");
    }
    return -1;
  }

  if (filters_json) {
    filters_clone = turbo_json_clone(filters_json);
    if (!filters_clone) {
      turbo_free_json(&params_json);
      if (out_error_json) {
        *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
            NULL, 1, "Failed to clone observability filters");
      }
      return -1;
    }
    turbo_json_object_add(params_json, "filters", filters_clone);
  }

  rc = turbo_agent_runtime_remote_client_call_json(
      client, "runtime.listObservabilityIndexesFiltered", params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "indexes", out_indexes_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.listObservabilityIndexesFiltered result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_list_child_runs(
    turbo_agent_runtime_remote_client_t *client, const char *parent_agent_run_id,
    json_value_t **out_runs_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_runs_json) {
    *out_runs_json = NULL;
  }
  if (!params_json || !parent_agent_run_id || !parent_agent_run_id[0] || !out_runs_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "parent_agent_run_id is required");
    }
    return -1;
  }
  turbo_json_object_set_string(params_json, "parent_agent_run_id", parent_agent_run_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.listChildRuns", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "runs", out_runs_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.listChildRuns result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_supervisor_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_inspect_json) {
  json_value_t *state_json_value = NULL;
  json_value_t *control_json_value = NULL;
  json_value_t *workflow_json_value = NULL;
  json_value_t *state_json = NULL;
  json_value_t *control_json = NULL;
  json_value_t *workflow_json = NULL;
  json_value_t *inspect_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *latest_handoff_event_json = NULL;
  json_value_t *supervisor_json = NULL;
  const json_value_t *state_inbox = NULL;
  const json_value_t *state_history = NULL;
  const json_value_t *state_latest_handoff_event = NULL;
  const json_value_t *control_supervisor = NULL;
  json_value_t *error_json = NULL;
  int rc = -1;

  if (out_inspect_json) {
    *out_inspect_json = NULL;
  }
  if (!client || !thread_id || !thread_id[0] || !out_inspect_json) {
    return -1;
  }

  if (turbo_agent_runtime_remote_client_get_thread_state_json_value(client, thread_id, &state_json_value,
                                                              &error_json) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  turbo_free_json(&error_json);
  state_json = turbo_json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }

  control_json_value = turbo_agent_state_control_snapshot_json_value(state_json_value);
  workflow_json_value = turbo_agent_state_workflow_snapshot_json_value(state_json_value);
  if (!control_json_value || !workflow_json_value) {
    goto cleanup;
  }
  control_json = turbo_json_clone(control_json_value);
  workflow_json = turbo_json_clone(workflow_json_value);
  if (!control_json || !workflow_json) {
    goto cleanup;
  }

  state_inbox = turbo_agent_state_supervisor_inbox(state_json);
  state_history = turbo_agent_state_supervisor_handoff_history(state_json);
  state_latest_handoff_event = turbo_agent_state_latest_handoff_event(state_json);
  control_supervisor = turbo_json_object_get(control_json, "supervisor");

  inbox_json = state_inbox && turbo_json_type(state_inbox) == TURBO_JSON_ARRAY
                   ? turbo_json_clone(state_inbox)
                   : turbo_json_create_array();
  history_json = state_history && turbo_json_type(state_history) == TURBO_JSON_ARRAY
                     ? turbo_json_clone(state_history)
                     : turbo_json_create_array();
  latest_handoff_event_json =
      state_latest_handoff_event &&
              turbo_json_type(state_latest_handoff_event) == TURBO_JSON_OBJECT
          ? turbo_json_clone(state_latest_handoff_event)
          : NULL;
  supervisor_json = control_supervisor && turbo_json_type(control_supervisor) == TURBO_JSON_OBJECT
                        ? turbo_json_clone(control_supervisor)
                        : turbo_json_create_object();
  inspect_json = turbo_json_create_object();
  if (!inbox_json || !history_json || !supervisor_json || !inspect_json ||
      (state_latest_handoff_event && !latest_handoff_event_json)) {
    goto cleanup;
  }

  turbo_json_object_add(inspect_json, "supervisor", supervisor_json);
  supervisor_json = NULL;
  turbo_json_object_add(inspect_json, "inbox", inbox_json);
  inbox_json = NULL;
  turbo_json_object_add(inspect_json, "handoff_history", history_json);
  history_json = NULL;
  if (latest_handoff_event_json) {
    turbo_json_object_add(inspect_json, "latest_handoff_event", latest_handoff_event_json);
    latest_handoff_event_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_handoff_event");
  }
  turbo_json_object_add(inspect_json, "control_snapshot", control_json);
  control_json = NULL;
  turbo_json_object_add(inspect_json, "workflow_snapshot", workflow_json);
  workflow_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&error_json);
  turbo_free_json(&supervisor_json);
  turbo_free_json(&latest_handoff_event_json);
  turbo_free_json(&history_json);
  turbo_free_json(&inbox_json);
  turbo_free_json(&inspect_json);
  turbo_free_json(&workflow_json);
  turbo_free_json(&control_json);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(workflow_json_value);
  turbo_runtime_json_destroy(control_json_value);
  turbo_runtime_json_destroy(state_json_value);
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_orchestration_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *index_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  const json_value_t *latest_run_json = NULL;
  const json_value_t *pending_run_json = NULL;
  const char *parent_run_id = NULL;
  json_value_t *error_json = NULL;
  int rc = -1;

  if (out_inspect_json) {
    *out_inspect_json = NULL;
  }
  if (!client || !thread_id || !thread_id[0] || !out_inspect_json) {
    return -1;
  }

  if (turbo_agent_runtime_remote_client_get_supervisor_inspect(client, thread_id,
                                                               &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_get_thread_observability_index(client, thread_id, &index_json,
                                                                       &error_json) != 0 ||
      !index_json) {
    goto cleanup;
  }
  turbo_free_json(&error_json);

  if (turbo_agent_runtime_remote_client_extract_object(index_json, "thread_timeline",
                                                       &thread_timeline_json) != 0 ||
      !thread_timeline_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_extract_object(index_json, "thread_lineage",
                                                       &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_extract_object(index_json, "branch_tree",
                                                       &branch_tree_json) != 0 ||
      !branch_tree_json) {
    goto cleanup;
  }

  latest_run_json = turbo_json_object_get(index_json, "latest_run");
  pending_run_json = turbo_json_object_get(index_json, "pending_run");
  if (latest_run_json && turbo_json_type(latest_run_json) == TURBO_JSON_OBJECT) {
    parent_run_id = turbo_json_get_string(latest_run_json, "id");
  }
  if ((!parent_run_id || !parent_run_id[0]) && pending_run_json &&
      turbo_json_type(pending_run_json) == TURBO_JSON_OBJECT) {
    parent_run_id = turbo_json_get_string(pending_run_json, "id");
  }
  if (parent_run_id && parent_run_id[0] != '\0') {
    if (turbo_agent_runtime_remote_client_list_child_runs(client, parent_run_id, &child_runs_json,
                                                          &error_json) != 0 ||
        !child_runs_json) {
      goto cleanup;
    }
    turbo_free_json(&error_json);
  } else {
    child_runs_json = turbo_json_create_array();
    if (!child_runs_json) {
      goto cleanup;
    }
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
  thread_timeline_json = NULL;
  turbo_json_object_add(inspect_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  turbo_json_object_add(inspect_json, "child_runs", child_runs_json);
  child_runs_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&error_json);
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_runs_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_free_json(&index_json);
  turbo_free_json(&supervisor_inspect_json);
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_child_inspect(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *checkpoints_json = NULL;
  json_value_t *latest_checkpoint_json = NULL;
  json_value_t *checkpoint_context_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *index_json = NULL;
  json_value_t *history_events_json_value = NULL;
  json_value_t *trace_events_json_value = NULL;
  const char *child_run_id;
  const char *child_checkpoint_id;
  const char *child_thread_id;
  json_value_t *observability_error_json = NULL;
  int rc = -1;

  if (out_inspect_json) {
    *out_inspect_json = NULL;
  }
  if (!client || !output_item || !out_inspect_json) {
    return -1;
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  child_thread_id = turbo_agent_state_tool_result_child_thread_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }

  if (turbo_agent_runtime_remote_client_get_run(client, child_run_id, &run_json,
                                                &observability_error_json) != 0 ||
      !run_json) {
    goto cleanup;
  }
  turbo_free_json(&observability_error_json);

  if (turbo_agent_runtime_remote_client_list_checkpoints(client, child_run_id, &checkpoints_json,
                                                         &observability_error_json) != 0 ||
      !checkpoints_json) {
    goto cleanup;
  }
  turbo_free_json(&observability_error_json);

  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_runtime_remote_client_get_checkpoint(client, child_checkpoint_id,
                                                         &latest_checkpoint_json,
                                                         &observability_error_json) != 0 ||
        !latest_checkpoint_json) {
      goto cleanup;
    }
    turbo_free_json(&observability_error_json);

    if (turbo_agent_runtime_remote_client_get_checkpoint_context(
            client, child_checkpoint_id, &checkpoint_context_json, &observability_error_json) != 0 ||
        !checkpoint_context_json) {
      goto cleanup;
    }
    turbo_free_json(&observability_error_json);
  }

  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    rc = turbo_agent_runtime_remote_client_load_history_events_json_value(
        client, NULL, child_checkpoint_id, &history_events_json_value, &observability_error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_load_history_events_json_value(
        client, child_run_id, NULL, &history_events_json_value, &observability_error_json);
  }
  if (rc != 0 || !history_events_json_value) {
    goto cleanup;
  }
  turbo_free_json(&observability_error_json);
  history_events_json = turbo_json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }

  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    rc = turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value(
        client, child_checkpoint_id, &trace_events_json_value, &observability_error_json);
  } else {
    rc = turbo_agent_runtime_remote_client_get_run_trace_events_json_value(
        client, child_run_id, &trace_events_json_value, &observability_error_json);
  }
  if (rc != 0 || !trace_events_json_value) {
    goto cleanup;
  }
  turbo_free_json(&observability_error_json);
  trace_events_json = turbo_json_clone(trace_events_json_value);
  if (!trace_events_json) {
    goto cleanup;
  }

  if (child_thread_id && child_thread_id[0] != '\0' &&
      turbo_agent_runtime_remote_client_get_thread_observability_index(
          client, child_thread_id, &index_json, &observability_error_json) == 0 &&
      index_json) {
    if (turbo_agent_runtime_remote_client_extract_object(index_json, "thread_timeline",
                                                        &thread_timeline_json) != 0) {
      turbo_free_json(&thread_timeline_json);
      thread_timeline_json = NULL;
    }
    if (turbo_agent_runtime_remote_client_extract_object(index_json, "branch_tree",
                                                        &branch_tree_json) != 0) {
      turbo_free_json(&branch_tree_json);
      branch_tree_json = NULL;
    }
  }
  turbo_free_json(&observability_error_json);
  turbo_free_json(&index_json);

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "run", run_json);
  run_json = NULL;
  turbo_json_object_add(inspect_json, "checkpoints", checkpoints_json);
  checkpoints_json = NULL;
  if (latest_checkpoint_json) {
    turbo_json_object_add(inspect_json, "latest_checkpoint", latest_checkpoint_json);
    latest_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_checkpoint");
  }
  if (checkpoint_context_json) {
    turbo_json_object_add(inspect_json, "checkpoint_context", checkpoint_context_json);
    checkpoint_context_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "checkpoint_context");
  }
  turbo_json_object_add(inspect_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(inspect_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  if (branch_tree_json) {
    turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
    branch_tree_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "branch_tree");
  }

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_timeline_json);
  turbo_free_json(&index_json);
  turbo_free_json(&trace_events_json);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_free_json(&history_events_json);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_free_json(&checkpoint_context_json);
  turbo_free_json(&latest_checkpoint_json);
  turbo_free_json(&checkpoints_json);
  turbo_free_json(&run_json);
  turbo_free_json(&observability_error_json);
  return *out_inspect_json ? rc : -1;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_child_orchestration_inspect(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *output_item,
    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *child_inspect_json = NULL;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;

  if (out_inspect_json) {
    *out_inspect_json = NULL;
  }
  if (!client || !output_item || !out_inspect_json) {
    return -1;
  }

  if (turbo_agent_runtime_remote_client_get_child_inspect(client, output_item,
                                                          &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }

  parent_agent_run_id = turbo_agent_state_tool_result_parent_agent_run_id(output_item);
  parent_tool_call_id = turbo_agent_state_tool_result_parent_tool_call_id(output_item);
  parent_tool_name = turbo_agent_state_tool_result_parent_tool_name(output_item);
  parent_graph_run_id = turbo_agent_state_tool_result_parent_graph_run_id(output_item);
  call_frame_id = turbo_agent_state_tool_result_call_frame_id(output_item);

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_agent_run_id");
  }
  if (parent_tool_call_id && parent_tool_call_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_call_id");
  }
  if (parent_tool_name && parent_tool_name[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_name");
  }
  if (parent_graph_run_id && parent_graph_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_graph_run_id");
  }
  if (call_frame_id && call_frame_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(inspect_json, "call_frame_id");
  }
  turbo_json_object_add(inspect_json, "child_inspect", child_inspect_json);
  child_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_child_multi_agent_inspect(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    const json_value_t *output_item, json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *child_orchestration_inspect_json = NULL;

  if (out_inspect_json) {
    *out_inspect_json = NULL;
  }
  if (!client || !thread_id || !thread_id[0] || !output_item || !out_inspect_json) {
    return -1;
  }

  if (turbo_agent_runtime_remote_client_get_supervisor_inspect(client, thread_id,
                                                               &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_get_orchestration_inspect(
          client, thread_id, &orchestration_inspect_json) != 0 ||
      !orchestration_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_remote_client_get_child_orchestration_inspect(
          client, output_item, &child_orchestration_inspect_json) != 0 ||
      !child_orchestration_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "orchestration_inspect", orchestration_inspect_json);
  orchestration_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "child_orchestration_inspect",
                        child_orchestration_inspect_json);
  child_orchestration_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_orchestration_inspect_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&supervisor_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_remote_client_resume_thread_command_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *thread_id, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json;
  json_value_t *result_json = NULL;
  int rc;

  params_json = turbo_agent_runtime_remote_client_thread_command_params_json(graph_name, thread_id,
                                                                             command, options);
  if (!params_json) {
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.resumeThreadCommandJsonValueGraph params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(
      client, "runtime.resumeThreadCommandJsonValueGraph", params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_summary_state(result_json, out_summary_json,
                                                               out_state);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.resumeThreadCommandJsonValueGraph result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_fork_thread_command_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *thread_id, const json_value_t *command,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json;
  json_value_t *result_json = NULL;
  int rc;

  params_json = turbo_agent_runtime_remote_client_thread_command_params_json(graph_name, thread_id,
                                                                             command, options);
  if (!params_json) {
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.forkThreadCommandJsonValueGraph params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(
      client, "runtime.forkThreadCommandJsonValueGraph", params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_summary_state(result_json, out_summary_json,
                                                               out_state);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.forkThreadCommandJsonValueGraph result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_resume_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *checkpoint_id, const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json;
  json_value_t *result_json = NULL;
  int rc;

  params_json = turbo_agent_runtime_remote_client_run_params_json(graph_name, NULL, state_override,
                                                                  checkpoint_id, options);
  if (!params_json || !checkpoint_id || !checkpoint_id[0]) {
    turbo_free_json(&params_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.resume params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.resume", params_json, &result_json,
                                                   out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_summary_state(result_json, out_summary_json,
                                                               out_state);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.resume result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_fork_json_value_graph(
    turbo_agent_runtime_remote_client_t *client, const char *graph_name,
    const char *checkpoint_id, const json_value_t *state_override,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json;
  json_value_t *result_json = NULL;
  int rc;

  params_json = turbo_agent_runtime_remote_client_run_params_json(graph_name, NULL, state_override,
                                                                  checkpoint_id, options);
  if (!params_json || !checkpoint_id || !checkpoint_id[0]) {
    turbo_free_json(&params_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.fork params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.fork", params_json, &result_json,
                                                   out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    if (out_summary_json) {
      *out_summary_json = NULL;
    }
    if (out_state) {
      *out_state = NULL;
    }
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_summary_state(result_json, out_summary_json,
                                                               out_state);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.fork result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_thread_state_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *thread_id,
    json_value_t **out_state, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  json_value_t *state_json = NULL;
  int rc;

  if (out_state) {
    *out_state = NULL;
  }
  if (!params_json || !thread_id || !thread_id[0] || !out_state) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getThreadState params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "thread_id", thread_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getThreadState", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "state", &state_json);
  if (rc == 0) {
    *out_state = turbo_json_clone(state_json);
    if (!*out_state) {
      rc = -1;
    }
  }
  turbo_free_json(&state_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getThreadState result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint_context(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_context_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_context_json) {
    *out_context_json = NULL;
  }
  if (!params_json || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getCheckpointContext params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "checkpoint_id", checkpoint_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getCheckpointContext",
                                                   params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "context", out_context_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getCheckpointContext result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_run(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_run_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_run_json) {
    *out_run_json = NULL;
  }
  if (!params_json || !run_id || !run_id[0] || !out_run_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json =
          turbo_agent_runtime_remote_client_build_error_json(NULL, 1,
                                                             "Failed to build runtime.getRun params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "run_id", run_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getRun", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "run", out_run_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json =
        turbo_agent_runtime_remote_client_build_error_json(NULL, 1,
                                                           "Malformed runtime.getRun result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_checkpoint_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_checkpoint_json) {
    *out_checkpoint_json = NULL;
  }
  if (!params_json || !checkpoint_id || !checkpoint_id[0] || !out_checkpoint_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getCheckpoint params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "checkpoint_id", checkpoint_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getCheckpoint", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "checkpoint",
                                                        out_checkpoint_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getCheckpoint result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_list_checkpoints(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_checkpoints_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_checkpoints_json) {
    *out_checkpoints_json = NULL;
  }
  if (!params_json || !run_id || !run_id[0] || !out_checkpoints_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.listCheckpoints params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "run_id", run_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.listCheckpoints", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "checkpoints",
                                                        out_checkpoints_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.listCheckpoints result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_load_history_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *run_id, const char *checkpoint_id,
    json_value_t **out_events, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_events) {
    *out_events = NULL;
  }
  if (!params_json || ((!run_id || !run_id[0]) && (!checkpoint_id || !checkpoint_id[0])) ||
      !out_events) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.loadHistoryEvents params");
    }
    return -1;
  }

  if (run_id && run_id[0] != '\0') {
    turbo_json_object_set_string(params_json, "run_id", run_id);
  }
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_json_object_set_string(params_json, "checkpoint_id", checkpoint_id);
  }
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.loadHistoryEvents",
                                                   params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_json_value_object(result_json, "events", out_events);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.loadHistoryEvents result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_run_trace_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *run_id,
    json_value_t **out_events, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_events) {
    *out_events = NULL;
  }
  if (!params_json || !run_id || !run_id[0] || !out_events) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getRunTraceEvents params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "run_id", run_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getRunTraceEvents",
                                                   params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_json_value_object(result_json, "events", out_events);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getRunTraceEvents result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value(
    turbo_agent_runtime_remote_client_t *client, const char *checkpoint_id,
    json_value_t **out_events, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_events) {
    *out_events = NULL;
  }
  if (!params_json || !checkpoint_id || !checkpoint_id[0] || !out_events) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build runtime.getCheckpointTraceEvents params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "checkpoint_id", checkpoint_id);
  rc = turbo_agent_runtime_remote_client_call_json(client, "runtime.getCheckpointTraceEvents",
                                                   params_json, &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_json_value_object(result_json, "events", out_events);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed runtime.getCheckpointTraceEvents result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_get_memory_record(
    turbo_agent_runtime_remote_client_t *client, const char *memory_namespace, const char *key,
    json_value_t **out_record_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_record_json) {
    *out_record_json = NULL;
  }
  if (!params_json || !memory_namespace || !memory_namespace[0] || !key || !key[0] ||
      !out_record_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.getRecord params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "memory_namespace", memory_namespace);
  turbo_json_object_set_string(params_json, "key", key);
  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.getRecord", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc != 0 ? rc : -1;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "record", out_record_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json =
        turbo_agent_runtime_remote_client_build_error_json(NULL, 1, "Malformed memory.getRecord result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_delete_memory_record(
    turbo_agent_runtime_remote_client_t *client, const char *memory_namespace, const char *key,
    json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (!params_json || !memory_namespace || !memory_namespace[0] || !key || !key[0]) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.deleteRecord params");
    }
    return -1;
  }

  turbo_json_object_set_string(params_json, "namespace", memory_namespace);
  turbo_json_object_set_string(params_json, "key", key);
  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.deleteRecord", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  if (!turbo_json_get_bool(result_json, "deleted", false)) {
    turbo_free_json(&result_json);
    if (out_error_json && !*out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Malformed memory.deleteRecord result");
    }
    return -1;
  }
  turbo_free_json(&result_json);
  return 0;
}

CXX_C_API int turbo_agent_runtime_remote_client_put_memory_record(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *record_json,
    json_value_t **out_record_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  json_value_t *record_clone = NULL;
  int rc;

  if (out_record_json) {
    *out_record_json = NULL;
  }
  if (!params_json || !record_json || turbo_json_type(record_json) != TURBO_JSON_OBJECT ||
      !out_record_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.putRecord params");
    }
    return -1;
  }

  record_clone = turbo_json_clone(record_json);
  if (!record_clone) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to clone memory record");
    }
    return -1;
  }
  turbo_json_object_add(params_json, "record", record_clone);
  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.putRecord", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "record", out_record_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json =
        turbo_agent_runtime_remote_client_build_error_json(NULL, 1, "Malformed memory.putRecord result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_validate_memory_record(
    turbo_agent_runtime_remote_client_t *client, const json_value_t *record_json, int *out_valid,
    json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  json_value_t *record_clone = NULL;
  const json_value_t *valid_json;
  int rc;

  if (out_valid) {
    *out_valid = 0;
  }
  if (!params_json || !record_json || turbo_json_type(record_json) != TURBO_JSON_OBJECT ||
      !out_valid) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.validateRecord params");
    }
    return -1;
  }

  record_clone = turbo_json_clone(record_json);
  if (!record_clone) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to clone memory record");
    }
    return -1;
  }
  turbo_json_object_add(params_json, "record", record_clone);
  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.validateRecord", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  valid_json = turbo_json_object_get(result_json, "valid");
  if (!valid_json || turbo_json_type(valid_json) != TURBO_JSON_BOOL) {
    turbo_free_json(&result_json);
    if (out_error_json && !*out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Malformed memory.validateRecord result");
    }
    return -1;
  }
  *out_valid = turbo_json_get_bool(result_json, "valid", false) ? 1 : 0;
  turbo_free_json(&result_json);
  return 0;
}

CXX_C_API int turbo_agent_runtime_remote_client_query_memory_records_ex(
    turbo_agent_runtime_remote_client_t *client, const turbo_agent_memory_query_options_t *options,
    json_value_t **out_records_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_agent_runtime_remote_client_build_memory_query_params_json(options);
  json_value_t *result_json = NULL;
  int rc;

  if (out_records_json) {
    *out_records_json = NULL;
  }
  if (!params_json || !out_records_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.queryRecordsEx params");
    }
    return -1;
  }

  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.queryRecordsEx", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "records", out_records_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed memory.queryRecordsEx result");
  }
  return rc;
}

CXX_C_API int turbo_agent_runtime_remote_client_query_memory_records(
    turbo_agent_runtime_remote_client_t *client, const char *namespace_prefix, const char *kind,
    const char *key_prefix, const char *text_substring, json_value_t **out_records_json,
    json_value_t **out_error_json) {
  turbo_agent_memory_query_options_t options = {0};

  options.namespace_prefix = namespace_prefix;
  options.kind = kind;
  options.key_prefix = key_prefix;
  options.text_substring = text_substring;
  return turbo_agent_runtime_remote_client_query_memory_records_ex(client, &options,
                                                                   out_records_json, out_error_json);
}

CXX_C_API int turbo_agent_runtime_remote_client_list_memory_records(
    turbo_agent_runtime_remote_client_t *client, const char *namespace_prefix,
    json_value_t **out_records_json, json_value_t **out_error_json) {
  json_value_t *params_json = turbo_json_create_object();
  json_value_t *result_json = NULL;
  int rc;

  if (out_records_json) {
    *out_records_json = NULL;
  }
  if (!params_json || !out_records_json) {
    turbo_free_json(&params_json);
    if (out_error_json) {
      *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
          NULL, 1, "Failed to build memory.listRecords params");
    }
    return -1;
  }
  if (namespace_prefix && namespace_prefix[0] != '\0') {
    turbo_json_object_set_string(params_json, "namespace_prefix", namespace_prefix);
  }

  rc = turbo_agent_runtime_remote_client_call_json(client, "memory.listRecords", params_json,
                                                   &result_json, out_error_json);
  turbo_free_json(&params_json);
  if (rc != 0 || !result_json) {
    turbo_free_json(&result_json);
    return rc;
  }
  rc = turbo_agent_runtime_remote_client_extract_object(result_json, "records", out_records_json);
  turbo_free_json(&result_json);
  if (rc != 0 && out_error_json && !*out_error_json) {
    *out_error_json = turbo_agent_runtime_remote_client_build_error_json(
        NULL, 1, "Malformed memory.listRecords result");
  }
  return rc;
}
