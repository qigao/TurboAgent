#ifndef TURBO_LANGCHAIN_H
#define TURBO_LANGCHAIN_H

#include "turbo_action_tool.h"
#include "turbo_agent_policy.h"
#include "turbo_chain.h"
#include "turbo_document_loader.h"
#include "turbo_embedding.h"
#include "turbo_event.h"
#include "turbo_event_log.h"
#include "turbo_graph.h"
#include "turbo_graph_run_log.h"
#include "turbo_agent.h"
#include "turbo_agent_app.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent_execution.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_harness_server.h"
#include "turbo_agent_harness_transport.h"
#include "turbo_agent_context.h"
#include "turbo_agent_inbox.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_memory_store.h"
#include "turbo_agent_remote_app.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_runtime_remote.h"
#include "turbo_agent_runtime_remote_client.h"
#include "turbo_agent_runtime_remote_chttp.h"
#include "turbo_agent_session.h"
#include "turbo_agent_subagent.h"
#include "turbo_agent_subgraph.h"
#include "turbo_agent_workspace.h"
#include "turbo_agent_state.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_workflow.h"
#include "turbo_agent_sse.h"
#include "turbo_model.h"
#include "turbo_model_provider.h"
#include "turbo_prompt.h"
#include "turbo_retriever.h"
#include "turbo_runnable.h"
#include "turbo_state_graph.h"
#include "turbo_state_graph_inspect.h"
#include "turbo_state_graph_runtime.h"
#include "turbo_state_graph_store.h"
#include "turbo_text_splitter.h"
#include "turbo_runtime_json.h"
#include "turbo_tool.h"
#include "turbo_tool_runtime.h"
#include "turbo_tool_registry.h"
#include "turbo_tool_schema.h"
#include "turbo_vector_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef turbo_agent_app_t turbo_langchain_agent_t;
typedef turbo_agent_session_t turbo_langchain_session_t;
typedef turbo_chain_t turbo_langchain_chain_t;
typedef turbo_event_log_t turbo_langchain_event_log_t;
typedef turbo_graph_t turbo_langchain_graph_t;
typedef turbo_runnable_t turbo_langchain_runnable_t;
typedef turbo_state_graph_t turbo_langchain_state_graph_t;
typedef turbo_tool_registry_t turbo_langchain_tool_registry_t;

/* Prompt */
CXX_C_API json_value_t *turbo_langchain_messages_create(void);
CXX_C_API turbo_prompt_status_t
turbo_langchain_messages_append(json_value_t *messages,
                                const char *role, const char *content);
CXX_C_API char *turbo_langchain_prompt_render_json_value(
    const char *template_text, const json_value_t *input);

/* Runnable */
CXX_C_API turbo_langchain_runnable_t *
turbo_langchain_runnable_create(const turbo_runnable_config_t *config);
CXX_C_API void turbo_langchain_runnable_destroy(turbo_langchain_runnable_t *runnable);
CXX_C_API int turbo_langchain_runnable_invoke(
    const turbo_langchain_runnable_t *runnable,
    const json_value_t *input,
    json_value_t **out_output);
CXX_C_API int turbo_langchain_runnable_stream(
    const turbo_langchain_runnable_t *runnable,
    const json_value_t *input,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_output);
CXX_C_API int turbo_langchain_runnable_log(
    const turbo_langchain_runnable_t *runnable,
    const json_value_t *input, turbo_langchain_event_log_t *log,
    json_value_t **out_output);
CXX_C_API int turbo_langchain_runnable_batch(
    const turbo_langchain_runnable_t *runnable,
    const json_value_t *inputs,
    json_value_t **out_outputs);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_pipe(
    const turbo_langchain_runnable_t *first,
    const turbo_langchain_runnable_t *second);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_runnable_wrap(
    const turbo_langchain_runnable_t *inner,
    const turbo_runnable_wrap_config_t *config);
CXX_C_API turbo_langchain_runnable_t *
turbo_langchain_runnable_from_chain(turbo_langchain_chain_t *chain);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_runnable_from_graph(
    turbo_langchain_graph_t *graph, const turbo_graph_run_options_t *options,
    turbo_graph_run_result_t *result_sink);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_runnable_from_state_graph(
    turbo_langchain_state_graph_t *graph, const char *thread_id,
    const turbo_state_graph_run_options_t *options,
    turbo_state_graph_run_result_t *result_sink);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_runnable_from_session(
    turbo_langchain_session_t *session, const turbo_graph_run_options_t *options);
