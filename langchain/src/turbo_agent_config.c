#include "turbo_agent_config_internal.h"
#include "turbo_agent_transport_internal.h"
#include "turbo_agent_util_internal.h"

#include "CoroNet/turbo_coro_context.h"
#include "http_client.h"
#include "turbo_model_provider.h"
#include "turbo_parser.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

CXX_C_API int turbo_agent_build_responses_turn_request(const turbo_agent_t *agent,
                                                       json_value_t *state,
                                                       char **out_request_json);
CXX_C_API int turbo_agent_build_chat_turn_request(const turbo_agent_t *agent, json_value_t *state,
                                                  char **out_request_json);
CXX_C_API int turbo_agent_build_compatible_chat_turn_request(const turbo_agent_t *agent,
                                                             json_value_t *state,
                                                             char **out_request_json);
CXX_C_API int turbo_agent_build_anthropic_messages_turn_request(const turbo_agent_t *agent,
                                                                json_value_t *state,
                                                                char **out_request_json);

static char *turbo_agent_normalize_base_url(const char *src) {
  const char *scheme;
  const char *path;
  size_t min_len = 0;
  size_t len;
  char *copy;
  const char *suffix = "";
  size_t suffix_len = 0;

  if (!src) {
    return NULL;
  }

  copy = turbo_agent_util_strdup(src);
  if (!copy) {
    return NULL;
  }

  scheme = strstr(src, "://");
  if (scheme) {
    min_len = (size_t)(scheme - src) + 3;
  }

  len = strlen(copy);
  while (len > min_len && len > 0 && copy[len - 1] == '/') {
    copy[len - 1] = '\0';
    --len;
  }
  tstr_set_len(copy, len);

  path = strchr(copy + min_len, '/');
  if (!path) {
    suffix = "/v1";
    suffix_len = 3;
  } else if (path[1] == '\0') {
    suffix = "v1";
    suffix_len = 2;
  }

  if (suffix_len == 0) {
    return copy;
  }

  if (turbo_agent_util_append_bytes(&copy, &len, suffix, suffix_len) != 0) {
    tstr_free(copy);
    return NULL;
  }

  return copy;
}

static char *turbo_agent_normalize_endpoint_path(const char *src) {
  size_t start = 0;
  size_t end;
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }

  len = strlen(src);
  while (start < len && src[start] == '/') {
    ++start;
  }

  end = len;
  while (end > start && src[end - 1] == '/') {
    --end;
  }

  copy = (char *)tstr_new_len(src + start, end - start);
  if (!copy) {
    return NULL;
  }
  return copy;
}

static const turbo_model_provider_t turbo_agent_responses_provider = {
    "openai_responses", "responses", turbo_agent_build_responses_turn_request,
    turbo_agent_configure_http_client_openai};

static const turbo_model_provider_t turbo_agent_chat_completions_provider = {
    "openai_chat_completions", "chat/completions", turbo_agent_build_chat_turn_request,
    turbo_agent_configure_http_client_openai};

static const turbo_model_provider_t turbo_agent_compatible_chat_completions_provider = {
    "openai_compatible_chat_completions", "chat/completions",
    turbo_agent_build_compatible_chat_turn_request, turbo_agent_configure_http_client_openai};

static const turbo_model_provider_t turbo_anthropic_messages_provider = {
    "anthropic_messages", "messages", turbo_agent_build_anthropic_messages_turn_request,
    turbo_agent_configure_http_client_anthropic};

static const turbo_model_provider_t *
turbo_agent_provider_from_api_mode(turbo_agent_api_mode_t api_mode) {
  return api_mode == TURBO_AGENT_API_CHAT_COMPLETIONS
             ? turbo_model_provider_openai_chat_completions()
             : turbo_model_provider_openai_responses();
}

static turbo_agent_api_mode_t
turbo_agent_provider_legacy_api_mode(const turbo_model_provider_t *provider) {
  return turbo_model_provider_is_legacy_chat(provider) ? TURBO_AGENT_API_CHAT_COMPLETIONS
                                                       : TURBO_AGENT_API_RESPONSES;
}

CXX_C_API const turbo_model_provider_t *turbo_model_provider_openai_responses(void) {
  return &turbo_agent_responses_provider;
}

CXX_C_API const turbo_model_provider_t *turbo_model_provider_openai_chat_completions(void) {
  return &turbo_agent_chat_completions_provider;
}

CXX_C_API const turbo_model_provider_t *
turbo_model_provider_openai_compatible_chat_completions(void) {
  return &turbo_agent_compatible_chat_completions_provider;
}

CXX_C_API const turbo_model_provider_t *turbo_model_provider_anthropic_messages(void) {
  return &turbo_anthropic_messages_provider;
}

static int turbo_agent_parse_api_mode_env(const char *value, turbo_agent_api_mode_t *out_mode) {
  if (!out_mode) {
    return -1;
  }

  if (!value || value[0] == '\0') {
    return -1;
  }

  if (strcmp(value, "chat") == 0 || strcmp(value, "chat_completions") == 0 ||
      strcmp(value, "chat-completions") == 0) {
    *out_mode = TURBO_AGENT_API_CHAT_COMPLETIONS;
    return 0;
  }

  if (strcmp(value, "responses") == 0) {
    *out_mode = TURBO_AGENT_API_RESPONSES;
    return 0;
  }

  return -1;
}

