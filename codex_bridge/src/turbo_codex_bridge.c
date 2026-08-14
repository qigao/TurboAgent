#include "turbo_codex_bridge.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <turbo_str.h>

#include "turbo_runtime_json.h"

enum {
  TURBO_CODEX_DEFAULT_MAX_MESSAGE_BYTES = 4 * 1024 * 1024,
  TURBO_CODEX_DEFAULT_RUN_TIMEOUT_MS = 30 * 60 * 1000
};

struct turbo_codex_client_s {
  turbo_codex_transport_t transport;
  tstr_t client_name;
  tstr_t client_title;
  tstr_t client_version;
  int experimental_api;
  size_t max_message_bytes;
  uint64_t initialize_timeout_ms;
  turbo_codex_event_fn event;
  void *event_user_data;
  turbo_codex_user_data_free_fn event_user_data_free;
  turbo_codex_server_request_fn server_request;
  void *server_request_user_data;
  turbo_codex_user_data_free_fn server_request_user_data_free;
  uint64_t next_request_id;
  int initialized;
  int closed;
  tstr_t last_error;
  tstr_t capture_thread_id;
  tstr_t capture_turn_id;
  tstr_t captured_text;
  json_value_t *completed_turn;
  int capture_error;
};

static char *turbo_codex_strdup(const char *text) {
  size_t length;
  char *copy;
  if (!text) return NULL;
  length = strlen(text);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1);
  if (!copy) return NULL;
  memcpy(copy, text, length + 1);
  return copy;
}

static void turbo_codex_set_error(turbo_codex_client_t *client, const char *message) {
  if (!client) return;
  tstr_free(client->last_error);
  client->last_error = tstr_dup(message ? message : "Codex bridge error");
}

static int turbo_codex_transport_valid(const turbo_codex_transport_t *transport) {
  return transport && transport->struct_size >= sizeof(*transport) &&
         transport->abi_version == TURBO_CODEX_TRANSPORT_ABI_VERSION && transport->write &&
         transport->read;
}

void turbo_codex_transport_release(turbo_codex_transport_t *transport) {
  if (!transport) return;
  if (transport->close) transport->close(transport->user_data);
  if (transport->user_data_free) transport->user_data_free(transport->user_data);
  memset(transport, 0, sizeof(*transport));
}

void turbo_codex_client_config_init(turbo_codex_client_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_CODEX_BRIDGE_ABI_VERSION;
  config->transport.struct_size = sizeof(config->transport);
  config->transport.abi_version = TURBO_CODEX_TRANSPORT_ABI_VERSION;
  config->client_name = "turbo_agent";
  config->client_title = "TurboAgent";
  config->client_version = "1.0.0";
  config->max_message_bytes = TURBO_CODEX_DEFAULT_MAX_MESSAGE_BYTES;
  config->initialize_timeout_ms = 30U * 1000U;
}

turbo_codex_client_t *turbo_codex_client_create(const turbo_codex_client_config_t *config) {
  turbo_codex_client_t *client;
  if (!config || config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_CODEX_BRIDGE_ABI_VERSION ||
      !turbo_codex_transport_valid(&config->transport) || !config->client_name ||
      !config->client_name[0] || !config->client_title || !config->client_title[0] ||
      !config->client_version || !config->client_version[0] || !config->max_message_bytes ||
      !config->initialize_timeout_ms) {
    return NULL;
  }
  client = (turbo_codex_client_t *)calloc(1, sizeof(*client));
  if (!client) return NULL;
  client->client_name = tstr_dup(config->client_name);
  client->client_title = tstr_dup(config->client_title);
  client->client_version = tstr_dup(config->client_version);
  client->captured_text = tstr_new();
  if (!client->client_name || !client->client_title || !client->client_version ||
      !client->captured_text) {
    tstr_free(client->client_name);
    tstr_free(client->client_title);
    tstr_free(client->client_version);
    tstr_free(client->captured_text);
    free(client);
    return NULL;
  }
  client->transport = config->transport;
  client->experimental_api = config->experimental_api ? 1 : 0;
  client->max_message_bytes = config->max_message_bytes;
  client->initialize_timeout_ms = config->initialize_timeout_ms;
  client->event = config->event;
  client->event_user_data = config->event_user_data;
  client->event_user_data_free = config->event_user_data_free;
  client->server_request = config->server_request;
  client->server_request_user_data = config->server_request_user_data;
  client->server_request_user_data_free = config->server_request_user_data_free;
  client->next_request_id = 1;
  return client;
}

