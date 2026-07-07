#include "turbo_agent_subgraph.h"

#include "turbo_agent_runtime_internal.h"

#include <string.h>

static const char *turbo_agent_subgraph_nonempty(const char *value, const char *fallback) {
  return value && value[0] != '\0' ? value : fallback;
}

static int turbo_agent_subgraph_copy_summary_string(json_value_t *result,
                                                    const json_value_t *summary,
                                                    const char *key) {
  const json_value_t *value;
  const char *text;

  if (!result || !summary || !key) {
    return -1;
  }
  value = turbo_json_object_get(summary, key);
  if (value && turbo_json_type(value) == TURBO_JSON_STRING) {
    text = turbo_json_get_string(summary, key);
    turbo_json_object_set_string(result, key, text ? text : "");
  } else {
    turbo_json_object_set_null(result, key);
  }
  return 0;
}

static void turbo_agent_subgraph_add_pending_string(json_value_t *result,
                                                    const json_value_t *summary,
                                                    const char *result_key,
                                                    const char *summary_key,
                                                    int is_interrupted) {
  const char *text = NULL;

  if (!result || !result_key) {
    return;
  }
  if (is_interrupted && summary && summary_key) {
    text = turbo_json_get_string(summary, summary_key);
  }
  if (text && text[0] != '\0') {
    turbo_json_object_set_string(result, result_key, text);
  } else {
    turbo_json_object_set_null(result, result_key);
  }
}

static json_value_t *turbo_agent_subgraph_result_json(
    const json_value_t *summary, const turbo_runtime_data_bind_value_t *state) {
  json_value_t *result = NULL;
  json_value_t *state_json = NULL;
  json_value_t *summary_clone = NULL;
  const char *status;
  int is_completed;
  int is_interrupted;
  int is_failed;

  if (!summary || !state || turbo_json_type(summary) != TURBO_JSON_OBJECT) {
    return NULL;
  }

  result = turbo_json_create_object();
  state_json = turbo_runtime_data_bind_value_to_json(state);
  summary_clone = turbo_json_clone(summary);
  if (!result || !state_json || !summary_clone) {
    turbo_free_json(&result);
    turbo_free_json(&state_json);
    turbo_free_json(&summary_clone);
    return NULL;
  }

  status = turbo_json_get_string(summary, "status");
  is_completed = status && strcmp(status, "completed") == 0;
  is_interrupted = status && strcmp(status, "interrupted") == 0;
  is_failed = status && strcmp(status, "failed") == 0;
  turbo_json_object_set_bool(result, "ok", is_completed || is_interrupted ? true : false);
  turbo_json_object_set_string(result, "kind", "subgraph_result");
  turbo_json_object_set_bool(result, "completed", is_completed ? true : false);
  turbo_json_object_set_bool(result, "interrupted", is_interrupted ? true : false);
  turbo_json_object_set_bool(result, "failed", is_failed ? true : false);
  turbo_agent_subgraph_copy_summary_string(result, summary, "status");
  turbo_agent_subgraph_copy_summary_string(result, summary, "thread_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "run_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "checkpoint_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "parent_agent_run_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "parent_tool_call_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "parent_tool_name");
  turbo_agent_subgraph_copy_summary_string(result, summary, "parent_graph_run_id");
  turbo_agent_subgraph_copy_summary_string(result, summary, "call_frame_id");
  turbo_agent_subgraph_add_pending_string(result, summary, "pending_checkpoint_id",
                                          "checkpoint_id", is_interrupted);
  turbo_agent_subgraph_add_pending_string(result, summary, "pending_node", "pending_node",
                                          is_interrupted);
  turbo_json_object_add(result, "summary", summary_clone);
  summary_clone = NULL;
  turbo_json_object_add(result, "state", state_json);
  state_json = NULL;
  return result;
}

CXX_C_API int turbo_agent_subgraph_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  const turbo_agent_subgraph_node_config_t *config =
      (const turbo_agent_subgraph_node_config_t *)user_data;
  turbo_agent_execution_context_t current_context = {0};
  turbo_agent_runtime_parent_link_t parent_link = {0};
  turbo_runtime_data_bind_value_t *child_state = NULL;
  turbo_runtime_data_bind_value_t *result_bind = NULL;
  json_value_t *summary = NULL;
  json_value_t *result_json = NULL;
  const char *call_frame_id;
  const char *output_key;
  int rc = -1;

  if (!ctx || !ctx->bind_state || !config || !config->runtime || !config->subgraph) {
    return -1;
  }

  turbo_agent_execution_context_get(&current_context);
  call_frame_id = turbo_agent_subgraph_nonempty(config->call_frame_id, ctx->current_node);
  output_key = turbo_agent_subgraph_nonempty(config->output_key, "subgraph_result");

  parent_link.parent_agent_run_id = current_context.run_id;
  parent_link.parent_tool_call_id = call_frame_id;
  parent_link.parent_tool_name = ctx->current_node;
  parent_link.parent_graph_run_id =
      turbo_agent_subgraph_nonempty(config->parent_graph_run_id, current_context.run_id);
  parent_link.call_frame_id = call_frame_id;
  turbo_agent_runtime_exec_options_t exec_options = {0};
  exec_options.thread_id = config->thread_id;
  exec_options.parent_link = &parent_link;

  if (turbo_agent_runtime_exec_start(
          config->runtime, config->subgraph, ctx->bind_state, config->options,
          &exec_options, &summary, &child_state) != 0 ||
      !summary || !child_state) {
    goto cleanup;
  }

  result_json = turbo_agent_subgraph_result_json(summary, child_state);
  if (!result_json) {
    goto cleanup;
  }
  result_bind = turbo_runtime_data_bind_value_from_json(result_json);
  if (!result_bind) {
    goto cleanup;
  }
  if (turbo_runtime_data_bind_object_set(ctx->bind_state, output_key, result_bind) !=
      TURBO_RUNTIME_DATA_BIND_OK) {
    goto cleanup;
  }
  result_bind = NULL;
  rc = 0;

cleanup:
  turbo_runtime_data_bind_value_destroy(result_bind);
  turbo_free_json(&result_json);
  turbo_free_json(&summary);
  turbo_runtime_data_bind_value_destroy(child_state);
  return rc;
}

CXX_C_API turbo_graph_exec_status_t turbo_agent_install_subgraph_node(
    turbo_graph_t *graph, const char *node_name, const char *semantic_id,
    const turbo_agent_subgraph_node_config_t *config) {
  if (!graph || !node_name || !config || !config->runtime || !config->subgraph) {
    return TURBO_GRAPH_EXEC_INVALID_ARGUMENT;
  }
  return turbo_graph_add_bind_node_ex(graph, node_name, semantic_id, turbo_agent_subgraph_node,
                                      (void *)config);
}
