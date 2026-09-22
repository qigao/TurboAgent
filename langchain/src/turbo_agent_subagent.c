#include "turbo_agent_subagent.h"

#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

typedef struct turbo_agent_subagent_tool_s {
  turbo_agent_subagent_mode_t mode;
  turbo_agent_subagent_result_kind_t result_kind;
  turbo_agent_session_config_t session_config;
  turbo_agent_app_t *shared_app;
} turbo_agent_subagent_tool_t;

static const char *turbo_agent_subagent_default_parameters_json = "{\"type\":\"object\"}";

static char *turbo_agent_subagent_strdup(const char *src) {
  size_t len;
  char *copy;

  if (!src) {
    return NULL;
  }
  len = strlen(src) + 1;
  copy = (char *)malloc(len);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, src, len);
  return copy;
}

static void turbo_agent_subagent_session_config_clear(turbo_agent_session_config_t *config) {
  if (!config) {
    return;
  }
  free((char *)config->agent_config.api_key);
  free((char *)config->agent_config.model);
  free((char *)config->agent_config.base_url);
  free((char *)config->agent_config.endpoint_path);
  free((char *)config->agent_config.instructions);
  free((char *)config->agent_config.structured_output_name);
  free((char *)config->agent_config.structured_output_schema_json);
  free((char *)config->thread_id);
  free((char *)config->env_path);
  free((char *)config->memory_namespace);
  free((char *)config->parent_agent_run_id);
  free((char *)config->parent_tool_call_id);
  free((char *)config->parent_tool_name);
  free((char *)config->parent_graph_run_id);
  free((char *)config->call_frame_id);
  free((char *)config->knowledge_query);
  free((char *)config->knowledge_kind);
  free((char *)config->knowledge_uri_prefix);
  free((char *)config->retriever_query);
  free((char *)config->retriever_kind);
  free((char *)config->retriever_uri_prefix);
  free((char *)config->retriever_scope);
  memset(config, 0, sizeof(*config));
}

static int turbo_agent_subagent_session_config_copy_ephemeral(
    turbo_agent_session_config_t *dst, const turbo_agent_session_config_t *src) {
  if (!dst || !src) {
    return -1;
  }

  memset(dst, 0, sizeof(*dst));
  dst->agent_config = src->agent_config;
  dst->thread_id = turbo_agent_subagent_strdup(src->thread_id);
  dst->env_path = turbo_agent_subagent_strdup(src->env_path);
  dst->memory_namespace = turbo_agent_subagent_strdup(src->memory_namespace);
  dst->parent_agent_run_id = turbo_agent_subagent_strdup(src->parent_agent_run_id);
  dst->parent_tool_call_id = turbo_agent_subagent_strdup(src->parent_tool_call_id);
  dst->parent_tool_name = turbo_agent_subagent_strdup(src->parent_tool_name);
  dst->parent_graph_run_id = turbo_agent_subagent_strdup(src->parent_graph_run_id);
  dst->call_frame_id = turbo_agent_subagent_strdup(src->call_frame_id);
  dst->knowledge_store = src->knowledge_store;
  dst->knowledge_query = turbo_agent_subagent_strdup(src->knowledge_query);
  dst->knowledge_kind = turbo_agent_subagent_strdup(src->knowledge_kind);
  dst->knowledge_uri_prefix = turbo_agent_subagent_strdup(src->knowledge_uri_prefix);
  dst->knowledge_limit = src->knowledge_limit;
  dst->retriever = src->retriever;
  dst->retriever_query = turbo_agent_subagent_strdup(src->retriever_query);
  dst->retriever_kind = turbo_agent_subagent_strdup(src->retriever_kind);
  dst->retriever_uri_prefix = turbo_agent_subagent_strdup(src->retriever_uri_prefix);
  dst->retriever_limit = src->retriever_limit;
  dst->retriever_scope = turbo_agent_subagent_strdup(src->retriever_scope);
  dst->workflow_kind = src->workflow_kind;
  dst->load_env = src->load_env;
  dst->overwrite_env = src->overwrite_env;

  dst->agent_config.api_key = turbo_agent_subagent_strdup(src->agent_config.api_key);
  dst->agent_config.model = turbo_agent_subagent_strdup(src->agent_config.model);
  dst->agent_config.base_url = turbo_agent_subagent_strdup(src->agent_config.base_url);
  dst->agent_config.endpoint_path = turbo_agent_subagent_strdup(src->agent_config.endpoint_path);
  dst->agent_config.instructions = turbo_agent_subagent_strdup(src->agent_config.instructions);
  dst->agent_config.structured_output_name =
      turbo_agent_subagent_strdup(src->agent_config.structured_output_name);
  dst->agent_config.structured_output_schema_json =
      turbo_agent_subagent_strdup(src->agent_config.structured_output_schema_json);

  if ((src->thread_id && !dst->thread_id) || (src->env_path && !dst->env_path) ||
      (src->memory_namespace && !dst->memory_namespace) ||
      (src->parent_agent_run_id && !dst->parent_agent_run_id) ||
      (src->parent_tool_call_id && !dst->parent_tool_call_id) ||
      (src->parent_tool_name && !dst->parent_tool_name) ||
      (src->parent_graph_run_id && !dst->parent_graph_run_id) ||
      (src->call_frame_id && !dst->call_frame_id) ||
      (src->knowledge_query && !dst->knowledge_query) ||
      (src->knowledge_kind && !dst->knowledge_kind) ||
      (src->knowledge_uri_prefix && !dst->knowledge_uri_prefix) ||
      (src->retriever_query && !dst->retriever_query) ||
      (src->retriever_kind && !dst->retriever_kind) ||
      (src->retriever_uri_prefix && !dst->retriever_uri_prefix) ||
      (src->retriever_scope && !dst->retriever_scope) ||
      (src->agent_config.api_key && !dst->agent_config.api_key) ||
      (src->agent_config.model && !dst->agent_config.model) ||
      (src->agent_config.base_url && !dst->agent_config.base_url) ||
      (src->agent_config.endpoint_path && !dst->agent_config.endpoint_path) ||
      (src->agent_config.instructions && !dst->agent_config.instructions) ||
      (src->agent_config.structured_output_name &&
       !dst->agent_config.structured_output_name) ||
      (src->agent_config.structured_output_schema_json &&
       !dst->agent_config.structured_output_schema_json)) {
    turbo_agent_subagent_session_config_clear(dst);
    return -1;
  }

  memset(&dst->runtime_store, 0, sizeof(dst->runtime_store));
  memset(&dst->memory_store, 0, sizeof(dst->memory_store));
  return 0;
}