void turbo_codex_client_destroy(turbo_codex_client_t *client) {
  if (!client) return;
  if (!client->closed) turbo_codex_transport_release(&client->transport);
  client->closed = 1;
  if (client->event_user_data_free) client->event_user_data_free(client->event_user_data);
  if (client->server_request_user_data_free) {
    client->server_request_user_data_free(client->server_request_user_data);
  }
  tstr_free(client->client_name);
  tstr_free(client->client_title);
  tstr_free(client->client_version);
  tstr_free(client->last_error);
  tstr_free(client->capture_thread_id);
  tstr_free(client->capture_turn_id);
  tstr_free(client->captured_text);
  turbo_runtime_json_destroy(client->completed_turn);
  free(client);
}

static int turbo_codex_send_json(turbo_codex_client_t *client, const json_value_t *message) {
  char *serialized = NULL;
  tstr_t frame = NULL;
  size_t length = 0;
  int rc;
  if (!client || !message || client->closed) return TURBO_ESHUTDOWN;
  serialized = turbo_json_serialize(message, &length);
  if (!serialized) return TURBO_ENOMEM;
  if (length > client->max_message_bytes) {
    turbo_json_serialize_free(serialized);
    turbo_codex_set_error(client, "outbound Codex message exceeds max_message_bytes");
    return TURBO_EMSGSIZE;
  }
  frame = tstr_new_len(serialized, length);
  turbo_json_serialize_free(serialized);
  if (!frame || !(frame = tstr_cat(frame, "\n"))) {
    tstr_free(frame);
    return TURBO_ENOMEM;
  }
  rc = client->transport.write((const uint8_t *)frame, tstr_len(frame),
                               client->transport.user_data);
  tstr_free(frame);
  if (rc != TURBO_OK) turbo_codex_set_error(client, "Codex transport write failed");
  return rc;
}

static int turbo_codex_send_notification(turbo_codex_client_t *client, const char *method,
                                         const json_value_t *params) {
  json_value_t *message = NULL;
  json_value_t *params_copy = NULL;
  int rc;
  if (!client || !method || !method[0]) return TURBO_EINVAL;
  message = turbo_json_create_object();
  params_copy = params ? turbo_json_clone(params) : turbo_json_create_object();
  if (!message || !params_copy) {
    turbo_runtime_json_destroy(message);
    turbo_runtime_json_destroy(params_copy);
    return TURBO_ENOMEM;
  }
  turbo_json_object_set_string(message, "method", method);
  if (!turbo_json_object_add_checked(message, "params", params_copy)) {
    turbo_runtime_json_destroy(params_copy);
    turbo_runtime_json_destroy(message);
    return TURBO_ENOMEM;
  }
  rc = turbo_codex_send_json(client, message);
  turbo_runtime_json_destroy(message);
  return rc;
}

static const char *turbo_codex_message_id(const json_value_t *message) {
  const json_value_t *id = turbo_json_object_get(message, "id");
  return id && turbo_json_type(id) == TURBO_JSON_STRING ? turbo_json_string(id) : NULL;
}

static int turbo_codex_send_server_response(turbo_codex_client_t *client,
                                            const json_value_t *request_id,
                                            json_value_t *result, int error_code,
                                            const char *error_message) {
  json_value_t *response = turbo_json_create_object();
  json_value_t *id_copy = turbo_json_clone(request_id);
  json_value_t *error = NULL;
  if (!response || !id_copy) {
    turbo_runtime_json_destroy(response);
    turbo_runtime_json_destroy(id_copy);
    turbo_runtime_json_destroy(result);
    return TURBO_ENOMEM;
  }
  if (!turbo_json_object_add_checked(response, "id", id_copy)) {
    turbo_runtime_json_destroy(id_copy);
    turbo_runtime_json_destroy(response);
    turbo_runtime_json_destroy(result);
    return TURBO_ENOMEM;
  }
  if (error_code) {
    error = turbo_json_create_object();
    if (!error) {
      turbo_runtime_json_destroy(response);
      turbo_runtime_json_destroy(result);
      return TURBO_ENOMEM;
    }
    turbo_json_object_set_number(error, "code", (double)error_code);
    turbo_json_object_set_string(error, "message", error_message ? error_message : "Request failed");
    if (!turbo_json_object_add_checked(response, "error", error)) {
      turbo_runtime_json_destroy(error);
      turbo_runtime_json_destroy(response);
      turbo_runtime_json_destroy(result);
      return TURBO_ENOMEM;
    }
    turbo_runtime_json_destroy(result);
  } else {
    if (!result) result = turbo_json_create_object();
    if (!result || !turbo_json_object_add_checked(response, "result", result)) {
      turbo_runtime_json_destroy(result);
      turbo_runtime_json_destroy(response);
      return TURBO_ENOMEM;
    }
  }
  error_code = turbo_codex_send_json(client, response);
  turbo_runtime_json_destroy(response);
  return error_code;
}

