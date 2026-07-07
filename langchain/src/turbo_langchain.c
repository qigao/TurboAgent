#include "turbo_langchain.h"

turbo_runtime_data_bind_value_t *turbo_langchain_messages_create(void) {
  return turbo_prompt_messages_create_bind();
}

turbo_prompt_status_t
turbo_langchain_messages_append(turbo_runtime_data_bind_value_t *messages,
                                const char *role, const char *content) {
  return turbo_prompt_messages_append_bind(messages, role, content);
}

char *turbo_langchain_prompt_render_bind(
    const char *template_text, const turbo_runtime_data_bind_value_t *input) {
  return turbo_prompt_render_template_bind(template_text, input);
}

turbo_langchain_runnable_t *
turbo_langchain_runnable_create(const turbo_runnable_config_t *config) {
  return turbo_runnable_create(config);
}

void turbo_langchain_runnable_destroy(turbo_langchain_runnable_t *runnable) {
  turbo_runnable_destroy(runnable);
}

int turbo_langchain_runnable_invoke(
    const turbo_langchain_runnable_t *runnable,
    const turbo_runtime_data_bind_value_t *input,
    turbo_runtime_data_bind_value_t **out_output) {
  return turbo_runnable_invoke_bind(runnable, input, out_output);
}

int turbo_langchain_runnable_stream(
    const turbo_langchain_runnable_t *runnable,
    const turbo_runtime_data_bind_value_t *input,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_output) {
  return turbo_runnable_invoke_bind_stream(runnable, input, event_sink,
                                           event_sink_user_data, out_output);
}

int turbo_langchain_runnable_log(
    const turbo_langchain_runnable_t *runnable,
    const turbo_runtime_data_bind_value_t *input, turbo_langchain_event_log_t *log,
    turbo_runtime_data_bind_value_t **out_output) {
  return turbo_runnable_invoke_bind_log(runnable, input, log, out_output);
}

int turbo_langchain_runnable_batch(
    const turbo_langchain_runnable_t *runnable,
    const turbo_runtime_data_bind_value_t *inputs,
    turbo_runtime_data_bind_value_t **out_outputs) {
  return turbo_runnable_batch_bind(runnable, inputs, out_outputs);
}

turbo_langchain_runnable_t *turbo_langchain_pipe(
    const turbo_langchain_runnable_t *first,
    const turbo_langchain_runnable_t *second) {
  return turbo_runnable_pipe(first, second);
}

turbo_langchain_runnable_t *turbo_langchain_runnable_wrap(
    const turbo_langchain_runnable_t *inner,
    const turbo_runnable_wrap_config_t *config) {
  return turbo_runnable_wrap_bind(inner, config);
}

turbo_langchain_runnable_t *
turbo_langchain_runnable_from_chain(turbo_langchain_chain_t *chain) {
  return turbo_runnable_from_chain(chain);
}

turbo_langchain_runnable_t *turbo_langchain_runnable_from_graph(
    turbo_langchain_graph_t *graph, const turbo_graph_run_options_t *options,
    turbo_graph_run_result_t *result_sink) {
  return turbo_runnable_from_graph(graph, options, result_sink);
}

turbo_langchain_runnable_t *turbo_langchain_runnable_from_state_graph(
    turbo_langchain_state_graph_t *graph, const char *thread_id,
    const turbo_state_graph_run_options_t *options,
    turbo_state_graph_run_result_t *result_sink) {
  return turbo_runnable_from_state_graph(graph, thread_id, options, result_sink);
}

turbo_langchain_runnable_t *turbo_langchain_runnable_from_session(
    turbo_langchain_session_t *session, const turbo_graph_run_options_t *options) {
  return turbo_runnable_from_agent_session(session, options);
}

turbo_langchain_runnable_t *turbo_langchain_runnable_from_agent(
    turbo_langchain_agent_t *agent, const turbo_graph_run_options_t *options) {
  return turbo_runnable_from_agent_app(agent, options);
}

turbo_langchain_chain_t *turbo_langchain_chain_create(const char *name) {
  return turbo_chain_create(name);
}

void turbo_langchain_chain_destroy(turbo_langchain_chain_t *chain) {
  turbo_chain_destroy(chain);
}

turbo_chain_status_t turbo_langchain_chain_add_prompt(
    turbo_langchain_chain_t *chain, const char *name, const char *role,
    const char *template_text) {
  return turbo_chain_add_prompt_step(chain, name, role, template_text);
}

turbo_chain_status_t turbo_langchain_chain_add_model(
    turbo_langchain_chain_t *chain, const char *name, const turbo_model_t *model,
    const turbo_langchain_tool_registry_t *tools) {
  return turbo_chain_add_model_step(chain, name, model, tools);
}

turbo_chain_status_t turbo_langchain_chain_add_tools(
    turbo_langchain_chain_t *chain, const char *name,
    const turbo_langchain_tool_registry_t *tools) {
  return turbo_chain_add_tool_step(chain, name, tools);
}