static void turbo_agent_subagent_tool_destroy(void *user_data) {
  turbo_agent_subagent_tool_t *tool = (turbo_agent_subagent_tool_t *)user_data;

  if (!tool) {
    return;
  }
  turbo_agent_app_destroy(tool->shared_app);
  turbo_agent_subagent_session_config_clear(&tool->session_config);
  free(tool);
}

static turbo_agent_subagent_tool_t *turbo_agent_subagent_tool_create(
    const turbo_agent_subagent_tool_config_t *config) {
  turbo_agent_subagent_tool_t *tool;
  turbo_agent_app_config_t app_config = {0};

  if (!config || !config->session_config) {
    return NULL;
  }

  tool = (turbo_agent_subagent_tool_t *)calloc(1, sizeof(*tool));
  if (!tool) {
    return NULL;
  }

  tool->mode = config->mode;
  tool->result_kind = config->result_kind;

  if (tool->mode == TURBO_AGENT_SUBAGENT_SHARED_APP) {
    app_config.session_config = config->session_config;
    tool->shared_app = turbo_agent_app_create(&app_config);
    if (!tool->shared_app) {
      turbo_agent_subagent_tool_destroy(tool);
      return NULL;
    }
  } else if (turbo_agent_subagent_session_config_copy_ephemeral(&tool->session_config,
                                                                config->session_config) != 0) {
    turbo_agent_subagent_tool_destroy(tool);
    return NULL;
  }

  return tool;
}

static turbo_agent_app_t *turbo_agent_subagent_tool_acquire_app(
    turbo_agent_subagent_tool_t *tool, int *out_owned) {
  turbo_agent_app_config_t app_config = {0};
  turbo_agent_app_t *app;

  if (!tool || !out_owned) {
    return NULL;
  }

  *out_owned = 0;
  if (tool->mode == TURBO_AGENT_SUBAGENT_SHARED_APP) {
    return tool->shared_app;
  }

  app_config.session_config = &tool->session_config;
  app = turbo_agent_app_create(&app_config);
  if (!app) {
    return NULL;
  }
  *out_owned = 1;
  return app;
}

static void turbo_agent_subagent_tool_release_app(turbo_agent_app_t *app, int owned) {
  if (owned) {
    turbo_agent_app_destroy(app);
  }
}