static int turbo_codex_is_approval_method(const char *method) {
  return method && (!strcmp(method, "item/commandExecution/requestApproval") ||
                    !strcmp(method, "item/fileChange/requestApproval") ||
                    !strcmp(method, "execCommandApproval") ||
                    !strcmp(method, "applyPatchApproval"));
}

static int turbo_codex_handle_server_request(turbo_codex_client_t *client,
                                             const json_value_t *message,
                                             const char *method,
                                             const json_value_t *params) {
  const json_value_t *id = turbo_json_object_get(message, "id");
  json_value_t *result = NULL;
  int rc;
  if (!id) return TURBO_EPROTO;
  if (client->server_request) {
    rc = client->server_request(method, params, &result, client->server_request_user_data);
    if (rc == TURBO_OK && result) {
      return turbo_codex_send_server_response(client, id, result, 0, NULL);
    }
    turbo_runtime_json_destroy(result);
    return turbo_codex_send_server_response(client, id, NULL, -32000,
                                             "TurboAgent rejected the server request");
  }
  if (turbo_codex_is_approval_method(method)) {
    result = turbo_json_create_object();
    if (!result) return TURBO_ENOMEM;
    turbo_json_object_set_string(result, "decision", "decline");
    return turbo_codex_send_server_response(client, id, result, 0, NULL);
  }
  return turbo_codex_send_server_response(client, id, NULL, -32601, "Method not found");
}

static void turbo_codex_capture_notification(turbo_codex_client_t *client, const char *method,
                                             const json_value_t *params) {
  const char *thread_id;
  const char *turn_id;
  const char *delta;
  const json_value_t *turn;
  if (!client || !method || !params || turbo_json_type(params) != TURBO_JSON_OBJECT) return;
  thread_id = turbo_json_get_string(params, "threadId");
  turn_id = turbo_json_get_string(params, "turnId");
  if (!strcmp(method, "item/agentMessage/delta") && client->capture_thread_id && thread_id &&
      !strcmp(client->capture_thread_id, thread_id) &&
      (!client->capture_turn_id || (turn_id && !strcmp(client->capture_turn_id, turn_id)))) {
    delta = turbo_json_get_string(params, "delta");
    if (delta && client->captured_text) {
      size_t current_length = tstr_len(client->captured_text);
      size_t delta_length = strlen(delta);
      if (delta_length > client->max_message_bytes - current_length) {
        client->capture_error = TURBO_EMSGSIZE;
        turbo_codex_set_error(client, "Codex agent message exceeds max_message_bytes");
      } else {
        tstr_t appended = tstr_cat_len(client->captured_text, delta, delta_length);
        if (!appended) {
          client->capture_error = TURBO_ENOMEM;
          turbo_codex_set_error(client, "failed to capture Codex agent message");
        } else {
          client->captured_text = appended;
        }
      }
    }
  }
  if (strcmp(method, "turn/completed")) return;
  turn = turbo_json_object_get(params, "turn");
  if (!turn || turbo_json_type(turn) != TURBO_JSON_OBJECT) return;
  turn_id = turbo_json_get_string(turn, "id");
  thread_id = turbo_json_get_string(params, "threadId");
  if (client->capture_thread_id && thread_id &&
      !strcmp(client->capture_thread_id, thread_id) && turn_id &&
      (!client->capture_turn_id || !strcmp(client->capture_turn_id, turn_id))) {
    turbo_runtime_json_destroy(client->completed_turn);
    client->completed_turn = turbo_json_clone(turn);
    if (!client->completed_turn) {
      client->capture_error = TURBO_ENOMEM;
      turbo_codex_set_error(client, "failed to capture completed Codex turn");
    }
  }
}

