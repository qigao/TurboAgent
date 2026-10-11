#include "turbo_model_provider.h"
#include "turbo_provider.h"
#include <string.h>

/* Preserve builtin identity and custom callback behavior at the Agent boundary. */
static turbo_provider_format_t turbo_model_provider_format(const turbo_model_provider_t *provider) {
  if (provider == turbo_model_provider_openai_responses()) return TURBO_PROVIDER_RESPONSES;
  if (provider == turbo_model_provider_openai_chat_completions()) return TURBO_PROVIDER_CHAT;
  if (provider == turbo_model_provider_openai_compatible_chat_completions()) return TURBO_PROVIDER_COMPATIBLE_CHAT;
  if (provider == turbo_model_provider_anthropic_messages()) return TURBO_PROVIDER_ANTHROPIC;
  return TURBO_PROVIDER_UNKNOWN;
}

const turbo_model_provider_t *turbo_model_provider_by_name(const char *name) {
  if (!name || name[0] == '\0') {
    return NULL;
  }

  if (strcmp(name, "openai") == 0 || strcmp(name, "openai_responses") == 0 ||
      strcmp(name, "responses") == 0) {
    return turbo_model_provider_openai_responses();
  }

  if (strcmp(name, "openai_chat_completions") == 0 || strcmp(name, "chat_completions") == 0) {
    return turbo_model_provider_openai_chat_completions();
  }

  if (strcmp(name, "openai_compatible_chat_completions") == 0 ||
      strcmp(name, "openai_compatible_chat") == 0) {
    return turbo_model_provider_openai_compatible_chat_completions();
  }

  if (strcmp(name, "anthropic_messages") == 0 || strcmp(name, "anthropic") == 0) {
    return turbo_model_provider_anthropic_messages();
  }

  return NULL;
}

int turbo_model_provider_is_legacy_chat(const turbo_model_provider_t *provider) {
  return provider == turbo_model_provider_openai_chat_completions() ||
                 provider == turbo_model_provider_openai_compatible_chat_completions()
             ? 1
             : 0;
}

int turbo_model_provider_is_anthropic_messages(const turbo_model_provider_t *provider) {
  return provider == turbo_model_provider_anthropic_messages() ? 1 : 0;
}

const char *turbo_model_provider_select_api_key(const turbo_model_provider_t *provider,
                                                const char *openai_api_key,
                                                const char *anthropic_auth_token) {
  if (turbo_model_provider_is_anthropic_messages(provider) && anthropic_auth_token &&
      anthropic_auth_token[0] != '\0') {
    return anthropic_auth_token;
  }

  return openai_api_key;
}

const char *turbo_model_provider_select_base_url(const turbo_model_provider_t *provider,
                                                 const char *openai_base_url,
                                                 const char *anthropic_base_url,
                                                 const char *default_openai_base_url) {
  if (turbo_model_provider_is_anthropic_messages(provider) && anthropic_base_url &&
      anthropic_base_url[0] != '\0') {
    return anthropic_base_url;
  }

  if (openai_base_url && openai_base_url[0] != '\0') {
    return openai_base_url;
  }

  return default_openai_base_url;
}

const char *turbo_model_provider_select_endpoint_path(
    const turbo_model_provider_t *provider, const char *endpoint_path_override) {
  if (endpoint_path_override && endpoint_path_override[0] != '\0') {
    return endpoint_path_override;
  }

  return provider ? provider->default_endpoint_path : NULL;
}

json_value_t *turbo_model_provider_messages_to_wire_json(const turbo_model_provider_t *provider,
    const json_value_t *messages, char **out_system) {
  return turbo_provider_messages_to_wire_json(turbo_model_provider_format(provider), messages, out_system);
}

json_value_t *turbo_model_provider_response_to_event_json(const turbo_model_provider_t *provider,
    const json_value_t *response) {
  return turbo_provider_response_to_event_json(turbo_model_provider_format(provider), response);
}

json_value_t *turbo_model_provider_response_to_event_json_value(const turbo_model_provider_t *provider,
    const json_value_t *response) {
  return turbo_provider_response_to_event_json_value(turbo_model_provider_format(provider), response);
}

json_value_t *turbo_model_provider_sse_to_event_json(const turbo_model_provider_t *provider,
    const char *data, size_t size) {
  return turbo_provider_sse_to_event_json(turbo_model_provider_format(provider), data, size);
}

json_value_t *turbo_model_provider_sse_to_event_json_value(const turbo_model_provider_t *provider,
    const char *data, size_t size) {
  return turbo_provider_sse_to_event_json_value(turbo_model_provider_format(provider), data, size);
}

int turbo_model_provider_response_emit_json_value(const turbo_model_provider_t *provider,
    const json_value_t *response, turbo_event_sink_json_value_fn sink, void *user_data) {
  return turbo_provider_response_emit_json_value(turbo_model_provider_format(provider), response, sink, user_data);
}

int turbo_model_provider_sse_emit_json_value(const turbo_model_provider_t *provider,
    const char *data, size_t size, turbo_event_sink_json_value_fn sink, void *user_data) {
  return turbo_provider_sse_emit_json_value(turbo_model_provider_format(provider), data, size, sink, user_data);
}