static int turbo_agent_parse_bool_env(const char *value, int default_value) {
  if (!value || value[0] == '\0') {
    return default_value;
  }

  if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
      strcmp(value, "on") == 0) {
    return 1;
  }

  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
      strcmp(value, "off") == 0) {
    return 0;
  }

  return default_value;
}

#ifdef _WIN32
static void turbo_agent_config_sync_process_env_key(const char *key) {
  DWORD needed;
  char *buffer;

  if (!key || getenv(key)) {
    return;
  }

  needed = GetEnvironmentVariableA(key, NULL, 0);
  if (needed == 0) {
    return;
  }

  buffer = (char *)malloc((size_t)needed);
  if (!buffer) {
    return;
  }

  if (GetEnvironmentVariableA(key, buffer, needed) > 0) {
    _putenv_s(key, buffer);
  }
  free(buffer);
}

static void turbo_agent_config_sync_process_env(void) {
  static const char *const keys[] = {
      "OPENAI_API_KEY",       "OPENAI_BASE_URL",       "OPENAI_MODEL",
      "OPENAI_PROVIDER",      "OPENAI_API_MODE",       "OPENAI_ENDPOINT_PATH",
      "OPENAI_STREAM",        "ANTHROPIC_AUTH_TOKEN",  "ANTHROPIC_BASE_URL"};
  size_t i;

  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    turbo_agent_config_sync_process_env_key(keys[i]);
  }
}
#else
static void turbo_agent_config_sync_process_env(void) {}
#endif

static const turbo_model_provider_t *
turbo_agent_config_pick_env_provider(turbo_agent_config_t *config, const char *provider_env) {
  const turbo_model_provider_t *provider;

  provider = config ? config->provider : NULL;
  if ((!provider || provider->name == NULL) && provider_env && provider_env[0] != '\0') {
    provider = turbo_model_provider_by_name(provider_env);
    if (provider && config) {
      config->provider = provider;
    }
  }

  return provider;
}

static void
turbo_agent_config_apply_auth_defaults(turbo_agent_config_t *config,
                                       const turbo_model_provider_t *provider, const char *api_key,
                                       const char *anthropic_api_key, const char *base_url,
                                       const char *anthropic_base_url, const char *model) {
  if (!config) {
    return;
  }

  if (!config->api_key || config->api_key[0] == '\0') {
    config->api_key = turbo_model_provider_select_api_key(provider, api_key, anthropic_api_key);
  }

  if (!config->model || config->model[0] == '\0') {
    config->model = (model && model[0] != '\0') ? model : TURBO_AGENT_DEFAULT_MODEL;
  }

  if (!config->base_url || config->base_url[0] == '\0') {
    config->base_url = turbo_model_provider_select_base_url(provider, base_url, anthropic_base_url,
                                                            "https://api.openai.com/v1");
  }
}

CXX_C_API const turbo_model_provider_t *
turbo_agent_resolve_provider(const turbo_model_provider_t *provider,
                             turbo_agent_api_mode_t api_mode,
                             turbo_agent_api_mode_t *out_api_mode) {
  const turbo_model_provider_t *resolved;

  resolved = provider ? provider : turbo_agent_provider_from_api_mode(api_mode);
  if (out_api_mode) {
    *out_api_mode = turbo_agent_provider_legacy_api_mode(resolved);
  }
  return resolved;
}

static const turbo_model_provider_t *
turbo_agent_config_finalize_provider(turbo_agent_config_t *config, const char *api_mode_env) {
  turbo_agent_api_mode_t api_mode;
  const turbo_model_provider_t *provider;

  if (!config) {
    return NULL;
  }

  api_mode = config->api_mode;
  if (!config->provider && turbo_agent_parse_api_mode_env(api_mode_env, &api_mode) == 0) {
    config->api_mode = api_mode;
  }

  provider = turbo_agent_resolve_provider(config->provider, config->api_mode, &config->api_mode);
  config->provider = provider;
  return provider;
}

