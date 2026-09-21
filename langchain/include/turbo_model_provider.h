#ifndef TURBO_MODEL_PROVIDER_H
#define TURBO_MODEL_PROVIDER_H

#include <turbo_agent_api.h>
#include <http_client/http.h>

#include <json_parser.h>
#include "turbo_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;
typedef struct turbo_model_provider_s turbo_model_provider_t;

typedef int (*turbo_model_provider_build_request_fn)(const turbo_agent_t *agent,
                                                     json_value_t *state, char **out_request_json);
typedef int (*turbo_model_provider_build_http_headers_fn)(const turbo_agent_t *agent,
                                                           chttp_header *headers,
                                                           size_t capacity,
                                                           size_t *out_count);

struct turbo_model_provider_s {
  const char *name;
  const char *default_endpoint_path;
  turbo_model_provider_build_request_fn build_request;
  turbo_model_provider_build_http_headers_fn build_http_headers;
};

/**
 * @brief Return the builtin OpenAI Responses provider adapter.
 * @return Shared provider singleton.
 */
CXX_C_API const turbo_model_provider_t *turbo_model_provider_openai_responses(void);

/**
 * @brief Return the builtin OpenAI Chat Completions provider adapter.
 * @return Shared provider singleton.
 */
CXX_C_API const turbo_model_provider_t *turbo_model_provider_openai_chat_completions(void);

/**
 * @brief Return the builtin OpenAI-compatible Chat Completions provider adapter.
 * @return Shared provider singleton.
 */
CXX_C_API const turbo_model_provider_t *
turbo_model_provider_openai_compatible_chat_completions(void);

/**
 * @brief Return the builtin Anthropic Messages provider adapter.
 * @return Shared provider singleton.
 *
 * This provider currently proves the non-OpenAI wire shape through custom
 * transports. The default built-in HTTP path still uses OpenAI-style bearer
 * authentication and should not be used against Anthropic directly yet.
 */
CXX_C_API const turbo_model_provider_t *turbo_model_provider_anthropic_messages(void);

/**
 * @brief Look up a builtin provider by stable name.
 * @param name Provider name such as `openai`, `openai_responses`,
 *        `openai_chat_completions`, `openai_compatible_chat_completions`, or
 *        `anthropic_messages`.
 * @return Provider singleton or NULL when not found.
 */
CXX_C_API const turbo_model_provider_t *turbo_model_provider_by_name(const char *name);

/**
 * @brief Return whether the provider uses legacy Chat Completions mode semantics.
 * @param provider Provider singleton.
 * @return Non-zero when the provider maps to chat-completions compatibility mode.
 */
CXX_C_API int turbo_model_provider_is_legacy_chat(const turbo_model_provider_t *provider);

/**
 * @brief Return whether the provider is the builtin Anthropic Messages adapter.
 * @param provider Provider singleton.
 * @return Non-zero when this is the Anthropic provider.
 */
CXX_C_API int turbo_model_provider_is_anthropic_messages(const turbo_model_provider_t *provider);

/**
 * @brief Choose the most appropriate API key source for a provider.
 * @param provider Provider singleton.
 * @param openai_api_key Generic OpenAI-compatible API key.
 * @param anthropic_auth_token Anthropic-specific auth token.
 * @return Borrowed pointer to the preferred key, or NULL.
 */
CXX_C_API const char *turbo_model_provider_select_api_key(
    const turbo_model_provider_t *provider, const char *openai_api_key,
    const char *anthropic_auth_token);

/**
 * @brief Choose the most appropriate base URL source for a provider.
 * @param provider Provider singleton.
 * @param openai_base_url Generic OpenAI-compatible base URL.
 * @param anthropic_base_url Anthropic-specific base URL.
 * @param default_openai_base_url Fallback OpenAI-style base URL when none is configured.
 * @return Borrowed pointer to the preferred base URL, or NULL.
 */
CXX_C_API const char *turbo_model_provider_select_base_url(
    const turbo_model_provider_t *provider, const char *openai_base_url,
    const char *anthropic_base_url, const char *default_openai_base_url);

/**
 * @brief Choose an endpoint path override or fall back to the provider default.
 * @param provider Provider singleton.
 * @param endpoint_path_override Optional explicit endpoint path.
 * @return Borrowed pointer to the preferred endpoint path, or NULL.
 */
CXX_C_API const char *turbo_model_provider_select_endpoint_path(
    const turbo_model_provider_t *provider, const char *endpoint_path_override);

/**
 * @brief Convert canonical TurboParser JSON-native messages into provider-specific wire JSON.
 * @param provider Provider singleton.
 * @param messages Canonical message array.
 * @param out_system Optional provider-side system text owned by caller when needed.
 * @return JSON array owned by caller, or NULL on validation/allocation failure.
 */
CXX_C_API json_value_t *turbo_model_provider_messages_to_wire_json(
    const turbo_model_provider_t *provider, const json_value_t *messages,
    char **out_system);

/**
 * @brief Convert one provider wire response into a canonical model event object.
 * @param provider Provider singleton.
 * @param response Provider response JSON.
 * @return Canonical `{"kind":"model",...}` event owned by caller, or NULL.
 */
CXX_C_API json_value_t *turbo_model_provider_response_to_event_json(
    const turbo_model_provider_t *provider, const json_value_t *response);

/**
 * @brief Convert provider SSE payload into one canonical model event object.
 * @param provider Provider singleton.
 * @param sse_data Raw SSE payload.
 * @param sse_len Raw SSE payload length.
 * @return Canonical `{"kind":"model",...}` event owned by caller, or NULL.
 */
CXX_C_API json_value_t *turbo_model_provider_sse_to_event_json(
    const turbo_model_provider_t *provider, const char *sse_data, size_t sse_len);

/**
 * @brief Convert one provider wire response into a canonical TurboParser JSON-native model event.
 */
CXX_C_API json_value_t *turbo_model_provider_response_to_event_json_value(
    const turbo_model_provider_t *provider, const json_value_t *response);

/**
 * @brief Emit canonical TurboParser JSON-native model events derived from one provider response.
 */
CXX_C_API int turbo_model_provider_response_emit_json_value(const turbo_model_provider_t *provider,
                                                      const json_value_t *response,
                                                      turbo_event_sink_json_value_fn event_sink,
                                                      void *event_sink_user_data);

/**
 * @brief Convert provider SSE payload into one canonical TurboParser JSON-native model event.
 */
CXX_C_API json_value_t *turbo_model_provider_sse_to_event_json_value(
    const turbo_model_provider_t *provider, const char *sse_data, size_t sse_len);

/**
 * @brief Emit canonical TurboParser JSON-native model events derived from one SSE payload.
 */
CXX_C_API int turbo_model_provider_sse_emit_json_value(const turbo_model_provider_t *provider,
                                                 const char *sse_data, size_t sse_len,
                                                 turbo_event_sink_json_value_fn event_sink,
                                                 void *event_sink_user_data);

#ifdef __cplusplus
}
#endif

#endif