static const json_value_t *turbo_agent_subagent_resolve_messages(
    const json_value_t *arguments) {
  const json_value_t *messages;

  if (!arguments) {
    return NULL;
  }
  if (turbo_json_type(arguments) == TURBO_JSON_ARRAY) {
    return arguments;
  }
  if (turbo_json_type(arguments) != TURBO_JSON_OBJECT) {
    return NULL;
  }
  messages = turbo_json_object_get(arguments, "messages");
  if (!messages ||
      turbo_json_type(messages) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  return messages;
}

static char *turbo_agent_subagent_serialize_json_value(
    const json_value_t *value) {
  json_value_t *json_value;
  char *serialized;

  if (!value) {
    return NULL;
  }
  json_value = turbo_json_clone(value);
  if (!json_value) {
    return NULL;
  }
  serialized = turbo_json_serialize(json_value, NULL);
  turbo_free_json(&json_value);
  return serialized;
}

static char *turbo_agent_subagent_resolve_input_text(
    const json_value_t *arguments) {
  const json_value_t *input_value;
  const char *text;

  if (!arguments) {
    return NULL;
  }
  if (turbo_json_type(arguments) == TURBO_JSON_STRING) {
    text = turbo_runtime_json_value_as_string(arguments);
    return text ? turbo_agent_subagent_strdup(text) : NULL;
  }
  if (turbo_json_type(arguments) == TURBO_JSON_OBJECT) {
    input_value = turbo_json_object_get(arguments, "input");
    if (input_value) {
      if (turbo_json_type(input_value) == TURBO_JSON_STRING) {
        text = turbo_runtime_json_value_as_string(input_value);
        return text ? turbo_agent_subagent_strdup(text) : NULL;
      }
      return turbo_agent_subagent_serialize_json_value(input_value);
    }
  }
  return turbo_agent_subagent_serialize_json_value(arguments);
}

static int turbo_agent_subagent_result_set_base(json_value_t *result, const json_value_t *summary) {
  const json_value_t *checkpoint_id;
  json_value_t *summary_clone;
  const char *status;
  const char *thread_id;
  const char *run_id;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;
  const char *active_agent;
  const char *handoff_target_agent;
  const char *handoff_reason;

  if (!result || !summary || turbo_json_type(summary) != TURBO_JSON_OBJECT) {
    return -1;
  }

  status = turbo_json_get_string(summary, "status");
  thread_id = turbo_json_get_string(summary, "thread_id");
  run_id = turbo_json_get_string(summary, "run_id");
  parent_agent_run_id = turbo_json_get_string(summary, "parent_agent_run_id");
  parent_tool_call_id = turbo_json_get_string(summary, "parent_tool_call_id");
  parent_tool_name = turbo_json_get_string(summary, "parent_tool_name");
  parent_graph_run_id = turbo_json_get_string(summary, "parent_graph_run_id");
  call_frame_id = turbo_json_get_string(summary, "call_frame_id");
  active_agent = turbo_json_get_string(summary, "active_agent");
  handoff_target_agent = turbo_json_get_string(summary, "handoff_target_agent");
  handoff_reason = turbo_json_get_string(summary, "handoff_reason");
  checkpoint_id = turbo_json_object_get(summary, "checkpoint_id");
  if (!status || !thread_id || !run_id) {
    return -1;
  }

  turbo_json_object_set_bool(result, "ok", true);
  turbo_json_object_set_string(result, "status", status);
  turbo_json_object_set_string(result, "child_status", status);
  turbo_json_object_set_string(result, "thread_id", thread_id);
  turbo_json_object_set_string(result, "child_thread_id", thread_id);
  turbo_json_object_set_string(result, "run_id", run_id);
  turbo_json_object_set_string(result, "child_run_id", run_id);
  if (parent_agent_run_id) {
    turbo_json_object_set_string(result, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_add(result, "parent_agent_run_id", turbo_json_create_null());
  }
  if (parent_tool_call_id) {
    turbo_json_object_set_string(result, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_add(result, "parent_tool_call_id", turbo_json_create_null());
  }
  if (parent_tool_name) {
    turbo_json_object_set_string(result, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_add(result, "parent_tool_name", turbo_json_create_null());
  }
  if (parent_graph_run_id) {
    turbo_json_object_set_string(result, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_add(result, "parent_graph_run_id", turbo_json_create_null());
  }
  if (call_frame_id) {
    turbo_json_object_set_string(result, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_add(result, "call_frame_id", turbo_json_create_null());
  }
  if (active_agent) {
    turbo_json_object_set_string(result, "active_agent", active_agent);
  } else {
    turbo_json_object_add(result, "active_agent", turbo_json_create_null());
  }
  if (handoff_target_agent) {
    turbo_json_object_set_string(result, "handoff_target_agent", handoff_target_agent);
  } else {
    turbo_json_object_add(result, "handoff_target_agent", turbo_json_create_null());
  }
  if (handoff_reason) {
    turbo_json_object_set_string(result, "handoff_reason", handoff_reason);
  } else {
    turbo_json_object_add(result, "handoff_reason", turbo_json_create_null());
  }
  if (checkpoint_id && turbo_json_type(checkpoint_id) == TURBO_JSON_STRING) {
    turbo_json_object_set_string(result, "checkpoint_id",
                                 turbo_json_get_string(summary, "checkpoint_id"));
    turbo_json_object_set_string(result, "child_checkpoint_id",
                                 turbo_json_get_string(summary, "checkpoint_id"));
  } else {
    turbo_json_object_add(result, "checkpoint_id", turbo_json_create_null());
    turbo_json_object_add(result, "child_checkpoint_id", turbo_json_create_null());
  }
  summary_clone = turbo_json_clone(summary);
  if (!summary_clone) {
    return -1;
  }
  turbo_json_object_add(result, "summary", summary_clone);
  return 0;
}

static json_value_t *turbo_agent_subagent_build_text_result(
    const json_value_t *summary, const char *text) {
  json_value_t *result;
  json_value_t *json_value_result;

  result = turbo_json_create_object();
  if (!result) {
    return NULL;
  }
  if (turbo_agent_subagent_result_set_base(result, summary) != 0) {
    turbo_free_json(&result);
    return NULL;
  }
  turbo_json_object_set_string(result, "output_text", text ? text : "");

  json_value_result = turbo_json_clone(result);
  turbo_free_json(&result);
  return json_value_result;
}

static json_value_t *turbo_agent_subagent_build_json_result(
    const json_value_t *summary, const json_value_t *output_json) {
  json_value_t *result;
  json_value_t *output_clone;
  json_value_t *json_value_result;
  char *serialized = NULL;

  if (!output_json) {
    return NULL;
  }

  result = turbo_json_create_object();
  if (!result) {
    return NULL;
  }
  if (turbo_agent_subagent_result_set_base(result, summary) != 0) {
    turbo_free_json(&result);
    return NULL;
  }
  output_clone = turbo_json_clone(output_json);
  if (!output_clone) {
    turbo_free_json(&result);
    return NULL;
  }
  serialized = turbo_json_serialize(output_json, NULL);
  if (!serialized) {
    turbo_free_json(&output_clone);
    turbo_free_json(&result);
    return NULL;
  }
  turbo_json_object_set_string(result, "output_text", serialized);
  turbo_json_object_add(result, "output_json", output_clone);
  turbo_json_serialize_free(serialized);

  json_value_result = turbo_json_clone(result);
  turbo_free_json(&result);
  return json_value_result;
}

static int turbo_agent_subagent_invoke_with_app(
    turbo_agent_subagent_tool_t *tool, turbo_agent_app_t *app,
    const json_value_t *arguments,
    json_value_t **out_result) {
  const json_value_t *messages;
  char *text = NULL;
  char *output_text = NULL;
  json_value_t *summary = NULL;
  json_value_t *output_json = NULL;
  json_value_t *result = NULL;
  int rc = -1;

  if (!tool || !app || !out_result) {
    return -1;
  }

  *out_result = NULL;
  messages = turbo_agent_subagent_resolve_messages(arguments);
  if (messages) {
    if (tool->result_kind == TURBO_AGENT_SUBAGENT_RESULT_JSON) {
      rc = turbo_agent_app_invoke_messages_json(app, messages, NULL, &output_json, &summary);
      if (rc == 0) {
        result = turbo_agent_subagent_build_json_result(summary, output_json);
      }
    } else {
      rc = turbo_agent_app_invoke_messages_text(app, messages, NULL, &output_text, &summary);
      if (rc == 0) {
        result = turbo_agent_subagent_build_text_result(summary, output_text);
      }
    }
  } else {
    text = turbo_agent_subagent_resolve_input_text(arguments);
    if (tool->result_kind == TURBO_AGENT_SUBAGENT_RESULT_JSON) {
      rc = turbo_agent_app_invoke_json(app, text, NULL, &output_json, &summary);
      if (rc == 0) {
        result = turbo_agent_subagent_build_json_result(summary, output_json);
      }
    } else {
      rc = turbo_agent_app_invoke_text(app, text, NULL, &output_text, &summary);
      if (rc == 0) {
        result = turbo_agent_subagent_build_text_result(summary, output_text);
      }
    }
  }

  free(text);
  free(output_text);
  turbo_free_json(&summary);
  turbo_free_json(&output_json);

  if (rc != 0 || !result) {
    turbo_runtime_json_destroy(result);
    return -1;
  }
  *out_result = result;
  return 0;
}

static int turbo_agent_subagent_json_value_handler(const json_value_t *arguments,
                                             json_value_t **out_result,
                                             void *user_data) {
  turbo_agent_subagent_tool_t *tool = (turbo_agent_subagent_tool_t *)user_data;
  turbo_agent_app_t *app;
  int owned = 0;
  int rc;

  if (!tool || !out_result) {
    return -1;
  }

  app = turbo_agent_subagent_tool_acquire_app(tool, &owned);
  if (!app) {
    return -1;
  }
  rc = turbo_agent_subagent_invoke_with_app(tool, app, arguments, out_result);
  turbo_agent_subagent_tool_release_app(app, owned);
  return rc;
}

static int turbo_agent_subagent_handler(const char *arguments_json, char **out_output,
                                        void *user_data) {
  json_value_t *arguments = NULL;
  json_value_t *json_value_arguments = NULL;
  json_value_t *json_value_result = NULL;
  json_value_t *result_json = NULL;
  char *serialized = NULL;
  int rc = -1;

  if (!out_output) {
    return -1;
  }
  *out_output = NULL;

  if (!arguments_json || arguments_json[0] == '\0') {
    arguments = turbo_json_create_object();
  } else if (turbo_parse_json((const uint8_t *)arguments_json, strlen(arguments_json), &arguments) !=
             0) {
    return -1;
  }
  if (!arguments) {
    return -1;
  }

  json_value_arguments = turbo_json_clone(arguments);
  turbo_free_json(&arguments);
  if (!json_value_arguments) {
    return -1;
  }

  rc = turbo_agent_subagent_json_value_handler(json_value_arguments, &json_value_result, user_data);
  turbo_runtime_json_destroy(json_value_arguments);
  if (rc != 0 || !json_value_result) {
    turbo_runtime_json_destroy(json_value_result);
    return -1;
  }

  result_json = turbo_json_clone(json_value_result);
  turbo_runtime_json_destroy(json_value_result);
  if (!result_json) {
    return -1;
  }

  serialized = turbo_json_serialize(result_json, NULL);
  turbo_free_json(&result_json);
  if (!serialized) {
    return -1;
  }

  *out_output = serialized;
  return 0;
}

static turbo_tool_status_t turbo_agent_subagent_prepare_definition(
    const turbo_agent_subagent_tool_config_t *config, turbo_tool_definition_t *out_definition) {
  turbo_agent_subagent_tool_t *tool;

  if (!config || !config->name || !config->description || !config->session_config ||
      !out_definition) {
    return TURBO_TOOL_INVALID_ARGUMENT;
  }

  tool = turbo_agent_subagent_tool_create(config);
  if (!tool) {
    return TURBO_TOOL_OUT_OF_MEMORY;
  }

  memset(out_definition, 0, sizeof(*out_definition));
  out_definition->name = config->name;
  out_definition->description = config->description;
  out_definition->parameters_json =
      config->parameters_json ? config->parameters_json : turbo_agent_subagent_default_parameters_json;
  out_definition->parameters_schema = config->parameters_schema;
  out_definition->strict = config->strict;
  out_definition->handler = turbo_agent_subagent_handler;
  out_definition->json_value_handler = turbo_agent_subagent_json_value_handler;
  out_definition->user_data = tool;
  out_definition->user_data_free = turbo_agent_subagent_tool_destroy;
  return TURBO_TOOL_OK;
}

CXX_C_API turbo_tool_status_t turbo_agent_subagent_add_tool_runtime(
    turbo_tool_runtime_t *runtime, const turbo_agent_subagent_tool_config_t *config) {
  turbo_tool_definition_t definition = {0};
  turbo_tool_status_t status;

  status = turbo_agent_subagent_prepare_definition(config, &definition);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

  status = turbo_tool_runtime_native_add_tool(runtime, &definition);
  if (status != TURBO_TOOL_OK && definition.user_data_free) {
    definition.user_data_free(definition.user_data);
  }
  return status;
}

CXX_C_API turbo_tool_status_t turbo_agent_subagent_add_tool_registry(
    turbo_tool_registry_t *registry, const turbo_agent_subagent_tool_config_t *config) {
  turbo_tool_definition_t definition = {0};
  turbo_tool_status_t status;

  status = turbo_agent_subagent_prepare_definition(config, &definition);
  if (status != TURBO_TOOL_OK) {
    return status;
  }

  status = turbo_tool_registry_add(registry, &definition);
  if (status != TURBO_TOOL_OK && definition.user_data_free) {
    definition.user_data_free(definition.user_data);
  }
  return status;
}