CXX_C_API int turbo_agent_apply_core_config(turbo_agent_t *agent,
                                            const turbo_agent_config_t *config,
                                            const turbo_model_provider_t *provider) {
  if (!agent || !config || !provider) {
    return -1;
  }

  agent->provider = provider;
  agent->api_mode = turbo_agent_provider_legacy_api_mode(provider);
  agent->model = turbo_agent_util_strdup(config->model);
  agent->base_url = turbo_agent_normalize_base_url(config->base_url ? config->base_url
                                                                    : "https://api.openai.com/v1");
  agent->endpoint_path = turbo_agent_normalize_endpoint_path(
      turbo_model_provider_select_endpoint_path(provider, config->endpoint_path));
  agent->instructions = config->instructions ? turbo_agent_util_strdup(config->instructions) : NULL;
  agent->structured_output_name = config->structured_output_name
                                      ? turbo_agent_util_strdup(config->structured_output_name)
                                      : NULL;
  agent->structured_output_schema_json =
      config->structured_output_schema_json
          ? turbo_agent_util_strdup(config->structured_output_schema_json)
          : NULL;
  agent->structured_output_strict = config->structured_output_strict ? 1 : 0;
  agent->structured_output_max_retries = config->structured_output_max_retries;
  agent->api_key = config->api_key ? turbo_agent_util_strdup(config->api_key) : NULL;

  if (!provider->build_request || !agent->model || !agent->base_url || !agent->endpoint_path ||
      (config->instructions && !agent->instructions) ||
      (config->structured_output_name && !agent->structured_output_name) ||
      (config->structured_output_schema_json && !agent->structured_output_schema_json) ||
      (config->api_key && !agent->api_key)) {
    return -1;
  }

  agent->parallel_tool_calls = config->parallel_tool_calls ? 1 : 0;
  agent->stream_response = config->stream_response ? 1 : 0;
  agent->tool_registry = config->tool_registry;
  agent->transport_fn = config->transport_fn;
  agent->transport_user_data = config->transport_user_data;
  return 0;
}

typedef struct turbo_agent_http_client_create_task_s {
  turbo_agent_t *agent;
  int done;
} turbo_agent_http_client_create_task_t;

static void turbo_agent_http_client_create_coro(coro_t *co, void *arg) {
  turbo_agent_http_client_create_task_t *task = (turbo_agent_http_client_create_task_t *)arg;
  coro_context_t *ctx;

  (void)co;
  if (task && task->agent) {
    task->agent->http_client = http_client_create(task->agent->base_url);
    task->done = 1;
  }

  ctx = coro_context_current();
  if (ctx) {
    coro_context_stop(ctx);
  }
}

static int turbo_agent_create_owned_http_client(turbo_agent_t *agent) {
  turbo_agent_http_client_create_task_t task = {0};
  coro_context_t *ctx;

  if (!agent) {
    return -1;
  }

  ctx = coro_context_create(NULL);
  if (!ctx) {
    return -1;
  }

  task.agent = agent;
  if (coro_context_spawn(ctx, turbo_agent_http_client_create_coro, &task) != 0) {
    coro_context_destroy(ctx);
    return -1;
  }

  coro_context_run(ctx, TURBO_RUN_DEFAULT);
  if (!task.done || !agent->http_client) {
    coro_context_destroy(ctx);
    return -1;
  }

  agent->http_context = ctx;
  agent->owns_http_context = 1;
  agent->owns_http_client = 1;
  return 0;
}

CXX_C_API int turbo_agent_attach_http_client(turbo_agent_t *agent,
                                             const turbo_agent_config_t *config,
                                             const turbo_model_provider_t *provider) {
  if (!agent || !config || !provider) {
    return -1;
  }

  if (config->http_client) {
    agent->http_client = config->http_client;
    agent->owns_http_client = 0;
  } else if (!config->transport_fn) {
    if (turbo_agent_create_owned_http_client(agent) != 0) {
      return -1;
    }
  }

  if (agent->http_client && provider->configure_http_client) {
    if (provider->configure_http_client(agent, agent->http_client) != 0) {
      return -1;
    }
  }

  return 0;
}

CXX_C_API int turbo_agent_config_apply_env(turbo_agent_config_t *config, const char *env_path,
                                           int overwrite_env) {
  const char *api_key;
  const char *base_url;
  const char *anthropic_api_key;
  const char *anthropic_base_url;
  const char *model;
  const char *provider_env;
  const char *api_mode_env;
  const char *endpoint_path;
  const char *stream_env;
  int env_rc;
  const turbo_model_provider_t *provider;

  if (!config) {
    return -1;
  }

  if (env_path && env_path[0] != '\0') {
    env_rc = turbo_dotenv_load(env_path, overwrite_env ? true : false);
  } else {
    env_rc = turbo_dotenv_load_default(overwrite_env ? true : false);
  }
  turbo_agent_config_sync_process_env();

  api_key = getenv("OPENAI_API_KEY");
  base_url = getenv("OPENAI_BASE_URL");
  anthropic_api_key = getenv("ANTHROPIC_AUTH_TOKEN");
  anthropic_base_url = getenv("ANTHROPIC_BASE_URL");
  model = getenv("OPENAI_MODEL");
  provider_env = getenv("OPENAI_PROVIDER");
  api_mode_env = getenv("OPENAI_API_MODE");
  endpoint_path = getenv("OPENAI_ENDPOINT_PATH");
  stream_env = getenv("OPENAI_STREAM");

  provider = turbo_agent_config_pick_env_provider(config, provider_env);
  turbo_agent_config_apply_auth_defaults(config, provider, api_key, anthropic_api_key, base_url,
                                         anthropic_base_url, model);
  provider = turbo_agent_config_finalize_provider(config, api_mode_env);

  if (!config->endpoint_path || config->endpoint_path[0] == '\0') {
    config->endpoint_path = turbo_model_provider_select_endpoint_path(provider, endpoint_path);
  }

  config->stream_response = turbo_agent_parse_bool_env(stream_env, config->stream_response);
  return env_rc;
}
