#include "turbo_retriever.h"

#include "turbo_agent_state.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_workflow.h"
#include "turbo_embedding.h"
#include "turbo_vector_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_retriever_s {
  turbo_retriever_query_fn query;
  void *user_data;
  turbo_retriever_user_data_free_fn user_data_free;
};

typedef struct turbo_retriever_vector_store_adapter_s {
  turbo_vector_store_t *store;
  turbo_embedding_model_t *embedding_model;
} turbo_retriever_vector_store_adapter_t;

static const char *turbo_retriever_latest_user_query(const json_value_t *state) {
  const json_value_t *input;
  size_t i;

  if (!state || turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return NULL;
  }
  input = turbo_json_object_get(state, "input");
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY) {
    return NULL;
  }
  for (i = turbo_json_array_size(input); i > 0; --i) {
    const json_value_t *message = turbo_json_array_get(input, i - 1);
    const json_value_t *content;
    const char *role;
    const char *text;

    if (!message || turbo_json_type(message) != TURBO_JSON_OBJECT) {
      continue;
    }
    role = turbo_json_get_string(message, "role");
    if (!role || strcmp(role, "user") != 0) {
      continue;
    }
    content = turbo_json_object_get(message, "content");
    if (!content || turbo_json_type(content) != TURBO_JSON_STRING) {
      continue;
    }
    text = turbo_json_get_string(message, "content");
    if (text && text[0] != '\0') {
      return text;
    }
  }
  return NULL;
}

turbo_retriever_t *
turbo_retriever_create(const turbo_retriever_config_t *config) {
  turbo_retriever_t *retriever;

  if (!config || !config->query) {
    return NULL;
  }
  retriever = (turbo_retriever_t *)calloc(1, sizeof(*retriever));
  if (!retriever) {
    return NULL;
  }
  retriever->query = config->query;
  retriever->user_data = config->user_data;
  retriever->user_data_free = config->user_data_free;
  return retriever;
}

void turbo_retriever_destroy(turbo_retriever_t *retriever) {
  if (!retriever) {
    return;
  }
  if (retriever->user_data_free) {
    retriever->user_data_free(retriever->user_data);
  }
  free(retriever);
}

int turbo_retriever_query(
    turbo_retriever_t *retriever, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  turbo_retriever_query_options_t default_options = {0};

  if (!retriever || !retriever->query || !query || !query[0] ||
      !out_results_json) {
    return -1;
  }
  return retriever->query(retriever->user_data, query,
                          options ? options : &default_options,
                          out_results_json);
}

static int turbo_retriever_context_append(char **buffer, size_t *capacity,
                                          size_t *length, const char *text) {
  size_t text_len;
  size_t needed;
  char *next;

  if (!buffer || !capacity || !length || !text) {
    return -1;
  }
  text_len = strlen(text);
  needed = *length + text_len + 2;
  if (needed > *capacity) {
    size_t next_capacity = *capacity ? *capacity : 256;

    while (next_capacity < needed) {
      next_capacity *= 2;
    }
    next = (char *)realloc(*buffer, next_capacity);
    if (!next) {
      return -1;
    }
    *buffer = next;
    *capacity = next_capacity;
  }
  memcpy(*buffer + *length, text, text_len);
  *length += text_len;
  (*buffer)[(*length)++] = '\n';
  (*buffer)[*length] = '\0';
  return 0;
}

