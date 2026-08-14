#include "turbo_agent_harness_transport.h"

#include "turbo_agent_harness_server_internal.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>
#include <turbo_thread.h>

enum { TURBO_AGENT_HARNESS_JSONL_DEFAULT_EVENT_BATCH = 64 };

struct turbo_agent_harness_jsonl_transport_s {
  turbo_agent_harness_jsonl_transport_config_t config;
  turbo_mutex_t output_mutex;
};

static int turbo_agent_harness_jsonl_transport_config_valid(
    const turbo_agent_harness_jsonl_transport_config_t *config) {
  return config && config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_AGENT_HARNESS_JSONL_TRANSPORT_ABI_VERSION &&
         config->connection && config->write && config->max_event_batch > 0;
}

static int
turbo_agent_harness_jsonl_write_serialized(turbo_agent_harness_jsonl_transport_t *transport,
                                           const char *json_text) {
  uint8_t *frame;
  size_t json_size;
  int rc;

  if (!transport || !json_text) return TURBO_EINVAL;
  json_size = strlen(json_text);
  if (json_size == SIZE_MAX) return TURBO_EFBIG;
  frame = (uint8_t *)malloc(json_size + 1);
  if (!frame) return TURBO_ENOMEM;
  memcpy(frame, json_text, json_size);
  frame[json_size] = '\n';
  turbo_mutex_lock(&transport->output_mutex);
  rc = transport->config.write(frame, json_size + 1, transport->config.write_user_data);
  turbo_mutex_unlock(&transport->output_mutex);
  free(frame);
  return rc;
}

void turbo_agent_harness_jsonl_transport_config_init(
    turbo_agent_harness_jsonl_transport_config_t *config) {
  if (!config) return;
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->abi_version = TURBO_AGENT_HARNESS_JSONL_TRANSPORT_ABI_VERSION;
  config->max_event_batch = TURBO_AGENT_HARNESS_JSONL_DEFAULT_EVENT_BATCH;
}

turbo_agent_harness_jsonl_transport_t *turbo_agent_harness_jsonl_transport_create(
    const turbo_agent_harness_jsonl_transport_config_t *config) {
  turbo_agent_harness_jsonl_transport_t *transport;
  if (!turbo_agent_harness_jsonl_transport_config_valid(config)) return NULL;
  transport = (turbo_agent_harness_jsonl_transport_t *)calloc(1, sizeof(*transport));
  if (!transport) return NULL;
  transport->config = *config;
  transport->config.connection = turbo_agent_harness_connection_retain_internal(config->connection);
  if (!transport->config.connection) {
    free(transport);
    return NULL;
  }
  turbo_mutex_init(&transport->output_mutex);
  if (!transport->output_mutex) {
    turbo_agent_harness_connection_release_internal(transport->config.connection);
    free(transport);
    return NULL;
  }
  return transport;
}

void turbo_agent_harness_jsonl_transport_destroy(turbo_agent_harness_jsonl_transport_t *transport) {
  if (!transport) return;
  turbo_mutex_destroy(&transport->output_mutex);
  if (transport->config.write_user_data_free) {
    transport->config.write_user_data_free(transport->config.write_user_data);
  }
  turbo_agent_harness_connection_release_internal(transport->config.connection);
  free(transport);
}

int turbo_agent_harness_jsonl_transport_dispatch_line(
    turbo_agent_harness_jsonl_transport_t *transport, const char *request_json_line) {
  char *response_json = NULL;
  int rc;

  if (!transport || !request_json_line) return TURBO_EINVAL;
  if (strchr(request_json_line, '\r') || strchr(request_json_line, '\n')) return TURBO_EPROTO;
  rc = turbo_agent_harness_connection_dispatch_text(transport->config.connection, request_json_line,
                                                    &response_json);
  if (rc != TURBO_OK || !response_json) return rc;
  rc = turbo_agent_harness_jsonl_write_serialized(transport, response_json);
  turbo_json_serialize_free(response_json);
  return rc;
}

int turbo_agent_harness_jsonl_transport_pump_events(
    turbo_agent_harness_jsonl_transport_t *transport, uint64_t timeout_ms,
    size_t *out_event_count) {
  size_t event_count = 0;
  int rc = TURBO_OK;

  if (!transport || !out_event_count) return TURBO_EINVAL;
  *out_event_count = 0;
  while (event_count < transport->config.max_event_batch) {
    json_value_t *event_json = NULL;
    char *event_text;
    uint64_t sequence = 0;
    uint64_t wait_ms = event_count == 0 ? timeout_ms : 0;

    rc = turbo_agent_harness_connection_wait_event_json_value(transport->config.connection, wait_ms,
                                                              &event_json, &sequence);
    if (rc == TURBO_ETIMEDOUT && event_count > 0) {
      rc = TURBO_OK;
      break;
    }
    if (rc != TURBO_OK) break;
    event_text = turbo_json_serialize(event_json, NULL);
    if (!event_text) {
      turbo_runtime_json_destroy(event_json);
      rc = TURBO_ENOMEM;
      break;
    }
    rc = turbo_agent_harness_jsonl_write_serialized(transport, event_text);
    turbo_json_serialize_free(event_text);
    turbo_runtime_json_destroy(event_json);
    if (rc != TURBO_OK) break;
    rc = turbo_agent_harness_connection_ack_events(transport->config.connection, sequence);
    if (rc != TURBO_OK) break;
    ++event_count;
  }
  *out_event_count = event_count;
  return rc;
}