static int turbo_codex_dispatch_incoming(turbo_codex_client_t *client, json_value_t *message,
                                         const char *expected_id, json_value_t **out_result,
                                         int *out_matched) {
  const char *method;
  const char *message_id;
  const json_value_t *params;
  const json_value_t *error;
  const json_value_t *result;
  const char *error_message;
  if (!client || !message || turbo_json_type(message) != TURBO_JSON_OBJECT || !out_matched) {
    return TURBO_EPROTO;
  }
  *out_matched = 0;
  method = turbo_json_get_string(message, "method");
  message_id = turbo_codex_message_id(message);
  params = turbo_json_object_get(message, "params");
  if (method) {
    if (turbo_json_object_get(message, "id")) {
      return turbo_codex_handle_server_request(client, message, method, params);
    }
    turbo_codex_capture_notification(client, method, params);
    if (client->event) client->event(method, params, client->event_user_data);
    return TURBO_OK;
  }
  if (!expected_id || !message_id || strcmp(expected_id, message_id)) {
    turbo_codex_set_error(client, "unexpected Codex response id");
    return TURBO_EPROTO;
  }
  error = turbo_json_object_get(message, "error");
  if (error) {
    error_message = turbo_json_get_string(error, "message");
    turbo_codex_set_error(client, error_message ? error_message : "Codex request failed");
    *out_matched = 1;
    return TURBO_EIO;
  }
  result = turbo_json_object_get(message, "result");
  if (!result) {
    turbo_codex_set_error(client, "Codex response contains neither result nor error");
    return TURBO_EPROTO;
  }
  if (out_result) {
    *out_result = turbo_json_clone(result);
    if (!*out_result) return TURBO_ENOMEM;
  }
  *out_matched = 1;
  return TURBO_OK;
}

static int turbo_codex_read_message(turbo_codex_client_t *client, uint64_t timeout_ms,
                                    json_value_t **out_message) {
  char *line = NULL;
  size_t length;
  int rc;
  if (!client || !out_message) return TURBO_EINVAL;
  *out_message = NULL;
  rc = client->transport.read(timeout_ms, &line, client->transport.user_data);
  if (rc != TURBO_OK) {
    if (rc != TURBO_ETIMEDOUT) turbo_codex_set_error(client, "Codex transport read failed");
    free(line);
    return rc;
  }
  if (!line) return TURBO_EPROTO;
  length = strlen(line);
  if (!length || length > client->max_message_bytes || strchr(line, '\r') || strchr(line, '\n')) {
    free(line);
    turbo_codex_set_error(client, "invalid or oversized Codex JSONL frame");
    return length > client->max_message_bytes ? TURBO_EMSGSIZE : TURBO_EPROTO;
  }
  if (turbo_parse_json((const uint8_t *)line, length, out_message) != 0 || !*out_message) {
    free(line);
    turbo_codex_set_error(client, "invalid JSON from Codex app-server");
    return TURBO_EPROTO;
  }
  free(line);
  return TURBO_OK;
}

int turbo_codex_client_request(turbo_codex_client_t *client, const char *method,
                               const json_value_t *params, uint64_t timeout_ms,
                               json_value_t **out_result) {
  json_value_t *request = NULL;
  json_value_t *params_copy = NULL;
  json_value_t *message = NULL;
  char request_id[48];
  uint64_t deadline = 0;
  int matched = 0;
  int rc;
  if (out_result) *out_result = NULL;
  if (!client || !method || !method[0] || client->closed ||
      (params && turbo_json_type(params) != TURBO_JSON_OBJECT)) {
    return TURBO_EINVAL;
  }
  if (!client->initialized && strcmp(method, "initialize")) return TURBO_EBUSY;
  if (client->next_request_id == UINT64_MAX) return TURBO_ERANGE;
  if (snprintf(request_id, sizeof(request_id), "turbo-%llu",
               (unsigned long long)client->next_request_id++) <= 0) {
    return TURBO_EIO;
  }
  request = turbo_json_create_object();
  params_copy = params ? turbo_json_clone(params) : turbo_json_create_object();
  if (!request || !params_copy) {
    turbo_runtime_json_destroy(request);
    turbo_runtime_json_destroy(params_copy);
    return TURBO_ENOMEM;
  }
  turbo_json_object_set_string(request, "method", method);
  turbo_json_object_set_string(request, "id", request_id);
  if (!turbo_json_object_add_checked(request, "params", params_copy)) {
    turbo_runtime_json_destroy(params_copy);
    turbo_runtime_json_destroy(request);
    return TURBO_ENOMEM;
  }
  rc = turbo_codex_send_json(client, request);
  turbo_runtime_json_destroy(request);
  if (rc != TURBO_OK) return rc;
  if (timeout_ms != UINT64_MAX) {
    uint64_t now = turbo_monotonic_ms();
    deadline = timeout_ms > UINT64_MAX - now ? UINT64_MAX : now + timeout_ms;
  }
  while (!matched) {
    uint64_t remaining = UINT64_MAX;
    if (timeout_ms != UINT64_MAX) {
      uint64_t now = turbo_monotonic_ms();
      if (now >= deadline) return TURBO_ETIMEDOUT;
      remaining = deadline - now;
    }
    rc = turbo_codex_read_message(client, remaining, &message);
    if (rc != TURBO_OK) return rc;
    rc = turbo_codex_dispatch_incoming(client, message, request_id, out_result, &matched);
    turbo_runtime_json_destroy(message);
    message = NULL;
    if (rc != TURBO_OK) return rc;
  }
  return TURBO_OK;
}