turbo_chain_status_t turbo_langchain_chain_run(
    turbo_langchain_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_chain_run_bind(chain, state, out_state);
}

turbo_chain_status_t turbo_langchain_chain_stream(
    turbo_langchain_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
    turbo_event_sink_bind_fn event_sink, void *event_sink_user_data,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_chain_run_bind_stream(chain, state, event_sink, event_sink_user_data,
                                     out_state);
}

turbo_chain_status_t turbo_langchain_chain_log(
    turbo_langchain_chain_t *chain, const turbo_runtime_data_bind_value_t *state,
    turbo_langchain_event_log_t *log, turbo_runtime_data_bind_value_t **out_state) {
  return turbo_chain_run_bind_log(chain, state, log, out_state);
}

turbo_runtime_data_bind_value_t *turbo_langchain_chain_state_create(void) {
  return turbo_chain_state_create_bind();
}

turbo_langchain_tool_registry_t *turbo_langchain_tools_create(void) {
  return turbo_tool_registry_create();
}

void turbo_langchain_tools_destroy(turbo_langchain_tool_registry_t *tools) {
  turbo_tool_registry_destroy(tools);
}

turbo_tool_status_t turbo_langchain_tools_add(
    turbo_langchain_tool_registry_t *tools, const turbo_tool_definition_t *definition) {
  return turbo_tool_registry_add(tools, definition);
}

turbo_tool_status_t turbo_langchain_tool_invoke(
    const turbo_langchain_tool_registry_t *tools, const char *name,
    const char *arguments_json, char **out_output) {
  return turbo_tool_registry_execute(tools, name, arguments_json, out_output);
}

turbo_tool_status_t turbo_langchain_tool_invoke_bind(
    const turbo_langchain_tool_registry_t *tools, const char *name,
    const turbo_runtime_data_bind_value_t *arguments,
    turbo_runtime_data_bind_value_t **out_result) {
  return turbo_tool_registry_execute_bind(tools, name, arguments, out_result);
}

turbo_langchain_agent_t *
turbo_langchain_agent_create(const turbo_agent_session_config_t *session_config) {
  turbo_agent_app_config_t app_config;

  app_config.session_config = session_config;
  return turbo_agent_app_create(&app_config);
}

void turbo_langchain_agent_destroy(turbo_langchain_agent_t *agent) {
  turbo_agent_app_destroy(agent);
}

int turbo_langchain_agent_invoke_text(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json) {
  return turbo_agent_app_invoke_text(agent, user_text, options, out_text,
                                     out_summary_json);
}

int turbo_langchain_agent_invoke_json(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  return turbo_agent_app_invoke_json(agent, user_text, options, out_json,
                                     out_summary_json);
}

int turbo_langchain_agent_start_text(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    turbo_runtime_data_bind_value_t **out_state) {
  return turbo_agent_app_start_text(agent, user_text, options, out_summary_json,
                                    out_state);
}

int turbo_langchain_agent_memory_put_context(
    const turbo_langchain_agent_t *agent, const char *memory_namespace,
    const char *key, const char *scope, const char *path, const char *text) {
  return turbo_agent_app_memory_put_context(agent, memory_namespace, key, scope, path,
                                            text);
}

int turbo_langchain_agent_memory_query(
    const turbo_langchain_agent_t *agent, const char *namespace_prefix,
    const char *kind, const char *key_prefix, const char *text_substring,
    json_value_t **out_records_json) {
  return turbo_agent_app_memory_query_records(agent, namespace_prefix, kind, key_prefix,
                                              text_substring, out_records_json);
}

turbo_langchain_session_t *
turbo_langchain_session_create(const turbo_agent_session_config_t *config) {
  return turbo_agent_session_create(config);
}

void turbo_langchain_session_destroy(turbo_langchain_session_t *session) {
  turbo_agent_session_destroy(session);
}

int turbo_langchain_session_invoke_text(
    turbo_langchain_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json) {
  return turbo_agent_session_invoke_text(session, user_text, options, out_text,
                                         out_summary_json);
}

int turbo_langchain_session_invoke_json(
    turbo_langchain_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json) {
  return turbo_agent_session_invoke_json(session, user_text, options, out_json,
                                         out_summary_json);
}

turbo_agent_memory_store_t turbo_langchain_memory_store_create(void) {
  return turbo_agent_memory_store_memory_create();
}

turbo_agent_memory_store_t
turbo_langchain_file_memory_store_create(const char *root_dir) {
  return turbo_agent_memory_store_file_create(root_dir);
}

void turbo_langchain_memory_store_destroy(turbo_agent_memory_store_t *store) {
  turbo_agent_memory_store_destroy(store);
}

turbo_langchain_event_log_t *turbo_langchain_event_log_create(void) {
  return turbo_event_log_create();
}

void turbo_langchain_event_log_destroy(turbo_langchain_event_log_t *log) {
  turbo_event_log_destroy(log);
}