CXX_C_API turbo_langchain_runnable_t *turbo_langchain_runnable_from_agent(
    turbo_langchain_agent_t *agent, const turbo_graph_run_options_t *options);

/* Chain */
CXX_C_API turbo_langchain_chain_t *turbo_langchain_chain_create(const char *name);
CXX_C_API void turbo_langchain_chain_destroy(turbo_langchain_chain_t *chain);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_add_prompt(
    turbo_langchain_chain_t *chain, const char *name, const char *role,
    const char *template_text);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_add_model(
    turbo_langchain_chain_t *chain, const char *name, const turbo_model_t *model,
    const turbo_langchain_tool_registry_t *tools);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_add_tools(
    turbo_langchain_chain_t *chain, const char *name,
    const turbo_langchain_tool_registry_t *tools);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_run(
    turbo_langchain_chain_t *chain, const json_value_t *state,
    json_value_t **out_state);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_stream(
    turbo_langchain_chain_t *chain, const json_value_t *state,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_state);
CXX_C_API turbo_chain_status_t turbo_langchain_chain_log(
    turbo_langchain_chain_t *chain, const json_value_t *state,
    turbo_langchain_event_log_t *log, json_value_t **out_state);
CXX_C_API json_value_t *turbo_langchain_chain_state_create(void);

/* Tools */
CXX_C_API turbo_langchain_tool_registry_t *turbo_langchain_tools_create(void);
CXX_C_API void turbo_langchain_tools_destroy(turbo_langchain_tool_registry_t *tools);
CXX_C_API turbo_tool_status_t turbo_langchain_tools_add(
    turbo_langchain_tool_registry_t *tools, const turbo_tool_definition_t *definition);
CXX_C_API turbo_tool_status_t turbo_langchain_tool_invoke(
    const turbo_langchain_tool_registry_t *tools, const char *name,
    const char *arguments_json, char **out_output);
CXX_C_API turbo_tool_status_t turbo_langchain_tool_invoke_json_value(
    const turbo_langchain_tool_registry_t *tools, const char *name,
    const json_value_t *arguments,
    json_value_t **out_result);

/* Agent/session */
CXX_C_API turbo_langchain_agent_t *
turbo_langchain_agent_create(const turbo_agent_session_config_t *session_config);
CXX_C_API void turbo_langchain_agent_destroy(turbo_langchain_agent_t *agent);
CXX_C_API int turbo_langchain_agent_invoke_text(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json);
CXX_C_API int turbo_langchain_agent_invoke_json(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);
CXX_C_API int turbo_langchain_agent_start_text(
    turbo_langchain_agent_t *agent, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_summary_json,
    json_value_t **out_state);
CXX_C_API int turbo_langchain_agent_memory_put_context(
    const turbo_langchain_agent_t *agent, const char *memory_namespace,
    const char *key, const char *scope, const char *path, const char *text);
CXX_C_API int turbo_langchain_agent_memory_query(
    const turbo_langchain_agent_t *agent, const char *namespace_prefix,
    const char *kind, const char *key_prefix, const char *text_substring,
    json_value_t **out_records_json);

CXX_C_API turbo_langchain_session_t *
turbo_langchain_session_create(const turbo_agent_session_config_t *config);
CXX_C_API void turbo_langchain_session_destroy(turbo_langchain_session_t *session);
CXX_C_API int turbo_langchain_session_invoke_text(
    turbo_langchain_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, char **out_text,
    json_value_t **out_summary_json);
CXX_C_API int turbo_langchain_session_invoke_json(
    turbo_langchain_session_t *session, const char *user_text,
    const turbo_graph_run_options_t *options, json_value_t **out_json,
    json_value_t **out_summary_json);

/* Memory and events */
CXX_C_API turbo_agent_memory_store_t turbo_langchain_memory_store_create(void);
CXX_C_API turbo_agent_memory_store_t
turbo_langchain_file_memory_store_create(const char *root_dir);
CXX_C_API void turbo_langchain_memory_store_destroy(turbo_agent_memory_store_t *store);
CXX_C_API turbo_langchain_event_log_t *turbo_langchain_event_log_create(void);
CXX_C_API void turbo_langchain_event_log_destroy(turbo_langchain_event_log_t *log);

#ifdef __cplusplus
}
#endif

#endif