int turbo_codex_client_pump(turbo_codex_client_t *client, uint64_t timeout_ms) {
  json_value_t *message = NULL;
  int matched = 0;
  int rc;
  if (!client || !client->initialized || client->closed) return TURBO_EINVAL;
  rc = turbo_codex_read_message(client, timeout_ms, &message);
  if (rc != TURBO_OK) return rc;
  rc = turbo_codex_dispatch_incoming(client, message, NULL, NULL, &matched);
  turbo_runtime_json_destroy(message);
  return rc;
}

int turbo_codex_client_initialize(turbo_codex_client_t *client,
                                  json_value_t **out_server_info) {
  json_value_t *params = NULL;
  json_value_t *client_info = NULL;
  json_value_t *capabilities = NULL;
  json_value_t *initialized_params = NULL;
  int rc;
  if (out_server_info) *out_server_info = NULL;
  if (!client || client->closed) return TURBO_EINVAL;
  if (client->initialized) return TURBO_EALREADY;
  params = turbo_json_create_object();
  client_info = turbo_json_create_object();
  capabilities = turbo_json_create_object();
  if (!params || !client_info || !capabilities) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  turbo_json_object_set_string(client_info, "name", client->client_name);
  turbo_json_object_set_string(client_info, "title", client->client_title);
  turbo_json_object_set_string(client_info, "version", client->client_version);
  turbo_json_object_set_bool(capabilities, "experimentalApi", client->experimental_api != 0);
  if (!turbo_json_object_add_checked(params, "clientInfo", client_info)) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  client_info = NULL;
  if (!turbo_json_object_add_checked(params, "capabilities", capabilities)) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  capabilities = NULL;
  rc = turbo_codex_client_request(client, "initialize", params,
                                  client->initialize_timeout_ms, out_server_info);
  if (rc != TURBO_OK) goto cleanup;
  client->initialized = 1;
  initialized_params = turbo_json_create_object();
  if (!initialized_params) {
    client->initialized = 0;
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  rc = turbo_codex_send_notification(client, "initialized", initialized_params);
  if (rc != TURBO_OK) {
    client->initialized = 0;
    if (out_server_info) {
      turbo_runtime_json_destroy(*out_server_info);
      *out_server_info = NULL;
    }
  }

cleanup:
  turbo_runtime_json_destroy(params);
  turbo_runtime_json_destroy(client_info);
  turbo_runtime_json_destroy(capabilities);
  turbo_runtime_json_destroy(initialized_params);
  return rc;
}

void turbo_codex_run_options_init(turbo_codex_run_options_t *options) {
  if (!options) return;
  memset(options, 0, sizeof(*options));
  options->struct_size = sizeof(*options);
  options->abi_version = TURBO_CODEX_BRIDGE_ABI_VERSION;
  options->sandbox = "workspace-write";
  options->approval_policy = "on-request";
  options->ephemeral_thread = 1;
  options->timeout_ms = TURBO_CODEX_DEFAULT_RUN_TIMEOUT_MS;
}

static uint64_t turbo_codex_remaining(uint64_t deadline) {
  uint64_t now;
  if (deadline == UINT64_MAX) return UINT64_MAX;
  now = turbo_monotonic_ms();
  return now >= deadline ? 0 : deadline - now;
}

static int turbo_codex_turn_status(const json_value_t *turn) {
  const char *status = turn ? turbo_json_get_string(turn, "status") : NULL;
  if (!status) return TURBO_EPROTO;
  if (!strcmp(status, "completed")) return TURBO_OK;
  if (!strcmp(status, "interrupted")) return TURBO_ECANCELED;
  return TURBO_EIO;
}

int turbo_codex_client_run_text(turbo_codex_client_t *client, const char *prompt,
                                const turbo_codex_run_options_t *options,
                                char **out_thread_id, char **out_turn_id, char **out_text,
                                json_value_t **out_turn) {
  turbo_codex_run_options_t defaults;
  const turbo_codex_run_options_t *run_options = options;
  json_value_t *params = NULL;
  json_value_t *result = NULL;
  json_value_t *input = NULL;
  json_value_t *input_item = NULL;
  const json_value_t *thread;
  const json_value_t *turn;
  const char *thread_id = NULL;
  const char *turn_id = NULL;
  tstr_t owned_thread_id = NULL;
  tstr_t owned_turn_id = NULL;
  uint64_t deadline;
  uint64_t remaining;
  int rc = TURBO_OK;
  if (out_thread_id) *out_thread_id = NULL;
  if (out_turn_id) *out_turn_id = NULL;
  if (out_text) *out_text = NULL;
  if (out_turn) *out_turn = NULL;
  if (!client || !prompt || !prompt[0]) return TURBO_EINVAL;
  if (!run_options) {
    turbo_codex_run_options_init(&defaults);
    run_options = &defaults;
  }
  if (run_options->struct_size < sizeof(*run_options) ||
      run_options->abi_version != TURBO_CODEX_BRIDGE_ABI_VERSION ||
      !run_options->timeout_ms) {
    return TURBO_EINVAL;
  }
  if (!client->initialized) {
    rc = turbo_codex_client_initialize(client, NULL);
    if (rc != TURBO_OK) return rc;
  }
  if (run_options->timeout_ms == UINT64_MAX) {
    deadline = UINT64_MAX;
  } else {
    uint64_t now = turbo_monotonic_ms();
    deadline = run_options->timeout_ms > UINT64_MAX - now
                   ? UINT64_MAX
                   : now + run_options->timeout_ms;
  }
  params = turbo_json_create_object();
  if (!params) return TURBO_ENOMEM;
  if (run_options->thread_id && run_options->thread_id[0]) {
    turbo_json_object_set_string(params, "threadId", run_options->thread_id);
    remaining = turbo_codex_remaining(deadline);
    if (!remaining) {
      rc = TURBO_ETIMEDOUT;
      goto cleanup;
    }
    rc = turbo_codex_client_request(client, "thread/resume", params, remaining, &result);
  } else {
    if (run_options->model && run_options->model[0]) {
      turbo_json_object_set_string(params, "model", run_options->model);
    }
    if (run_options->cwd && run_options->cwd[0]) {
      turbo_json_object_set_string(params, "cwd", run_options->cwd);
    }
    if (run_options->sandbox && run_options->sandbox[0]) {
      turbo_json_object_set_string(params, "sandbox", run_options->sandbox);
    }
    if (run_options->approval_policy && run_options->approval_policy[0]) {
      turbo_json_object_set_string(params, "approvalPolicy", run_options->approval_policy);
    }
    turbo_json_object_set_bool(params, "ephemeral", run_options->ephemeral_thread != 0);
    remaining = turbo_codex_remaining(deadline);
    if (!remaining) {
      rc = TURBO_ETIMEDOUT;
      goto cleanup;
    }
    rc = turbo_codex_client_request(client, "thread/start", params, remaining, &result);
  }
  if (rc != TURBO_OK) goto cleanup;
  thread = turbo_json_object_get(result, "thread");
  thread_id = thread ? turbo_json_get_string(thread, "id") : NULL;
  if (!thread_id || !thread_id[0]) {
    rc = TURBO_EPROTO;
    turbo_codex_set_error(client, "Codex thread response has no thread id");
    goto cleanup;
  }
  owned_thread_id = tstr_dup(thread_id);
  if (!owned_thread_id) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  turbo_runtime_json_destroy(params);
  turbo_runtime_json_destroy(result);
  params = turbo_json_create_object();
  input = turbo_json_create_array();
  input_item = turbo_json_create_object();
  if (!params || !input || !input_item) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  turbo_json_object_set_string(input_item, "type", "text");
  turbo_json_object_set_string(input_item, "text", prompt);
  if (!turbo_json_array_add_checked(input, input_item)) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  input_item = NULL;
  turbo_json_object_set_string(params, "threadId", owned_thread_id);
  if (!turbo_json_object_add_checked(params, "input", input)) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  input = NULL;
  tstr_free(client->capture_thread_id);
  tstr_free(client->capture_turn_id);
  client->capture_thread_id = tstr_dup(owned_thread_id);
  client->capture_turn_id = NULL;
  tstr_clear(client->captured_text);
  turbo_runtime_json_destroy(client->completed_turn);
  client->completed_turn = NULL;
  client->capture_error = TURBO_OK;
  if (!client->capture_thread_id) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  remaining = turbo_codex_remaining(deadline);
  if (!remaining) {
    rc = TURBO_ETIMEDOUT;
    goto cleanup;
  }
  rc = turbo_codex_client_request(client, "turn/start", params, remaining, &result);
  if (rc != TURBO_OK) goto cleanup;
  if (client->capture_error != TURBO_OK) {
    rc = client->capture_error;
    goto cleanup;
  }
  turn = turbo_json_object_get(result, "turn");
  turn_id = turn ? turbo_json_get_string(turn, "id") : NULL;
  if (!turn_id || !turn_id[0]) {
    rc = TURBO_EPROTO;
    turbo_codex_set_error(client, "Codex turn response has no turn id");
    goto cleanup;
  }
  owned_turn_id = tstr_dup(turn_id);
  client->capture_turn_id = tstr_dup(turn_id);
  if (!owned_turn_id || !client->capture_turn_id) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  if (client->completed_turn) {
    const char *completed_id = turbo_json_get_string(client->completed_turn, "id");
    if (!completed_id || strcmp(completed_id, client->capture_turn_id)) {
      turbo_runtime_json_destroy(client->completed_turn);
      client->completed_turn = NULL;
    }
  }
  while (!client->completed_turn) {
    remaining = turbo_codex_remaining(deadline);
    if (!remaining) {
      rc = TURBO_ETIMEDOUT;
      goto cleanup;
    }
    rc = turbo_codex_client_pump(client, remaining);
    if (rc != TURBO_OK) goto cleanup;
    if (client->capture_error != TURBO_OK) {
      rc = client->capture_error;
      goto cleanup;
    }
  }
  rc = turbo_codex_turn_status(client->completed_turn);
  if (rc != TURBO_OK) turbo_codex_set_error(client, "Codex turn did not complete successfully");
  if (out_thread_id) {
    *out_thread_id = turbo_codex_strdup(owned_thread_id);
    if (!*out_thread_id) rc = TURBO_ENOMEM;
  }
  if (rc == TURBO_OK && out_turn_id) {
    *out_turn_id = turbo_codex_strdup(owned_turn_id);
    if (!*out_turn_id) rc = TURBO_ENOMEM;
  }
  if (rc == TURBO_OK && out_text) {
    *out_text = turbo_codex_strdup(client->captured_text ? client->captured_text : "");
    if (!*out_text) rc = TURBO_ENOMEM;
  }
  if (rc == TURBO_OK && out_turn) {
    *out_turn = turbo_json_clone(client->completed_turn);
    if (!*out_turn) rc = TURBO_ENOMEM;
  }

cleanup:
  if (rc != TURBO_OK) {
    if (out_thread_id) {
      free(*out_thread_id);
      *out_thread_id = NULL;
    }
    if (out_turn_id) {
      free(*out_turn_id);
      *out_turn_id = NULL;
    }
    if (out_text) {
      free(*out_text);
      *out_text = NULL;
    }
    if (out_turn) {
      turbo_runtime_json_destroy(*out_turn);
      *out_turn = NULL;
    }
  }
  turbo_runtime_json_destroy(params);
  turbo_runtime_json_destroy(result);
  turbo_runtime_json_destroy(input);
  turbo_runtime_json_destroy(input_item);
  tstr_free(owned_thread_id);
  tstr_free(owned_turn_id);
  tstr_free(client->capture_thread_id);
  tstr_free(client->capture_turn_id);
  client->capture_thread_id = NULL;
  client->capture_turn_id = NULL;
  return rc;
}

const char *turbo_codex_client_last_error(const turbo_codex_client_t *client) {
  return client && client->last_error ? client->last_error : "";
}
