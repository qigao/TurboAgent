#ifndef TURBO_AGENT_H
#define TURBO_AGENT_H

#include <turbo_agent_api.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default model identifier used when no model is configured.
 *
 * Override at build time with `-DTURBO_AGENT_DEFAULT_MODEL=\"your-model\"`
 * or set the `OPENAI_MODEL` environment variable at runtime.
 */
#ifndef TURBO_AGENT_DEFAULT_MODEL
#define TURBO_AGENT_DEFAULT_MODEL "gpt-5.4"
#endif

typedef struct turbo_agent_s turbo_agent_t;
typedef struct chttp_client chttp_client;
typedef struct turbo_model_provider_s turbo_model_provider_t;
typedef struct turbo_tool_registry_s turbo_tool_registry_t;
typedef struct turbo_action_tool_registry_s turbo_action_tool_registry_t;

typedef enum {
  TURBO_AGENT_API_RESPONSES = 0,
  TURBO_AGENT_API_CHAT_COMPLETIONS = 1
} turbo_agent_api_mode_t;

typedef int (*turbo_agent_transport_fn)(const char *request_json, char **out_response_json,
                                         void *user_data);

typedef struct turbo_agent_config_s {
  const char *api_key;
  const char *model;
  const char *base_url;
  const char *endpoint_path;
  const char *instructions;
  const char *structured_output_name;
  const char *structured_output_schema_json;
  int structured_output_strict;
  size_t structured_output_max_retries;
  turbo_agent_api_mode_t api_mode;
  int stream_response;
  int parallel_tool_calls;
  chttp_client *http_client;
  turbo_tool_registry_t *tool_registry;
  turbo_agent_transport_fn transport_fn;
  void *transport_user_data;
  const turbo_model_provider_t *provider;
} turbo_agent_config_t;

/**
 * @brief Load agent defaults from process environment and optional .env file.
 * @param config Config to populate. Existing tool/http/transport fields are preserved.
 * @param env_path Optional .env path. Pass NULL or empty to load default ".env" in CWD.
 * @param overwrite_env true to overwrite existing process environment while loading .env.
 * @return 0 when .env load succeeded, negative when no .env was loaded or load failed.
 *
 * Supported environment variables:
 * - OPENAI_API_KEY
 * - OPENAI_BASE_URL
 * - OPENAI_MODEL
 * - OPENAI_PROVIDER
 * - OPENAI_API_MODE
 * - OPENAI_ENDPOINT_PATH
 * - OPENAI_STREAM
 * - ANTHROPIC_AUTH_TOKEN
 * - ANTHROPIC_BASE_URL
 */
CXX_C_API int turbo_agent_config_apply_env(turbo_agent_config_t *config,
                                                  const char *env_path,
                                                  int overwrite_env);

/**
 * @brief Create an agent helper.
 * @param config Agent configuration.
 * @return Agent handle or NULL on failure.
 */
CXX_C_API turbo_agent_t *
turbo_agent_create(const turbo_agent_config_t *config);

/**
 * @brief Return the tool registry currently attached to the agent.
 *
 * The returned pointer is borrowed. It may be NULL when the agent has no tools.
 */
CXX_C_API const turbo_tool_registry_t *
turbo_agent_tool_registry(const turbo_agent_t *agent);

/**
 * @brief Return the number of tools currently visible to the agent.
 */
CXX_C_API size_t turbo_agent_tool_count(const turbo_agent_t *agent);

/**
 * @brief Create an agent that bridges a turbo_action_tool registry into legacy tool calls.
 * @param config Agent configuration. `tool_registry` is ignored when action tools are supplied.
 * @param action_tool_registry Action tool registry that must outlive the agent.
 * @return Agent handle or NULL on failure.
 */
CXX_C_API turbo_agent_t *turbo_agent_create_with_action_tools(
    const turbo_agent_config_t *config,
    const turbo_action_tool_registry_t *action_tool_registry);

/**
 * @brief Destroy an agent helper.
 * @param agent Agent handle, may be NULL.
 */
CXX_C_API void turbo_agent_destroy(turbo_agent_t *agent);

#include "turbo_agent_extensions.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_resilience.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_state.h"
#include "turbo_agent_tool_executor.h"

#ifdef __cplusplus
}
#endif

#endif