int turbo_retriever_build_context(
    turbo_retriever_t *retriever, const char *query,
    const turbo_retriever_query_options_t *options,
    json_value_t **out_context_json) {
  turbo_retriever_query_options_t effective_options = {0};
  json_value_t *results = NULL;
  json_value_t *context = NULL;
  json_value_t *layers = NULL;
  char *context_text = NULL;
  size_t context_capacity = 0;
  size_t context_len = 0;
  size_t result_count;
  size_t i;
  int rc = -1;

  if (!retriever || !query || !query[0] || !out_context_json) {
    return -1;
  }
  *out_context_json = NULL;
  if (options) {
    effective_options = *options;
  }
  if (turbo_retriever_query(retriever, query, &effective_options, &results) !=
      0) {
    return -1;
  }
  context = turbo_json_create_object();
  layers = turbo_json_create_array();
  if (!context || !layers) {
    goto cleanup;
  }

  result_count = turbo_json_array_size(results);
  for (i = 0; i < result_count; ++i) {
    const json_value_t *result = turbo_json_array_get(results, i);
    const char *text = turbo_json_get_string(result, "text");
    const char *uri = turbo_json_get_string(result, "uri");
    json_value_t *layer;

    if (!text || !text[0]) {
      continue;
    }
    layer = turbo_json_create_object();
    if (!layer) {
      goto cleanup;
    }
    turbo_json_object_set_string(layer, "scope", "retriever");
    turbo_json_object_set_string(layer, "path", uri ? uri : "");
    turbo_json_object_set_string(layer, "text", text);
    turbo_json_array_add(layers, layer);
    if (turbo_retriever_context_append(&context_text, &context_capacity,
                                       &context_len, text) != 0) {
      goto cleanup;
    }
  }

  turbo_json_object_set_string(context, "query", query);
  turbo_json_object_set_string(context, "kind",
                               effective_options.kind ? effective_options.kind : "");
  turbo_json_object_set_string(
      context, "uri_prefix",
      effective_options.uri_prefix ? effective_options.uri_prefix : "");
  turbo_json_object_set_number(context, "layer_count",
                               (double)turbo_json_array_size(layers));
  turbo_json_object_set_string(context, "context_text",
                               context_text ? context_text : "");
  turbo_json_object_add(context, "layers", layers);
  layers = NULL;
  turbo_json_object_add(context, "evidence", results);
  results = NULL;
  *out_context_json = context;
  context = NULL;
  rc = 0;

cleanup:
  free(context_text);
  turbo_free_json(&layers);
  turbo_free_json(&context);
  turbo_free_json(&results);
  return rc;
}

int turbo_retriever_load_context(
    turbo_retriever_t *retriever, json_value_t *state, const char *query,
    const turbo_retriever_query_options_t *options, const char *scope) {
  turbo_retriever_query_options_t effective_options = {0};
  json_value_t *results = NULL;
  size_t i;
  int rc = 0;

  if (!retriever || !state || !query || !query[0] ||
      turbo_json_type(state) != TURBO_JSON_OBJECT) {
    return -1;
  }
  if (options) {
    effective_options = *options;
  }
  if (turbo_retriever_query(retriever, query, &effective_options, &results) !=
      0) {
    return -1;
  }
  if (!results || turbo_json_type(results) != TURBO_JSON_ARRAY) {
    turbo_free_json(&results);
    return -1;
  }

  for (i = 0; i < turbo_json_array_size(results); ++i) {
    const json_value_t *item = turbo_json_array_get(results, i);
    const char *uri;
    const char *text;

    if (!item || turbo_json_type(item) != TURBO_JSON_OBJECT) {
      continue;
    }
    uri = turbo_json_get_string(item, "uri");
    text = turbo_json_get_string(item, "text");
    if (text && text[0] != '\0' &&
        turbo_agent_state_add_memory_context_layer(
            state, scope && scope[0] ? scope : "retriever", uri, text) != 0) {
      rc = -1;
      break;
    }
  }

  turbo_free_json(&results);
  return rc;
}

int turbo_retriever_context_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const turbo_retriever_context_config_t *config =
      (const turbo_retriever_context_config_t *)user_data;
  turbo_retriever_query_options_t options = {0};
  const char *query;

  if (!ctx || !ctx->state || !config || !config->retriever) {
    return -1;
  }
  query = config->query && config->query[0] != '\0'
              ? config->query
              : turbo_retriever_latest_user_query(ctx->state);
  if (!query || query[0] == '\0') {
    return 0;
  }
  options.kind = config->kind;
  options.uri_prefix = config->uri_prefix;
  options.limit = config->limit;
  return turbo_retriever_load_context(config->retriever, ctx->state, query,
                                      &options, config->scope);
}

