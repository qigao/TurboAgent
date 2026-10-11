#include "turbo_provider.h"

#include <string.h>

/* Consume value on both success and failure. */
static int turbo_provider_put(json_value_t *object, const char *key, json_value_t *value) {
  if (!value) return -1;
  if (turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    json_free(value);
    return -1;
  }
  return 0;
}

static int turbo_provider_structured_output(const turbo_provider_request_t *config,
                                              json_value_t *request) {
  json_value_t *schema;
  json_value_t *format;
  json_value_t *container;
  int chat = config->format != TURBO_PROVIDER_RESPONSES;
  const char *name = config->structured_output_name;
  const char *schema_text = config->structured_output_schema_json;

  if (config->format == TURBO_PROVIDER_ANTHROPIC || !schema_text || !schema_text[0]) return 0;
  schema = json_parse(schema_text, strlen(schema_text));
  if (!schema || json_type(schema) != JSON_OBJECT) {
    json_free(schema);
    return -1;
  }
  format = json_create_object();
  if (!format) {
    json_free(schema);
    return -1;
  }
  if ((!chat && turbo_provider_put(format, "type", json_create_string("json_schema")) != 0) ||
      turbo_provider_put(format, "name", json_create_string(name && name[0] ? name : "structured_output")) != 0) {
    json_free(schema);
    json_free(format);
    return -1;
  }
  if (turbo_provider_put(format, "schema", schema) != 0 ||
      turbo_provider_put(format, "strict", json_create_bool(config->structured_output_strict != 0)) != 0) {
    json_free(format);
    return -1;
  }
  container = json_create_object();
  if (!container) {
    json_free(format);
    return -1;
  }
  if (chat && turbo_provider_put(container, "type", json_create_string("json_schema")) != 0) {
    json_free(format);
    json_free(container);
    return -1;
  }
  if (turbo_provider_put(container, chat ? "json_schema" : "format", format) != 0) {
    json_free(container);
    return -1;
  }
  return turbo_provider_put(request, chat ? "response_format" : "text", container);
}

int turbo_provider_build_request(const turbo_provider_request_t *config, char **out_json) {
  json_value_t *request;
  int responses;
  int anthropic;
  if (!out_json) return -1;
  *out_json = NULL;
  if (!config || config->format < TURBO_PROVIDER_RESPONSES ||
      config->format > TURBO_PROVIDER_ANTHROPIC || !config->model ||
      !config->input || json_type(config->input) != JSON_ARRAY ||
      !config->tools || json_type(config->tools) != JSON_ARRAY) return -1;

  responses = config->format == TURBO_PROVIDER_RESPONSES;
  anthropic = config->format == TURBO_PROVIDER_ANTHROPIC;
  request = json_create_object();
  if (!request) return -1;
  if (turbo_provider_put(request, "model", json_create_string(config->model)) != 0 ||
      (config->stream && turbo_provider_put(request, "stream", json_create_bool(1)) != 0) ||
      (responses && config->previous_response_id &&
       turbo_provider_put(request, "previous_response_id", json_create_string(config->previous_response_id)) != 0) ||
      turbo_provider_put(request, responses ? "input" : "messages", json_clone(config->input)) != 0 ||
      ((responses || anthropic) && config->instructions && config->instructions[0] &&
       turbo_provider_put(request, responses ? "instructions" : "system", json_create_string(config->instructions)) != 0) ||
      ((responses || config->format == TURBO_PROVIDER_CHAT) && config->parallel_tool_calls &&
       turbo_provider_put(request, "parallel_tool_calls", json_create_bool(1)) != 0)) goto fail;

  if (json_array_size(config->tools) > 0) {
    if (turbo_provider_put(request, "tools", json_clone(config->tools)) != 0 ||
        (config->format == TURBO_PROVIDER_CHAT &&
         turbo_provider_put(request, "tool_choice", json_create_string("auto")) != 0)) goto fail;
  }
  if (turbo_provider_structured_output(config, request) != 0) goto fail;
  *out_json = json_serialize(request, NULL);
  json_free(request);
  return *out_json ? 0 : -1;
fail:
  json_free(request);
  return -1;
}