turbo_graph_exec_status_t
turbo_agent_install_retriever_engineering_loop(
    turbo_graph_t *graph, const turbo_retriever_context_config_t *retriever_config,
    turbo_agent_t *planner_agent, turbo_agent_t *executor_agent,
    const char *retriever_node_name, const char *planner_node_name,
    const char *plan_commit_node_name, const char *plan_step_node_name,
    const char *review_node_name, const char *executor_node_name,
    const char *tool_node_name, const char *detect_failure_node_name,
    const char *replan_route_node_name, const char *replan_prepare_node_name,
    const char *plan_advance_node_name, const char *end_node_name, int set_entry) {
  turbo_graph_exec_status_t status;

  if (!graph || !retriever_config || !retriever_config->retriever ||
      !planner_agent || !executor_agent || !retriever_node_name ||
      !planner_node_name || !plan_commit_node_name || !plan_step_node_name ||
      !review_node_name || !executor_node_name || !tool_node_name ||
      !detect_failure_node_name || !replan_route_node_name ||
      !replan_prepare_node_name || !plan_advance_node_name || !end_node_name) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }

  status = turbo_graph_add_node(graph, retriever_node_name,
                                turbo_retriever_context_node,
                                (void *)retriever_config);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_agent_install_engineering_loop(
      graph, planner_agent, executor_agent, planner_node_name,
      plan_commit_node_name, plan_step_node_name, review_node_name,
      executor_node_name, tool_node_name, detect_failure_node_name,
      replan_route_node_name, replan_prepare_node_name, plan_advance_node_name,
      end_node_name, 0);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  status = turbo_graph_add_edge(graph, retriever_node_name, planner_node_name,
                                NULL, NULL);
  if (status != TURBO_GRAPH_EXEC_OK) {
    return status;
  }

  return set_entry ? turbo_graph_set_entry(graph, retriever_node_name)
                   : TURBO_GRAPH_EXEC_OK;
}

static int turbo_retriever_knowledge_store_query(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  turbo_agent_knowledge_store_t *store =
      (turbo_agent_knowledge_store_t *)user_data;

  if (!store || !options) {
    return -1;
  }
  return turbo_agent_knowledge_store_query_ex(
      store, query, options->kind, options->uri_prefix, options->limit,
      out_results_json);
}

turbo_retriever_t *
turbo_retriever_from_knowledge_store(turbo_agent_knowledge_store_t *store) {
  turbo_retriever_config_t config = {0};

  if (!store) {
    return NULL;
  }
  config.query = turbo_retriever_knowledge_store_query;
  config.user_data = store;
  return turbo_retriever_create(&config);
}

static int turbo_retriever_vector_store_query(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  turbo_retriever_vector_store_adapter_t *adapter =
      (turbo_retriever_vector_store_adapter_t *)user_data;
  turbo_vector_store_query_options_t vector_options = {0};
  json_value_t *query_embedding = NULL;
  int rc;

  if (!adapter || !adapter->store || !adapter->embedding_model || !options) {
    return -1;
  }
  rc = turbo_embedding_model_embed_text(adapter->embedding_model, query,
                                        &query_embedding);
  if (rc != 0) {
    return -1;
  }
  vector_options.kind = options->kind;
  vector_options.uri_prefix = options->uri_prefix;
  vector_options.limit = options->limit;
  rc = turbo_vector_store_query(adapter->store, query_embedding, &vector_options,
                                out_results_json);
  turbo_free_json(&query_embedding);
  return rc;
}

turbo_retriever_t *turbo_retriever_from_vector_store(
    turbo_vector_store_t *store, turbo_embedding_model_t *embedding_model) {
  turbo_retriever_vector_store_adapter_t *adapter;
  turbo_retriever_config_t config = {0};
  turbo_retriever_t *retriever;

  if (!store || !embedding_model) {
    return NULL;
  }
  adapter = (turbo_retriever_vector_store_adapter_t *)calloc(1, sizeof(*adapter));
  if (!adapter) {
    return NULL;
  }
  adapter->store = store;
  adapter->embedding_model = embedding_model;
  config.query = turbo_retriever_vector_store_query;
  config.user_data = adapter;
  config.user_data_free = free;
  retriever = turbo_retriever_create(&config);
  if (!retriever) {
    free(adapter);
  }
  return retriever;
}
