#include "tinytest.h"
#include "turbo_langchain.h"

spec("turbo langchain public api") {

  it("should expose the aggregate public header as a usable core surface") {
    turbo_graph_t *graph = turbo_graph_create("public-api");
    turbo_chain_t *chain = turbo_chain_create("public-api");
    json_value_t *runtime_value =
        turbo_json_create_object();
    json_value_t *chain_state = turbo_chain_state_create_json_value();
    void *chain_run_json_value_stream = (void *)turbo_chain_run_json_value_stream;
    void *graph_run_json_value_stream = (void *)turbo_graph_run_json_value_stream;
    void *graph_run_json_value_stream_controlled =
        (void *)turbo_graph_run_json_value_stream_controlled;
    void *graph_run_checkpoint_json_value = (void *)turbo_graph_run_checkpoint_json_value;
    void *graph_run_checkpoint_json_value_stream = (void *)turbo_graph_run_checkpoint_json_value_stream;
    void *graph_run_checkpoint_json_value_stream_controlled =
        (void *)turbo_graph_run_checkpoint_json_value_stream_controlled;
    void *graph_run_log_create = (void *)turbo_graph_run_log_create;
    void *graph_run_json_value_log = (void *)turbo_graph_run_json_value_log;
    void *graph_checkpoint_clone = (void *)turbo_graph_checkpoint_clone;
    void *state_graph_create = (void *)turbo_state_graph_create;
    void *state_graph_add_channel = (void *)turbo_state_graph_add_channel;
    void *state_graph_add_json_value_node = (void *)turbo_state_graph_add_json_value_node;
    void *state_graph_add_subgraph_node = (void *)turbo_state_graph_add_subgraph_node;
    void *state_graph_add_json_value_edge = (void *)turbo_state_graph_add_json_value_edge;
    void *state_graph_snapshot_schema_version =
        (void *)turbo_state_graph_snapshot_schema_version;
    void *state_graph_serialize_snapshot = (void *)turbo_state_graph_serialize_snapshot;
    void *state_graph_load_snapshot = (void *)turbo_state_graph_load_snapshot;
    void *state_graph_ctx_goto = (void *)turbo_state_graph_ctx_goto;
    void *state_graph_ctx_send = (void *)turbo_state_graph_ctx_send;
    void *state_graph_start = (void *)turbo_state_graph_start;
    void *state_graph_resume = (void *)turbo_state_graph_resume;
    void *state_graph_update_state = (void *)turbo_state_graph_update_state;
    void *state_graph_resume_from_history = (void *)turbo_state_graph_resume_from_history;
    void *state_graph_fork_from_history = (void *)turbo_state_graph_fork_from_history;
    void *state_graph_get_state = (void *)turbo_state_graph_get_state;
    void *state_graph_get_run = (void *)turbo_state_graph_get_run;
    void *state_graph_get_thread = (void *)turbo_state_graph_get_thread;
    void *state_graph_get_latest_run = (void *)turbo_state_graph_get_latest_run;
    void *state_graph_get_pending_run = (void *)turbo_state_graph_get_pending_run;
    void *state_graph_get_thread_state = (void *)turbo_state_graph_get_thread_state;
    void *state_graph_get_history_entry = (void *)turbo_state_graph_get_history_entry;
    void *state_graph_get_history_entry_state =
        (void *)turbo_state_graph_get_history_entry_state;
    void *state_graph_get_checkpoint = (void *)turbo_state_graph_get_checkpoint;
    void *state_graph_list_state_history = (void *)turbo_state_graph_list_state_history;
    void *state_graph_list_checkpoints = (void *)turbo_state_graph_list_checkpoints;
    void *state_graph_list_runs = (void *)turbo_state_graph_list_runs;
    void *state_graph_get_checkpoint_context =
        (void *)turbo_state_graph_get_checkpoint_context;
    void *state_graph_get_branch_tree = (void *)turbo_state_graph_get_branch_tree;
    void *state_graph_store_memory_create = (void *)turbo_state_graph_store_memory_create;
    void *state_graph_store_file_create = (void *)turbo_state_graph_store_file_create;
    void *state_graph_store_destroy = (void *)turbo_state_graph_store_destroy;
    void *state_graph_store_delete = (void *)turbo_state_graph_store_delete;
    void *state_graph_store_list = (void *)turbo_state_graph_store_list;
    void *state_graph_store_get_snapshot_descriptor =
        (void *)turbo_state_graph_store_get_snapshot_descriptor;
    void *state_graph_store_list_snapshot_descriptors =
        (void *)turbo_state_graph_store_list_snapshot_descriptors;
    void *state_graph_store_list_snapshot_descriptors_filtered =
        (void *)turbo_state_graph_store_list_snapshot_descriptors_filtered;
    void *state_graph_store_get_latest_snapshot_descriptor =
        (void *)turbo_state_graph_store_get_latest_snapshot_descriptor;
    void *state_graph_store_get_latest_snapshot_descriptor_for_thread =
        (void *)turbo_state_graph_store_get_latest_snapshot_descriptor_for_thread;
    void *state_graph_store_get_latest_snapshot_descriptor_for_run =
        (void *)turbo_state_graph_store_get_latest_snapshot_descriptor_for_run;
    void *state_graph_store_get_latest_snapshot_descriptor_for_history_entry =
        (void *)turbo_state_graph_store_get_latest_snapshot_descriptor_for_history_entry;
    void *state_graph_store_get_latest_snapshot_descriptor_for_checkpoint =
        (void *)turbo_state_graph_store_get_latest_snapshot_descriptor_for_checkpoint;
    void *state_graph_store_get_latest_snapshot_id =
        (void *)turbo_state_graph_store_get_latest_snapshot_id;
    void *state_graph_store_get_latest_snapshot_id_for_thread =
        (void *)turbo_state_graph_store_get_latest_snapshot_id_for_thread;
    void *state_graph_store_get_latest_snapshot_id_for_run =
        (void *)turbo_state_graph_store_get_latest_snapshot_id_for_run;
    void *state_graph_store_get_latest_snapshot_id_for_history_entry =
        (void *)turbo_state_graph_store_get_latest_snapshot_id_for_history_entry;
    void *state_graph_store_get_latest_snapshot_id_for_checkpoint =
        (void *)turbo_state_graph_store_get_latest_snapshot_id_for_checkpoint;
    void *state_graph_store_load_latest_snapshot =
        (void *)turbo_state_graph_store_load_latest_snapshot;
    void *state_graph_store_load_latest_snapshot_for_thread =
        (void *)turbo_state_graph_store_load_latest_snapshot_for_thread;
    void *state_graph_store_load_latest_snapshot_for_run =
        (void *)turbo_state_graph_store_load_latest_snapshot_for_run;
    void *state_graph_store_load_latest_snapshot_for_history_entry =
        (void *)turbo_state_graph_store_load_latest_snapshot_for_history_entry;
    void *state_graph_store_load_latest_snapshot_for_checkpoint =
        (void *)turbo_state_graph_store_load_latest_snapshot_for_checkpoint;
    void *state_graph_store_save_snapshot =
        (void *)turbo_state_graph_store_save_snapshot;
    void *state_graph_store_load_snapshot =
        (void *)turbo_state_graph_store_load_snapshot;
    void *prompt_json_value = (void *)turbo_prompt_render_template_json_value;
    void *prompt_message_schema = (void *)turbo_prompt_message_schema_json_value;
    void *prompt_openai_codec = (void *)turbo_prompt_messages_to_openai_chat_json;
    void *prompt_responses_codec = (void *)turbo_prompt_messages_to_openai_responses_json;
    void *prompt_anthropic_codec = (void *)turbo_prompt_messages_to_anthropic_json;
    void *provider_wire_codec = (void *)turbo_model_provider_messages_to_wire_json;
    void *provider_event_codec = (void *)turbo_model_provider_response_to_event_json;
    void *provider_sse_event_codec = (void *)turbo_model_provider_sse_to_event_json;
    void *provider_event_json_value_codec = (void *)turbo_model_provider_response_to_event_json_value;
    void *provider_event_emit_json_value = (void *)turbo_model_provider_response_emit_json_value;
    void *provider_sse_event_json_value_codec = (void *)turbo_model_provider_sse_to_event_json_value;
    void *provider_sse_emit_json_value = (void *)turbo_model_provider_sse_emit_json_value;
    void *event_kind_json_value = (void *)turbo_event_kind_json_value;
    void *event_stream_mode_accepts_json_value =
        (void *)turbo_event_stream_mode_accepts_json_value;
    void *event_stream_filter_sink_json_value =
        (void *)turbo_event_stream_filter_sink_json_value;
    void *event_validate_any_json_value = (void *)turbo_event_validate_json_value;
    void *event_schema_json_value = (void *)turbo_event_model_schema_json_value;
    void *event_log_create = (void *)turbo_event_log_create;
    void *event_log_capture = (void *)turbo_event_log_capture_json_value;
    void *event_log_events = (void *)turbo_event_log_events_json_value;
    void *event_log_reset = (void *)turbo_event_log_reset;
    void *event_log_load_events = (void *)turbo_event_log_load_events_json_value;
    void *trace_event_schema_json_value = (void *)turbo_event_trace_schema_json_value;
    void *tool_result_event_schema_json_value = (void *)turbo_event_tool_result_schema_json_value;
    void *handoff_event_schema_json_value = (void *)turbo_event_handoff_schema_json_value;
    void *trace_event_validate_json_value = (void *)turbo_event_trace_validate_json_value;
    void *trace_event_create_json_value = (void *)turbo_event_trace_create_json_value;
    void *event_validate_json_value = (void *)turbo_event_model_validate_json_value;
    void *event_create_json_value = (void *)turbo_event_model_create_json_value;
    void *tool_result_event_validate_json_value = (void *)turbo_event_tool_result_validate_json_value;
    void *tool_result_event_create_json_value = (void *)turbo_event_tool_result_create_json_value;
    void *handoff_event_validate_json_value = (void *)turbo_event_handoff_validate_json_value;
    void *handoff_event_create_json_value = (void *)turbo_event_handoff_create_json_value;
    void *runnable_create = (void *)turbo_runnable_create;
    void *runnable_invoke_json_value = (void *)turbo_runnable_invoke_json_value;
    void *runnable_invoke_json_value_stream = (void *)turbo_runnable_invoke_json_value_stream;
    void *runnable_invoke_json_value_log = (void *)turbo_runnable_invoke_json_value_log;
    void *runnable_batch_json_value = (void *)turbo_runnable_batch_json_value;
    void *runnable_pipe = (void *)turbo_runnable_pipe;
    void *runnable_wrap_json_value = (void *)turbo_runnable_wrap_json_value;
    void *runnable_from_state_graph = (void *)turbo_runnable_from_state_graph;
    void *runnable_from_agent_session = (void *)turbo_runnable_from_agent_session;
    void *runnable_from_agent_app = (void *)turbo_runnable_from_agent_app;
    void *langchain_runnable_batch = (void *)turbo_langchain_runnable_batch;
    void *langchain_runnable_wrap = (void *)turbo_langchain_runnable_wrap;
    void *langchain_runnable_from_session =
        (void *)turbo_langchain_runnable_from_session;
    void *langchain_runnable_from_agent =
        (void *)turbo_langchain_runnable_from_agent;
    void *chain_run_json_value_log = (void *)turbo_chain_run_json_value_log;
    void *agent_policy_default = (void *)turbo_agent_policy_default;
    void *agent_policy_validate_path = (void *)turbo_agent_policy_validate_path;
    void *agent_policy_is_dangerous_command =
        (void *)turbo_agent_policy_is_dangerous_command;
    void *agent_policy_allows_capability =
        (void *)turbo_agent_policy_allows_capability;
    void *agent_policy_check_action = (void *)turbo_agent_policy_check_action;
    void *agent_policy_requires_approval =
        (void *)turbo_agent_policy_requires_approval;
    void *knowledge_store_sqlite_open =
        (void *)turbo_agent_knowledge_store_sqlite_open;
    void *knowledge_store_close = (void *)turbo_agent_knowledge_store_close;
    void *knowledge_store_upsert_text =
        (void *)turbo_agent_knowledge_store_upsert_text;
    void *knowledge_store_index_file =
        (void *)turbo_agent_knowledge_store_index_file;
    void *knowledge_store_index_directory =
        (void *)turbo_agent_knowledge_store_index_directory;
    void *knowledge_store_index_directory_ex =
        (void *)turbo_agent_knowledge_store_index_directory_ex;
    void *knowledge_store_delete_document =
        (void *)turbo_agent_knowledge_store_delete_document;
    void *knowledge_store_query = (void *)turbo_agent_knowledge_store_query;
    void *knowledge_store_query_ex =
        (void *)turbo_agent_knowledge_store_query_ex;
    void *knowledge_store_list_documents =
        (void *)turbo_agent_knowledge_store_list_documents;
    void *knowledge_store_get_document =
        (void *)turbo_agent_knowledge_store_get_document;
    void *knowledge_store_load_context =
        (void *)turbo_agent_knowledge_store_load_context;
    void *knowledge_store_load_context_ex =
        (void *)turbo_agent_knowledge_store_load_context_ex;
    void *knowledge_store_build_context =
        (void *)turbo_agent_knowledge_store_build_context;
    void *knowledge_store_stats =
        (void *)turbo_agent_knowledge_store_stats;
    void *knowledge_store_tool_graph =
        (void *)turbo_agent_knowledge_store_tool_graph;
    void *knowledge_store_add_tools =
        (void *)turbo_agent_knowledge_store_add_tools;
    void *knowledge_store_add_action_tools =
        (void *)turbo_agent_knowledge_store_add_action_tools;
    void *knowledge_context_node = (void *)turbo_agent_knowledge_context_node;
    void *knowledge_engineering_loop =
        (void *)turbo_agent_install_knowledge_engineering_loop;
    void *retriever_create = (void *)turbo_retriever_create;
    void *retriever_destroy = (void *)turbo_retriever_destroy;
    void *retriever_query = (void *)turbo_retriever_query;
    void *retriever_build_context = (void *)turbo_retriever_build_context;
    void *retriever_load_context = (void *)turbo_retriever_load_context;
    void *retriever_context_node = (void *)turbo_retriever_context_node;
    void *retriever_engineering_loop =
        (void *)turbo_agent_install_retriever_engineering_loop;
    void *retriever_from_knowledge_store =
        (void *)turbo_retriever_from_knowledge_store;
    void *retriever_from_vector_store =
        (void *)turbo_retriever_from_vector_store;
    void *embedding_model_create = (void *)turbo_embedding_model_create;
    void *embedding_model_destroy = (void *)turbo_embedding_model_destroy;
    void *embedding_model_embed_text = (void *)turbo_embedding_model_embed_text;
    void *embedding_model_create_hashing =
        (void *)turbo_embedding_model_create_hashing;
    void *vector_store_create_memory = (void *)turbo_vector_store_create_memory;
    void *vector_store_sqlite_open = (void *)turbo_vector_store_sqlite_open;
    void *vector_store_destroy = (void *)turbo_vector_store_destroy;
    void *vector_store_upsert = (void *)turbo_vector_store_upsert;
    void *vector_store_delete_document =
        (void *)turbo_vector_store_delete_document;
    void *vector_store_query = (void *)turbo_vector_store_query;
    void *vector_store_index_text = (void *)turbo_vector_store_index_text;
    void *vector_store_index_text_file =
        (void *)turbo_vector_store_index_text_file;
    void *vector_store_index_directory =
        (void *)turbo_vector_store_index_directory;
    void *vector_store_index_directory_ex =
        (void *)turbo_vector_store_index_directory_ex;
    void *vector_store_count = (void *)turbo_vector_store_count;
    void *vector_store_tool_binding_create =
        (void *)turbo_vector_store_tool_binding_create;
    void *vector_store_tool_binding_destroy =
        (void *)turbo_vector_store_tool_binding_destroy;
    void *vector_store_tool_graph = (void *)turbo_vector_store_tool_graph;
    void *vector_store_add_tools = (void *)turbo_vector_store_add_tools;
    void *vector_store_add_action_tools =
        (void *)turbo_vector_store_add_action_tools;
    void *vector_store_add_indexing_action_tools =
        (void *)turbo_vector_store_add_indexing_action_tools;
    void *text_splitter_split_text = (void *)turbo_text_splitter_split_text;
    void *document_loader_load_text_file =
        (void *)turbo_document_loader_load_text_file;
    void *agent_trace_json_value_sink = (void *)turbo_agent_add_trace_json_value_sink;
    void *agent_runtime_create = (void *)turbo_agent_runtime_create;
    void *agent_runtime_destroy = (void *)turbo_agent_runtime_destroy;
    void *agent_runtime_remote_create = (void *)turbo_agent_runtime_remote_create;
    void *agent_runtime_remote_destroy = (void *)turbo_agent_runtime_remote_destroy;
    void *agent_runtime_remote_dispatch = (void *)turbo_agent_runtime_remote_dispatch_jsonrpc;
    void *agent_runtime_remote_dispatch_text =
        (void *)turbo_agent_runtime_remote_dispatch_jsonrpc_text;
    void *agent_runtime_remote_handle_http =
        (void *)turbo_agent_runtime_remote_handle_http_jsonrpc;
    void *agent_runtime_remote_client_create =
        (void *)turbo_agent_runtime_remote_client_create;
    void *agent_runtime_remote_client_destroy =
        (void *)turbo_agent_runtime_remote_client_destroy;
    void *agent_runtime_remote_client_call =
        (void *)turbo_agent_runtime_remote_client_call_json;
    void *agent_runtime_remote_client_get_startup_diagnostics =
        (void *)turbo_agent_runtime_remote_client_get_startup_diagnostics;
    void *agent_runtime_remote_client_start =
        (void *)turbo_agent_runtime_remote_client_start_json_value_graph;
    void *agent_runtime_remote_client_resume =
        (void *)turbo_agent_runtime_remote_client_resume_json_value_graph;
    void *agent_runtime_remote_client_fork =
        (void *)turbo_agent_runtime_remote_client_fork_json_value_graph;
    void *agent_runtime_remote_client_get_thread_state =
        (void *)turbo_agent_runtime_remote_client_get_thread_state_json_value;
    void *agent_runtime_remote_client_get_checkpoint_context =
        (void *)turbo_agent_runtime_remote_client_get_checkpoint_context;
    void *agent_runtime_remote_client_get_run =
        (void *)turbo_agent_runtime_remote_client_get_run;
    void *agent_runtime_remote_client_get_checkpoint =
        (void *)turbo_agent_runtime_remote_client_get_checkpoint;
    void *agent_runtime_remote_client_list_checkpoints =
        (void *)turbo_agent_runtime_remote_client_list_checkpoints;
    void *agent_runtime_remote_client_load_history_events =
        (void *)turbo_agent_runtime_remote_client_load_history_events_json_value;
    void *agent_runtime_remote_client_get_run_trace_events =
        (void *)turbo_agent_runtime_remote_client_get_run_trace_events_json_value;
    void *agent_runtime_remote_client_get_checkpoint_trace_events =
        (void *)turbo_agent_runtime_remote_client_get_checkpoint_trace_events_json_value;
    void *agent_runtime_remote_client_get_memory_record =
        (void *)turbo_agent_runtime_remote_client_get_memory_record;
    void *agent_runtime_remote_client_delete_memory_record =
        (void *)turbo_agent_runtime_remote_client_delete_memory_record;
    void *agent_runtime_remote_client_put_memory_record =
        (void *)turbo_agent_runtime_remote_client_put_memory_record;
    void *agent_runtime_remote_client_validate_memory_record =
        (void *)turbo_agent_runtime_remote_client_validate_memory_record;
    void *agent_runtime_remote_client_query_memory_records_ex =
        (void *)turbo_agent_runtime_remote_client_query_memory_records_ex;
    void *agent_runtime_remote_client_query_memory_records =
        (void *)turbo_agent_runtime_remote_client_query_memory_records;
    void *agent_runtime_remote_client_list_memory_records =
        (void *)turbo_agent_runtime_remote_client_list_memory_records;
    void *agent_runtime_remote_client_get_thread_timeline =
        (void *)turbo_agent_runtime_remote_client_get_thread_timeline_json_value;
    void *agent_runtime_remote_client_get_branch_tree =
        (void *)turbo_agent_runtime_remote_client_get_branch_tree;
    void *agent_runtime_remote_client_get_observability_index =
        (void *)turbo_agent_runtime_remote_client_get_thread_observability_index;
    void *agent_runtime_remote_client_list_observability_indexes =
        (void *)turbo_agent_runtime_remote_client_list_observability_indexes;
    void *agent_runtime_remote_client_list_observability_indexes_filtered =
        (void *)turbo_agent_runtime_remote_client_list_observability_indexes_filtered;
    void *agent_runtime_remote_client_list_child_runs =
        (void *)turbo_agent_runtime_remote_client_list_child_runs;
    void *agent_runtime_remote_client_get_supervisor_inspect =
        (void *)turbo_agent_runtime_remote_client_get_supervisor_inspect;
    void *agent_runtime_remote_client_get_orchestration_inspect =
        (void *)turbo_agent_runtime_remote_client_get_orchestration_inspect;
    void *agent_runtime_remote_client_get_child_inspect =
        (void *)turbo_agent_runtime_remote_client_get_child_inspect;
    void *agent_runtime_remote_client_get_child_orchestration_inspect =
        (void *)turbo_agent_runtime_remote_client_get_child_orchestration_inspect;
    void *agent_runtime_remote_client_get_child_multi_agent_inspect =
        (void *)turbo_agent_runtime_remote_client_get_child_multi_agent_inspect;
    void *agent_runtime_remote_client_resume_thread_command =
        (void *)turbo_agent_runtime_remote_client_resume_thread_command_json_value;
    void *agent_runtime_remote_client_fork_thread_command =
        (void *)turbo_agent_runtime_remote_client_fork_thread_command_json_value;
    void *agent_remote_session_create = (void *)turbo_agent_remote_session_create;
    void *agent_remote_session_destroy = (void *)turbo_agent_remote_session_destroy;
    void *agent_remote_session_client = (void *)turbo_agent_remote_session_client;
    void *agent_remote_session_thread_id = (void *)turbo_agent_remote_session_thread_id;
    void *agent_remote_session_last_run_id = (void *)turbo_agent_remote_session_last_run_id;
    void *agent_remote_session_last_checkpoint_id =
        (void *)turbo_agent_remote_session_last_checkpoint_id;
    void *agent_remote_session_get_startup_diagnostics =
        (void *)turbo_agent_remote_session_get_startup_diagnostics;
    void *agent_remote_session_start = (void *)turbo_agent_remote_session_start_json_value_graph;
    void *agent_remote_session_start_text = (void *)turbo_agent_remote_session_start_text;
    void *agent_remote_session_start_messages =
        (void *)turbo_agent_remote_session_start_messages;
    void *agent_remote_session_resume = (void *)turbo_agent_remote_session_resume_json_value_graph;
    void *agent_remote_session_fork = (void *)turbo_agent_remote_session_fork_json_value_graph;
    void *agent_remote_session_invoke_text = (void *)turbo_agent_remote_session_invoke_text;
    void *agent_remote_session_invoke_messages_text =
        (void *)turbo_agent_remote_session_invoke_messages_text;
    void *agent_remote_session_invoke_json = (void *)turbo_agent_remote_session_invoke_json;
    void *agent_remote_session_invoke_messages_json =
        (void *)turbo_agent_remote_session_invoke_messages_json;
    void *agent_remote_session_get_thread = (void *)turbo_agent_remote_session_get_thread;
    void *agent_remote_session_get_latest_run =
        (void *)turbo_agent_remote_session_get_latest_run;
    void *agent_remote_session_get_pending_run =
        (void *)turbo_agent_remote_session_get_pending_run;
    void *agent_remote_session_get_thread_state =
        (void *)turbo_agent_remote_session_get_thread_state_json_value;
    void *agent_remote_session_memory_list_records =
        (void *)turbo_agent_remote_session_memory_list_records;
    void *agent_remote_session_memory_delete_record =
        (void *)turbo_agent_remote_session_memory_delete_record;
    void *agent_remote_session_memory_get_record =
        (void *)turbo_agent_remote_session_memory_get_record;
    void *agent_remote_session_memory_put_record =
        (void *)turbo_agent_remote_session_memory_put_record;
    void *agent_remote_session_memory_validate_record =
        (void *)turbo_agent_remote_session_memory_validate_record;
    void *agent_remote_session_memory_query_records =
        (void *)turbo_agent_remote_session_memory_query_records;
    void *agent_remote_session_memory_query_records_ex =
        (void *)turbo_agent_remote_session_memory_query_records_ex;
    void *agent_remote_session_get_checkpoint_context =
        (void *)turbo_agent_remote_session_get_checkpoint_context;
    void *agent_remote_session_get_observability_index =
        (void *)turbo_agent_remote_session_get_observability_index;
    void *agent_remote_session_get_thread_timeline =
        (void *)turbo_agent_remote_session_get_thread_timeline_json_value;
    void *agent_remote_session_load_thread_history_events =
        (void *)turbo_agent_remote_session_load_thread_history_events_json_value;
    void *agent_remote_session_replay_thread_history =
        (void *)turbo_agent_remote_session_replay_thread_history_json_value;
    void *agent_remote_session_observe_thread_history =
        (void *)turbo_agent_remote_session_observe_thread_history_json_value;
    void *agent_remote_session_get_thread_trace_events =
        (void *)turbo_agent_remote_session_get_thread_trace_events_json_value;
    void *agent_remote_session_get_branch_tree =
        (void *)turbo_agent_remote_session_get_branch_tree;
    void *agent_remote_session_list_thread_lineage =
        (void *)turbo_agent_remote_session_list_thread_lineage;
    void *agent_remote_session_get_supervisor_inbox =
        (void *)turbo_agent_remote_session_get_supervisor_inbox;
    void *agent_remote_session_get_supervisor_handoff_history =
        (void *)turbo_agent_remote_session_get_supervisor_handoff_history;
    void *agent_remote_session_get_supervisor_inspect =
        (void *)turbo_agent_remote_session_get_supervisor_inspect;
    void *agent_remote_session_list_child_runs =
        (void *)turbo_agent_remote_session_list_child_runs;
    void *agent_remote_session_get_orchestration_inspect =
        (void *)turbo_agent_remote_session_get_orchestration_inspect;
    void *agent_remote_session_get_child_run =
        (void *)turbo_agent_remote_session_get_child_run;
    void *agent_remote_session_get_child_checkpoint =
        (void *)turbo_agent_remote_session_get_child_checkpoint;
    void *agent_remote_session_get_child_checkpoint_context =
        (void *)turbo_agent_remote_session_get_child_checkpoint_context;
    void *agent_remote_session_get_child_thread_timeline =
        (void *)turbo_agent_remote_session_get_child_thread_timeline_json_value;
    void *agent_remote_session_get_child_branch_tree =
        (void *)turbo_agent_remote_session_get_child_branch_tree;
    void *agent_remote_session_list_child_checkpoints =
        (void *)turbo_agent_remote_session_list_child_checkpoints;
    void *agent_remote_session_load_child_history_events =
        (void *)turbo_agent_remote_session_load_child_history_events_json_value;
    void *agent_remote_session_get_child_trace_events =
        (void *)turbo_agent_remote_session_get_child_trace_events_json_value;
    void *agent_remote_session_get_child_inspect =
        (void *)turbo_agent_remote_session_get_child_inspect;
    void *agent_remote_session_get_child_orchestration_inspect =
        (void *)turbo_agent_remote_session_get_child_orchestration_inspect;
    void *agent_remote_session_get_child_multi_agent_inspect =
        (void *)turbo_agent_remote_session_get_child_multi_agent_inspect;
    void *agent_remote_session_resume_thread_command =
        (void *)turbo_agent_remote_session_resume_thread_command_json_value;
    void *agent_remote_session_fork_thread_command =
        (void *)turbo_agent_remote_session_fork_thread_command_json_value;
    void *agent_remote_app_create = (void *)turbo_agent_remote_app_create;
    void *agent_remote_app_destroy = (void *)turbo_agent_remote_app_destroy;
    void *agent_remote_app_session = (void *)turbo_agent_remote_app_session;
    void *agent_remote_app_graph_name = (void *)turbo_agent_remote_app_graph_name;
    void *agent_remote_app_thread_id = (void *)turbo_agent_remote_app_thread_id;
    void *agent_remote_app_last_run_id = (void *)turbo_agent_remote_app_last_run_id;
    void *agent_remote_app_last_checkpoint_id =
        (void *)turbo_agent_remote_app_last_checkpoint_id;
    void *agent_remote_app_get_startup_diagnostics =
        (void *)turbo_agent_remote_app_get_startup_diagnostics;
    void *agent_remote_app_start = (void *)turbo_agent_remote_app_start_json_value_graph;
    void *agent_remote_app_start_text = (void *)turbo_agent_remote_app_start_text;
    void *agent_remote_app_start_messages = (void *)turbo_agent_remote_app_start_messages;
    void *agent_remote_app_resume = (void *)turbo_agent_remote_app_resume_json_value_graph;
    void *agent_remote_app_fork = (void *)turbo_agent_remote_app_fork_json_value_graph;
    void *agent_remote_app_invoke_text = (void *)turbo_agent_remote_app_invoke_text;
    void *agent_remote_app_invoke_messages_text =
        (void *)turbo_agent_remote_app_invoke_messages_text;
    void *agent_remote_app_invoke_json = (void *)turbo_agent_remote_app_invoke_json;
    void *agent_remote_app_invoke_messages_json =
        (void *)turbo_agent_remote_app_invoke_messages_json;
    void *agent_remote_app_get_thread = (void *)turbo_agent_remote_app_get_thread;
    void *agent_remote_app_get_latest_run = (void *)turbo_agent_remote_app_get_latest_run;
    void *agent_remote_app_get_pending_run = (void *)turbo_agent_remote_app_get_pending_run;
    void *agent_remote_app_get_thread_state =
        (void *)turbo_agent_remote_app_get_thread_state_json_value;
    void *agent_remote_app_memory_list_records =
        (void *)turbo_agent_remote_app_memory_list_records;
    void *agent_remote_app_memory_delete_record =
        (void *)turbo_agent_remote_app_memory_delete_record;
    void *agent_remote_app_memory_get_record =
        (void *)turbo_agent_remote_app_memory_get_record;
    void *agent_remote_app_memory_put_record =
        (void *)turbo_agent_remote_app_memory_put_record;
    void *agent_remote_app_memory_validate_record =
        (void *)turbo_agent_remote_app_memory_validate_record;
    void *agent_remote_app_memory_query_records =
        (void *)turbo_agent_remote_app_memory_query_records;
    void *agent_remote_app_memory_query_records_ex =
        (void *)turbo_agent_remote_app_memory_query_records_ex;
    void *agent_remote_app_get_checkpoint_context =
        (void *)turbo_agent_remote_app_get_checkpoint_context;
    void *agent_remote_app_get_observability_index =
        (void *)turbo_agent_remote_app_get_observability_index;
    void *agent_remote_app_get_thread_timeline =
        (void *)turbo_agent_remote_app_get_thread_timeline_json_value;
    void *agent_remote_app_load_thread_history_events =
        (void *)turbo_agent_remote_app_load_thread_history_events_json_value;
    void *agent_remote_app_replay_thread_history =
        (void *)turbo_agent_remote_app_replay_thread_history_json_value;
    void *agent_remote_app_observe_thread_history =
        (void *)turbo_agent_remote_app_observe_thread_history_json_value;
    void *agent_remote_app_get_thread_trace_events =
        (void *)turbo_agent_remote_app_get_thread_trace_events_json_value;
    void *agent_remote_app_get_branch_tree =
        (void *)turbo_agent_remote_app_get_branch_tree;
    void *agent_remote_app_list_thread_lineage =
        (void *)turbo_agent_remote_app_list_thread_lineage;
    void *agent_remote_app_get_supervisor_inbox =
        (void *)turbo_agent_remote_app_get_supervisor_inbox;
    void *agent_remote_app_get_supervisor_handoff_history =
        (void *)turbo_agent_remote_app_get_supervisor_handoff_history;
    void *agent_remote_app_get_supervisor_inspect =
        (void *)turbo_agent_remote_app_get_supervisor_inspect;
    void *agent_remote_app_list_child_runs =
        (void *)turbo_agent_remote_app_list_child_runs;
    void *agent_remote_app_get_orchestration_inspect =
        (void *)turbo_agent_remote_app_get_orchestration_inspect;
    void *agent_remote_app_get_child_run = (void *)turbo_agent_remote_app_get_child_run;
    void *agent_remote_app_get_child_checkpoint =
        (void *)turbo_agent_remote_app_get_child_checkpoint;
    void *agent_remote_app_get_child_checkpoint_context =
        (void *)turbo_agent_remote_app_get_child_checkpoint_context;
    void *agent_remote_app_get_child_thread_timeline =
        (void *)turbo_agent_remote_app_get_child_thread_timeline_json_value;
    void *agent_remote_app_get_child_branch_tree =
        (void *)turbo_agent_remote_app_get_child_branch_tree;
    void *agent_remote_app_list_child_checkpoints =
        (void *)turbo_agent_remote_app_list_child_checkpoints;
    void *agent_remote_app_load_child_history_events =
        (void *)turbo_agent_remote_app_load_child_history_events_json_value;
    void *agent_remote_app_get_child_trace_events =
        (void *)turbo_agent_remote_app_get_child_trace_events_json_value;
    void *agent_remote_app_get_child_inspect =
        (void *)turbo_agent_remote_app_get_child_inspect;
    void *agent_remote_app_get_child_orchestration_inspect =
        (void *)turbo_agent_remote_app_get_child_orchestration_inspect;
    void *agent_remote_app_get_child_multi_agent_inspect =
        (void *)turbo_agent_remote_app_get_child_multi_agent_inspect;
    void *agent_remote_app_resume_thread_command =
        (void *)turbo_agent_remote_app_resume_thread_command_json_value;
    void *agent_remote_app_fork_thread_command =
        (void *)turbo_agent_remote_app_fork_thread_command_json_value;
    void *agent_runtime_remote_chttp_mount =
        (void *)turbo_agent_runtime_remote_chttp_mount;
#if 0
    void *agent_runtime_store_memory = (void *)turbo_agent_runtime_store_memory_create;
    void *agent_runtime_store_file = (void *)turbo_agent_runtime_store_file_create;
    void *agent_runtime_start = (void *)turbo_agent_runtime_start_json_value_graph;
    void *agent_runtime_start_linked = (void *)turbo_agent_runtime_start_json_value_graph_linked;
    void *agent_runtime_start_stream = (void *)turbo_agent_runtime_start_json_value_graph_stream;
    void *agent_runtime_resume = (void *)turbo_agent_runtime_resume_json_value_graph;
    void *agent_runtime_resume_stream = (void *)turbo_agent_runtime_resume_json_value_graph_stream;
    void *agent_runtime_fork = (void *)turbo_agent_runtime_fork_json_value_graph;
    void *agent_runtime_fork_stream = (void *)turbo_agent_runtime_fork_json_value_graph_stream;
    void *agent_runtime_resume_thread = (void *)turbo_agent_runtime_resume_thread_json_value_graph;
    void *agent_runtime_fork_thread = (void *)turbo_agent_runtime_fork_thread_json_value_graph;
    void *agent_runtime_resume_checkpoint =
        (void *)turbo_agent_runtime_resume_checkpoint_json_value_graph;
    void *agent_runtime_fork_checkpoint = (void *)turbo_agent_runtime_fork_checkpoint_json_value_graph;
    void *agent_runtime_get_thread = (void *)turbo_agent_runtime_get_thread;
    void *agent_runtime_get_run = (void *)turbo_agent_runtime_get_run;
    void *agent_runtime_get_latest_run = (void *)turbo_agent_runtime_get_latest_run;
    void *agent_runtime_get_pending_run = (void *)turbo_agent_runtime_get_pending_run;
    void *agent_runtime_get_thread_timeline =
        (void *)turbo_agent_runtime_get_thread_timeline_json_value;
    void *agent_runtime_get_observability_index =
        (void *)turbo_agent_runtime_get_thread_observability_index;
    void *agent_runtime_list_observability_indexes =
        (void *)turbo_agent_runtime_list_observability_indexes;
    void *agent_runtime_list_observability_indexes_filtered =
        (void *)turbo_agent_runtime_list_observability_indexes_filtered;
    void *agent_runtime_get_branch_tree = (void *)turbo_agent_runtime_get_branch_tree;
    void *agent_runtime_get_checkpoint = (void *)turbo_agent_runtime_get_checkpoint;
    void *agent_runtime_get_latest_checkpoint = (void *)turbo_agent_runtime_get_latest_checkpoint;
    void *agent_runtime_get_checkpoint_context =
        (void *)turbo_agent_runtime_get_checkpoint_context;
    void *agent_runtime_get_thread_state = (void *)turbo_agent_runtime_get_thread_state_json_value;
    void *agent_runtime_get_thread_head_state =
        (void *)turbo_agent_runtime_get_thread_head_state_json_value;
    void *agent_runtime_get_thread_trace_events =
        (void *)turbo_agent_runtime_get_thread_trace_events_json_value;
    void *agent_runtime_get_thread_head_trace_events =
        (void *)turbo_agent_runtime_get_thread_head_trace_events_json_value;
    void *agent_runtime_get_run_state = (void *)turbo_agent_runtime_get_run_state_json_value;
    void *agent_runtime_get_run_trace_events =
        (void *)turbo_agent_runtime_get_run_trace_events_json_value;
    void *agent_runtime_get_checkpoint_state =
        (void *)turbo_agent_runtime_get_checkpoint_state_json_value;
    void *agent_runtime_prepare_checkpoint_state_override =
        (void *)turbo_agent_runtime_prepare_checkpoint_state_override_json_value;
    void *agent_runtime_update_checkpoint_state =
        (void *)turbo_agent_runtime_update_checkpoint_state_json_value;
    void *agent_runtime_prepare_thread_state_override =
        (void *)turbo_agent_runtime_prepare_thread_state_override_json_value;
    void *agent_runtime_update_thread_state =
        (void *)turbo_agent_runtime_update_thread_state_json_value;
    void *agent_runtime_get_checkpoint_trace_events =
        (void *)turbo_agent_runtime_get_checkpoint_trace_events_json_value;
    void *agent_runtime_list_runs = (void *)turbo_agent_runtime_list_runs;
    void *agent_runtime_list_thread_lineage = (void *)turbo_agent_runtime_list_thread_lineage;
    void *agent_runtime_list_child_runs = (void *)turbo_agent_runtime_list_child_runs;
    void *agent_runtime_list_checkpoints = (void *)turbo_agent_runtime_list_checkpoints;
    void *agent_runtime_history = (void *)turbo_agent_runtime_load_history_events_json_value;
    void *agent_runtime_thread_history = (void *)turbo_agent_runtime_load_thread_history_events_json_value;
    void *agent_runtime_replay_history = (void *)turbo_agent_runtime_replay_history_json_value;
    void *agent_runtime_replay_thread_history =
        (void *)turbo_agent_runtime_replay_thread_history_json_value;
    void *agent_runtime_observe_history = (void *)turbo_agent_runtime_observe_history_json_value;
    void *agent_runtime_observe_thread_history =
        (void *)turbo_agent_runtime_observe_thread_history_json_value;
    void *agent_runtime_prepare_checkpoint_command_override =
        (void *)turbo_agent_runtime_prepare_checkpoint_command_override_json_value;
    void *agent_runtime_apply_command = (void *)turbo_agent_runtime_apply_command_json_value;
    void *agent_runtime_apply_checkpoint_command =
        (void *)turbo_agent_runtime_apply_checkpoint_command_json_value;
    void *agent_runtime_prepare_thread_command_override =
        (void *)turbo_agent_runtime_prepare_thread_command_override_json_value;
    void *agent_runtime_apply_thread_command =
        (void *)turbo_agent_runtime_apply_thread_command_json_value;
    void *agent_runtime_resume_command = (void *)turbo_agent_runtime_resume_command_json_value;
    void *agent_runtime_resume_checkpoint_command =
        (void *)turbo_agent_runtime_resume_checkpoint_command_json_value;
    void *agent_runtime_resume_thread_command =
        (void *)turbo_agent_runtime_resume_thread_command_json_value;
    void *agent_runtime_fork_command = (void *)turbo_agent_runtime_fork_command_json_value;
    void *agent_runtime_fork_checkpoint_command =
        (void *)turbo_agent_runtime_fork_checkpoint_command_json_value;
    void *agent_runtime_fork_thread_command =
        (void *)turbo_agent_runtime_fork_thread_command_json_value;
    void *agent_runtime_resume_checkpoint_state =
        (void *)turbo_agent_runtime_resume_checkpoint_state_json_value_graph;
    void *agent_runtime_resume_thread_state =
        (void *)turbo_agent_runtime_resume_thread_state_json_value_graph;
    void *agent_runtime_fork_checkpoint_state =
        (void *)turbo_agent_runtime_fork_checkpoint_state_json_value_graph;
    void *agent_runtime_fork_thread_state =
        (void *)turbo_agent_runtime_fork_thread_state_json_value_graph;
    void *agent_memory_store_memory = (void *)turbo_agent_memory_store_memory_create;
    void *agent_memory_store_file = (void *)turbo_agent_memory_store_file_create;
    void *agent_memory_store_destroy = (void *)turbo_agent_memory_store_destroy;
    void *agent_memory_get = (void *)turbo_agent_memory_get;
    void *agent_memory_put = (void *)turbo_agent_memory_put;
    void *agent_memory_delete = (void *)turbo_agent_memory_delete;
    void *agent_memory_list = (void *)turbo_agent_memory_list;
    void *agent_memory_list_records = (void *)turbo_agent_memory_list_records;
    void *agent_memory_get_record = (void *)turbo_agent_memory_get_record;
    void *agent_memory_put_record = (void *)turbo_agent_memory_put_record;
    void *agent_memory_validate_record = (void *)turbo_agent_memory_validate_record;
    void *agent_memory_query_records = (void *)turbo_agent_memory_query_records;
    void *agent_memory_query_records_ex = (void *)turbo_agent_memory_query_records_ex;
    void *agent_tool_registry = (void *)turbo_agent_tool_registry;
    void *agent_tool_count = (void *)turbo_agent_tool_count;
    void *agent_app_create = (void *)turbo_agent_app_create;
    void *agent_app_destroy = (void *)turbo_agent_app_destroy;
    void *agent_app_session = (void *)turbo_agent_app_session;
    void *agent_app_thread_id = (void *)turbo_agent_app_thread_id;
    void *agent_app_last_run_id = (void *)turbo_agent_app_last_run_id;
    void *agent_app_last_checkpoint_id = (void *)turbo_agent_app_last_checkpoint_id;
    void *agent_app_workflow_kind = (void *)turbo_agent_app_workflow_kind;
    void *agent_app_memory_namespace = (void *)turbo_agent_app_memory_namespace;
    void *agent_app_memory_store = (void *)turbo_agent_app_memory_store;
    void *agent_app_tool_registry = (void *)turbo_agent_app_tool_registry;
    void *agent_app_tool_count = (void *)turbo_agent_app_tool_count;
    void *agent_app_get_capabilities = (void *)turbo_agent_app_get_capabilities;
    void *agent_app_get_tool_schemas = (void *)turbo_agent_app_get_tool_schemas;
    void *agent_app_get_startup_diagnostics =
        (void *)turbo_agent_app_get_startup_diagnostics;
    void *agent_app_add_trace_json_value_sink = (void *)turbo_agent_app_add_trace_json_value_sink;
    void *agent_app_add_observer_json_value_sink = (void *)turbo_agent_app_add_observer_json_value_sink;
    void *agent_app_set_trace_history_enabled = (void *)turbo_agent_app_set_trace_history_enabled;
    void *agent_app_get_thread = (void *)turbo_agent_app_get_thread;
    void *agent_app_get_run = (void *)turbo_agent_app_get_run;
    void *agent_app_get_latest_run = (void *)turbo_agent_app_get_latest_run;
    void *agent_app_get_pending_run = (void *)turbo_agent_app_get_pending_run;
    void *agent_app_get_thread_timeline = (void *)turbo_agent_app_get_thread_timeline_json_value;
    void *agent_app_get_observability_index =
        (void *)turbo_agent_app_get_observability_index;
    void *agent_app_get_branch_tree = (void *)turbo_agent_app_get_branch_tree;
    void *agent_app_get_checkpoint = (void *)turbo_agent_app_get_checkpoint;
    void *agent_app_get_latest_checkpoint = (void *)turbo_agent_app_get_latest_checkpoint;
    void *agent_app_get_checkpoint_context = (void *)turbo_agent_app_get_checkpoint_context;
    void *agent_app_get_thread_state = (void *)turbo_agent_app_get_thread_state_json_value;
    void *agent_app_get_thread_head_state =
        (void *)turbo_agent_app_get_thread_head_state_json_value;
    void *agent_app_get_thread_trace_events =
        (void *)turbo_agent_app_get_thread_trace_events_json_value;
    void *agent_app_get_thread_head_trace_events =
        (void *)turbo_agent_app_get_thread_head_trace_events_json_value;
    void *agent_app_get_run_state = (void *)turbo_agent_app_get_run_state_json_value;
    void *agent_app_get_run_trace_events =
        (void *)turbo_agent_app_get_run_trace_events_json_value;
    void *agent_app_get_checkpoint_state = (void *)turbo_agent_app_get_checkpoint_state_json_value;
    void *agent_app_prepare_checkpoint_state_override =
        (void *)turbo_agent_app_prepare_checkpoint_state_override_json_value;
    void *agent_app_update_checkpoint_state =
        (void *)turbo_agent_app_update_checkpoint_state_json_value;
    void *agent_app_prepare_thread_state_override =
        (void *)turbo_agent_app_prepare_thread_state_override_json_value;
    void *agent_app_update_thread_state = (void *)turbo_agent_app_update_thread_state_json_value;
    void *agent_app_get_supervisor_inbox = (void *)turbo_agent_app_get_supervisor_inbox;
    void *agent_app_get_supervisor_handoff_history =
        (void *)turbo_agent_app_get_supervisor_handoff_history;
    void *agent_app_get_supervisor_inspect = (void *)turbo_agent_app_get_supervisor_inspect;
    void *agent_app_get_orchestration_inspect =
        (void *)turbo_agent_app_get_orchestration_inspect;
    void *agent_app_append_supervisor_inbox_message =
        (void *)turbo_agent_app_append_supervisor_inbox_message_json_value;
    void *agent_app_get_checkpoint_trace_events =
        (void *)turbo_agent_app_get_checkpoint_trace_events_json_value;
    void *agent_app_get_child_run = (void *)turbo_agent_app_get_child_run;
    void *agent_app_get_child_checkpoint = (void *)turbo_agent_app_get_child_checkpoint;
    void *agent_app_get_child_checkpoint_context =
        (void *)turbo_agent_app_get_child_checkpoint_context;
    void *agent_app_get_child_thread_timeline =
        (void *)turbo_agent_app_get_child_thread_timeline_json_value;
    void *agent_app_get_child_branch_tree =
        (void *)turbo_agent_app_get_child_branch_tree;
    void *agent_app_get_child_inspect = (void *)turbo_agent_app_get_child_inspect;
    void *agent_app_get_child_orchestration_inspect =
        (void *)turbo_agent_app_get_child_orchestration_inspect;
    void *agent_app_get_child_multi_agent_inspect =
        (void *)turbo_agent_app_get_child_multi_agent_inspect;
    void *agent_app_list_runs = (void *)turbo_agent_app_list_runs;
    void *agent_app_list_thread_lineage = (void *)turbo_agent_app_list_thread_lineage;
    void *agent_app_list_child_runs = (void *)turbo_agent_app_list_child_runs;
    void *agent_app_list_child_checkpoints = (void *)turbo_agent_app_list_child_checkpoints;
    void *agent_app_list_checkpoints = (void *)turbo_agent_app_list_checkpoints;
    void *agent_app_history = (void *)turbo_agent_app_load_history_events_json_value;
    void *agent_app_thread_history = (void *)turbo_agent_app_load_thread_history_events_json_value;
    void *agent_app_replay_history = (void *)turbo_agent_app_replay_history_json_value;
    void *agent_app_replay_thread_history =
        (void *)turbo_agent_app_replay_thread_history_json_value;
    void *agent_app_observe_history = (void *)turbo_agent_app_observe_history_json_value;
    void *agent_app_observe_thread_history =
        (void *)turbo_agent_app_observe_thread_history_json_value;
    void *agent_app_start_stream = (void *)turbo_agent_app_start_json_value_graph_stream;
    void *agent_app_resume_stream = (void *)turbo_agent_app_resume_json_value_graph_stream;
    void *agent_app_fork_stream = (void *)turbo_agent_app_fork_json_value_graph_stream;
    void *agent_app_child_history = (void *)turbo_agent_app_load_child_history_events_json_value;
    void *agent_app_child_trace_events = (void *)turbo_agent_app_get_child_trace_events_json_value;
    void *agent_app_prepare_checkpoint_command_override =
        (void *)turbo_agent_app_prepare_checkpoint_command_override_json_value;
    void *agent_app_apply_command = (void *)turbo_agent_app_apply_command_json_value;
    void *agent_app_apply_checkpoint_command =
        (void *)turbo_agent_app_apply_checkpoint_command_json_value;
    void *agent_app_prepare_thread_command_override =
        (void *)turbo_agent_app_prepare_thread_command_override_json_value;
    void *agent_app_apply_thread_command = (void *)turbo_agent_app_apply_thread_command_json_value;
    void *agent_app_resume_command = (void *)turbo_agent_app_resume_command_json_value;
    void *agent_app_resume_checkpoint_command =
        (void *)turbo_agent_app_resume_checkpoint_command_json_value;
    void *agent_app_resume_thread_command = (void *)turbo_agent_app_resume_thread_command_json_value;
    void *agent_app_fork_command = (void *)turbo_agent_app_fork_command_json_value;
    void *agent_app_fork_checkpoint_command =
        (void *)turbo_agent_app_fork_checkpoint_command_json_value;
    void *agent_app_fork_thread_command = (void *)turbo_agent_app_fork_thread_command_json_value;
    void *agent_app_resume_thread = (void *)turbo_agent_app_resume_thread_json_value_graph;
    void *agent_app_fork_thread = (void *)turbo_agent_app_fork_thread_json_value_graph;
    void *agent_app_resume_checkpoint = (void *)turbo_agent_app_resume_checkpoint_json_value_graph;
    void *agent_app_fork_checkpoint = (void *)turbo_agent_app_fork_checkpoint_json_value_graph;
    void *agent_app_resume_checkpoint_state =
        (void *)turbo_agent_app_resume_checkpoint_state_json_value_graph;
    void *agent_app_resume_thread_state =
        (void *)turbo_agent_app_resume_thread_state_json_value_graph;
    void *agent_app_fork_checkpoint_state =
        (void *)turbo_agent_app_fork_checkpoint_state_json_value_graph;
    void *agent_app_fork_thread_state =
        (void *)turbo_agent_app_fork_thread_state_json_value_graph;
    void *agent_app_resume_preset = (void *)turbo_agent_app_resume_preset_json_value_graph;
    void *agent_app_fork_preset = (void *)turbo_agent_app_fork_preset_json_value_graph;
    void *agent_app_resume_preset_command = (void *)turbo_agent_app_resume_preset_command_json_value;
    void *agent_app_resume_thread_preset_command =
        (void *)turbo_agent_app_resume_thread_preset_command_json_value;
    void *agent_app_fork_preset_command = (void *)turbo_agent_app_fork_preset_command_json_value;
    void *agent_app_fork_thread_preset_command =
        (void *)turbo_agent_app_fork_thread_preset_command_json_value;
    void *agent_app_start_text = (void *)turbo_agent_app_start_text;
    void *agent_app_start_text_stream = (void *)turbo_agent_app_start_text_stream;
    void *agent_app_start_messages = (void *)turbo_agent_app_start_messages;
    void *agent_app_start_messages_stream =
        (void *)turbo_agent_app_start_messages_stream;
    void *agent_app_invoke_text = (void *)turbo_agent_app_invoke_text;
    void *agent_app_invoke_messages_text = (void *)turbo_agent_app_invoke_messages_text;
    void *agent_app_invoke_json = (void *)turbo_agent_app_invoke_json;
    void *agent_app_invoke_messages_json = (void *)turbo_agent_app_invoke_messages_json;
    void *agent_app_batch_text = (void *)turbo_agent_app_batch_text;
    void *agent_app_memory_get = (void *)turbo_agent_app_memory_get;
    void *agent_app_memory_put = (void *)turbo_agent_app_memory_put;
    void *agent_app_memory_put_context = (void *)turbo_agent_app_memory_put_context;
    void *agent_app_memory_delete = (void *)turbo_agent_app_memory_delete;
    void *agent_app_memory_list = (void *)turbo_agent_app_memory_list;
    void *agent_app_memory_list_records = (void *)turbo_agent_app_memory_list_records;
    void *agent_app_memory_get_record = (void *)turbo_agent_app_memory_get_record;
    void *agent_app_memory_put_record = (void *)turbo_agent_app_memory_put_record;
    void *agent_app_memory_validate_record = (void *)turbo_agent_app_memory_validate_record;
    void *agent_app_memory_query_records = (void *)turbo_agent_app_memory_query_records;
    void *agent_app_memory_query_records_ex = (void *)turbo_agent_app_memory_query_records_ex;
    void *agent_session_create = (void *)turbo_agent_session_create;
    void *agent_session_destroy = (void *)turbo_agent_session_destroy;
    void *agent_session_agent = (void *)turbo_agent_session_agent;
    void *agent_session_runtime = (void *)turbo_agent_session_runtime;
    void *agent_session_memory_store = (void *)turbo_agent_session_memory_store;
    void *agent_session_model = (void *)turbo_agent_session_model;
    void *agent_session_base_url = (void *)turbo_agent_session_base_url;
    void *agent_session_provider_name = (void *)turbo_agent_session_provider_name;
    void *agent_session_has_api_key = (void *)turbo_agent_session_has_api_key;
    void *agent_session_tool_registry = (void *)turbo_agent_session_tool_registry;
    void *agent_session_tool_count = (void *)turbo_agent_session_tool_count;
    void *agent_session_get_capabilities =
        (void *)turbo_agent_session_get_capabilities;
    void *agent_session_get_tool_schemas =
        (void *)turbo_agent_session_get_tool_schemas;
    void *agent_session_get_startup_diagnostics =
        (void *)turbo_agent_session_get_startup_diagnostics;
    void *agent_session_workflow_kind = (void *)turbo_agent_session_workflow_kind;
    void *agent_session_memory_namespace = (void *)turbo_agent_session_memory_namespace;
    void *agent_session_add_trace_json_value_sink =
        (void *)turbo_agent_session_add_trace_json_value_sink;
    void *agent_session_add_observer_json_value_sink =
        (void *)turbo_agent_session_add_observer_json_value_sink;
    void *agent_session_set_trace_history_enabled =
        (void *)turbo_agent_session_set_trace_history_enabled;
    void *agent_session_get_thread = (void *)turbo_agent_session_get_thread;
    void *agent_session_get_run = (void *)turbo_agent_session_get_run;
    void *agent_session_get_latest_run = (void *)turbo_agent_session_get_latest_run;
    void *agent_session_get_pending_run = (void *)turbo_agent_session_get_pending_run;
    void *agent_session_get_thread_timeline =
        (void *)turbo_agent_session_get_thread_timeline_json_value;
    void *agent_session_get_observability_index =
        (void *)turbo_agent_session_get_observability_index;
    void *agent_session_get_branch_tree = (void *)turbo_agent_session_get_branch_tree;
    void *agent_session_get_checkpoint = (void *)turbo_agent_session_get_checkpoint;
    void *agent_session_get_latest_checkpoint =
        (void *)turbo_agent_session_get_latest_checkpoint;
    void *agent_session_get_checkpoint_context =
        (void *)turbo_agent_session_get_checkpoint_context;
    void *agent_session_get_thread_state = (void *)turbo_agent_session_get_thread_state_json_value;
    void *agent_session_get_thread_head_state =
        (void *)turbo_agent_session_get_thread_head_state_json_value;
    void *agent_session_get_thread_trace_events =
        (void *)turbo_agent_session_get_thread_trace_events_json_value;
    void *agent_session_get_thread_head_trace_events =
        (void *)turbo_agent_session_get_thread_head_trace_events_json_value;
    void *agent_session_get_run_state = (void *)turbo_agent_session_get_run_state_json_value;
    void *agent_session_get_run_trace_events =
        (void *)turbo_agent_session_get_run_trace_events_json_value;
    void *agent_session_get_checkpoint_state =
        (void *)turbo_agent_session_get_checkpoint_state_json_value;
    void *agent_session_prepare_checkpoint_state_override =
        (void *)turbo_agent_session_prepare_checkpoint_state_override_json_value;
    void *agent_session_update_checkpoint_state =
        (void *)turbo_agent_session_update_checkpoint_state_json_value;
    void *agent_session_prepare_thread_state_override =
        (void *)turbo_agent_session_prepare_thread_state_override_json_value;
    void *agent_session_update_thread_state =
        (void *)turbo_agent_session_update_thread_state_json_value;
    void *agent_session_get_supervisor_inbox =
        (void *)turbo_agent_session_get_supervisor_inbox;
    void *agent_session_get_supervisor_handoff_history =
        (void *)turbo_agent_session_get_supervisor_handoff_history;
    void *agent_session_get_supervisor_inspect =
        (void *)turbo_agent_session_get_supervisor_inspect;
    void *agent_session_get_orchestration_inspect =
        (void *)turbo_agent_session_get_orchestration_inspect;
    void *agent_session_append_supervisor_inbox_message =
        (void *)turbo_agent_session_append_supervisor_inbox_message_json_value;
    void *agent_session_get_checkpoint_trace_events =
        (void *)turbo_agent_session_get_checkpoint_trace_events_json_value;
    void *agent_session_get_child_run = (void *)turbo_agent_session_get_child_run;
    void *agent_session_get_child_checkpoint =
        (void *)turbo_agent_session_get_child_checkpoint;
    void *agent_session_get_child_checkpoint_context =
        (void *)turbo_agent_session_get_child_checkpoint_context;
    void *agent_session_get_child_thread_timeline =
        (void *)turbo_agent_session_get_child_thread_timeline_json_value;
    void *agent_session_get_child_branch_tree =
        (void *)turbo_agent_session_get_child_branch_tree;
    void *agent_session_get_child_inspect = (void *)turbo_agent_session_get_child_inspect;
    void *agent_session_get_child_orchestration_inspect =
        (void *)turbo_agent_session_get_child_orchestration_inspect;
    void *agent_session_get_child_multi_agent_inspect =
        (void *)turbo_agent_session_get_child_multi_agent_inspect;
    void *agent_session_list_runs = (void *)turbo_agent_session_list_runs;
    void *agent_session_list_thread_lineage = (void *)turbo_agent_session_list_thread_lineage;
    void *agent_session_list_child_runs = (void *)turbo_agent_session_list_child_runs;
    void *agent_session_list_child_checkpoints =
        (void *)turbo_agent_session_list_child_checkpoints;
    void *agent_session_list_checkpoints = (void *)turbo_agent_session_list_checkpoints;
    void *agent_session_history = (void *)turbo_agent_session_load_history_events_json_value;
    void *agent_session_thread_history =
        (void *)turbo_agent_session_load_thread_history_events_json_value;
    void *agent_session_replay_history = (void *)turbo_agent_session_replay_history_json_value;
    void *agent_session_replay_thread_history =
        (void *)turbo_agent_session_replay_thread_history_json_value;
    void *agent_session_observe_history =
        (void *)turbo_agent_session_observe_history_json_value;
    void *agent_session_observe_thread_history =
        (void *)turbo_agent_session_observe_thread_history_json_value;
    void *agent_session_start_stream = (void *)turbo_agent_session_start_json_value_graph_stream;
    void *agent_session_resume_stream = (void *)turbo_agent_session_resume_json_value_graph_stream;
    void *agent_session_fork_stream = (void *)turbo_agent_session_fork_json_value_graph_stream;
    void *agent_session_child_history =
        (void *)turbo_agent_session_load_child_history_events_json_value;
    void *agent_session_child_trace_events =
        (void *)turbo_agent_session_get_child_trace_events_json_value;
    void *agent_session_prepare_checkpoint_command_override =
        (void *)turbo_agent_session_prepare_checkpoint_command_override_json_value;
    void *agent_session_apply_command = (void *)turbo_agent_session_apply_command_json_value;
    void *agent_session_apply_checkpoint_command =
        (void *)turbo_agent_session_apply_checkpoint_command_json_value;
    void *agent_session_prepare_thread_command_override =
        (void *)turbo_agent_session_prepare_thread_command_override_json_value;
    void *agent_session_apply_thread_command =
        (void *)turbo_agent_session_apply_thread_command_json_value;
    void *agent_session_resume_command = (void *)turbo_agent_session_resume_command_json_value;
    void *agent_session_resume_checkpoint_command =
        (void *)turbo_agent_session_resume_checkpoint_command_json_value;
    void *agent_session_resume_thread_command =
        (void *)turbo_agent_session_resume_thread_command_json_value;
    void *agent_session_fork_command = (void *)turbo_agent_session_fork_command_json_value;
    void *agent_session_fork_checkpoint_command =
        (void *)turbo_agent_session_fork_checkpoint_command_json_value;
    void *agent_session_fork_thread_command =
        (void *)turbo_agent_session_fork_thread_command_json_value;
    void *agent_session_resume_preset_command =
        (void *)turbo_agent_session_resume_preset_command_json_value;
    void *agent_session_resume_thread_preset_command =
        (void *)turbo_agent_session_resume_thread_preset_command_json_value;
    void *agent_session_fork_preset_command =
        (void *)turbo_agent_session_fork_preset_command_json_value;
    void *agent_session_fork_thread_preset_command =
        (void *)turbo_agent_session_fork_thread_preset_command_json_value;
    void *agent_session_start = (void *)turbo_agent_session_start_json_value_graph;
    void *agent_session_resume = (void *)turbo_agent_session_resume_json_value_graph;
    void *agent_session_fork = (void *)turbo_agent_session_fork_json_value_graph;
    void *agent_session_resume_thread = (void *)turbo_agent_session_resume_thread_json_value_graph;
    void *agent_session_fork_thread = (void *)turbo_agent_session_fork_thread_json_value_graph;
    void *agent_session_resume_checkpoint =
        (void *)turbo_agent_session_resume_checkpoint_json_value_graph;
    void *agent_session_fork_checkpoint = (void *)turbo_agent_session_fork_checkpoint_json_value_graph;
    void *agent_session_resume_checkpoint_state =
        (void *)turbo_agent_session_resume_checkpoint_state_json_value_graph;
    void *agent_session_resume_thread_state =
        (void *)turbo_agent_session_resume_thread_state_json_value_graph;
    void *agent_session_fork_checkpoint_state =
        (void *)turbo_agent_session_fork_checkpoint_state_json_value_graph;
    void *agent_session_fork_thread_state =
        (void *)turbo_agent_session_fork_thread_state_json_value_graph;
    void *agent_session_loop_graph = (void *)turbo_agent_session_create_loop_graph;
    void *agent_session_review_graph = (void *)turbo_agent_session_create_review_graph;
    void *agent_session_engineering_graph = (void *)turbo_agent_session_create_engineering_graph;
    void *agent_session_knowledge_engineering_graph =
        (void *)turbo_agent_session_create_knowledge_engineering_graph;
    void *agent_session_retriever_engineering_graph =
        (void *)turbo_agent_session_create_retriever_engineering_graph;
    void *agent_session_preset_graph = (void *)turbo_agent_session_create_preset_graph;
    void *agent_session_input_state = (void *)turbo_agent_session_create_input_state_json_value;
    void *agent_session_input_messages_state =
        (void *)turbo_agent_session_create_input_messages_state_json_value;
    void *agent_session_start_preset = (void *)turbo_agent_session_start_preset_json_value_graph;
    void *agent_session_start_preset_text = (void *)turbo_agent_session_start_preset_text;
    void *agent_session_start_preset_text_with_memory =
        (void *)turbo_agent_session_start_preset_text_with_memory;
    void *agent_session_start_preset_messages =
        (void *)turbo_agent_session_start_preset_messages;
    void *agent_session_start_text = (void *)turbo_agent_session_start_text;
    void *agent_session_start_text_stream =
        (void *)turbo_agent_session_start_text_stream;
    void *agent_session_start_messages = (void *)turbo_agent_session_start_messages;
    void *agent_session_start_messages_stream =
        (void *)turbo_agent_session_start_messages_stream;
    void *agent_session_result_text = (void *)turbo_agent_session_result_text;
    void *agent_session_invoke_preset_text = (void *)turbo_agent_session_invoke_preset_text;
    void *agent_session_invoke_text = (void *)turbo_agent_session_invoke_text;
    void *agent_session_invoke_preset_text_with_memory =
        (void *)turbo_agent_session_invoke_preset_text_with_memory;
    void *agent_session_invoke_preset_messages_text =
        (void *)turbo_agent_session_invoke_preset_messages_text;
    void *agent_session_invoke_messages_text =
        (void *)turbo_agent_session_invoke_messages_text;
    void *agent_session_invoke_preset_json = (void *)turbo_agent_session_invoke_preset_json;
    void *agent_session_invoke_json = (void *)turbo_agent_session_invoke_json;
    void *agent_session_invoke_preset_json_with_memory =
        (void *)turbo_agent_session_invoke_preset_json_with_memory;
    void *agent_session_invoke_preset_messages_json =
        (void *)turbo_agent_session_invoke_preset_messages_json;
    void *agent_session_invoke_messages_json =
        (void *)turbo_agent_session_invoke_messages_json;
    void *agent_session_batch_text = (void *)turbo_agent_session_batch_text;
    void *agent_session_memory_get = (void *)turbo_agent_session_memory_get;
    void *agent_session_memory_put = (void *)turbo_agent_session_memory_put;
    void *agent_session_memory_put_context = (void *)turbo_agent_session_memory_put_context;
    void *agent_session_memory_delete = (void *)turbo_agent_session_memory_delete;
    void *agent_session_memory_list = (void *)turbo_agent_session_memory_list;
    void *agent_session_memory_list_records = (void *)turbo_agent_session_memory_list_records;
    void *agent_session_memory_get_record = (void *)turbo_agent_session_memory_get_record;
    void *agent_session_memory_put_record = (void *)turbo_agent_session_memory_put_record;
    void *agent_session_memory_validate_record =
        (void *)turbo_agent_session_memory_validate_record;
    void *agent_session_memory_query_records = (void *)turbo_agent_session_memory_query_records;
    void *agent_session_memory_query_records_ex =
        (void *)turbo_agent_session_memory_query_records_ex;
    void *agent_session_load_memory_context = (void *)turbo_agent_session_load_memory_context;
    void *agent_session_input_state_with_memory =
        (void *)turbo_agent_session_create_input_state_with_memory_json_value;
    void *agent_session_input_messages_state_with_memory =
        (void *)turbo_agent_session_create_input_messages_state_with_memory_json_value;
    void *agent_session_resume_preset = (void *)turbo_agent_session_resume_preset_json_value_graph;
    void *agent_session_fork_preset = (void *)turbo_agent_session_fork_preset_json_value_graph;
#endif
    void *agent_runtime_store_memory = (void *)turbo_agent_runtime_store_memory_create;
    void *agent_runtime_store_file = (void *)turbo_agent_runtime_store_file_create;
    void *agent_runtime_exec_start = (void *)turbo_agent_runtime_exec_start;
    void *agent_runtime_exec_resume = (void *)turbo_agent_runtime_exec_resume;
    void *agent_runtime_exec_fork = (void *)turbo_agent_runtime_exec_fork;
    void *agent_runtime_exec_start_controlled =
        (void *)turbo_agent_runtime_exec_start_controlled;
    void *agent_runtime_exec_resume_controlled =
        (void *)turbo_agent_runtime_exec_resume_controlled;
    void *agent_runtime_exec_fork_controlled =
        (void *)turbo_agent_runtime_exec_fork_controlled;
    void *agent_execution_start = (void *)turbo_agent_execution_start;
    void *agent_execution_resume = (void *)turbo_agent_execution_resume;
    void *agent_execution_fork = (void *)turbo_agent_execution_fork;
    void *agent_execution_retain = (void *)turbo_agent_execution_retain;
    void *agent_execution_release = (void *)turbo_agent_execution_release;
    void *agent_execution_id = (void *)turbo_agent_execution_id;
    void *agent_execution_cancel = (void *)turbo_agent_execution_cancel;
    void *agent_execution_wait = (void *)turbo_agent_execution_wait;
    void *agent_execution_get_status = (void *)turbo_agent_execution_get_status;
    void *agent_execution_result_code =
        (void *)turbo_agent_execution_result_code;
    void *agent_execution_take_result =
        (void *)turbo_agent_execution_take_result;
    void *agent_runtime_get_thread = (void *)turbo_agent_runtime_get_thread;
    void *agent_runtime_get_run = (void *)turbo_agent_runtime_get_run;
    void *agent_runtime_get_latest_run = (void *)turbo_agent_runtime_get_latest_run;
    void *agent_runtime_get_pending_run = (void *)turbo_agent_runtime_get_pending_run;
    void *agent_runtime_get_thread_timeline =
        (void *)turbo_agent_runtime_get_thread_timeline_json_value;
    void *agent_runtime_get_observability_index =
        (void *)turbo_agent_runtime_get_thread_observability_index;
    void *agent_runtime_list_observability_indexes =
        (void *)turbo_agent_runtime_list_observability_indexes;
    void *agent_runtime_list_observability_indexes_filtered =
        (void *)turbo_agent_runtime_list_observability_indexes_filtered;
    void *agent_runtime_get_branch_tree = (void *)turbo_agent_runtime_get_branch_tree;
    void *agent_runtime_get_checkpoint = (void *)turbo_agent_runtime_get_checkpoint;
    void *agent_runtime_get_latest_checkpoint = (void *)turbo_agent_runtime_get_latest_checkpoint;
    void *agent_runtime_get_checkpoint_context =
        (void *)turbo_agent_runtime_get_checkpoint_context;
    void *agent_runtime_get_thread_state = (void *)turbo_agent_runtime_get_thread_state_json_value;
    void *agent_runtime_get_thread_head_state =
        (void *)turbo_agent_runtime_get_thread_head_state_json_value;
    void *agent_runtime_get_thread_trace_events =
        (void *)turbo_agent_runtime_get_thread_trace_events_json_value;
    void *agent_runtime_get_thread_head_trace_events =
        (void *)turbo_agent_runtime_get_thread_head_trace_events_json_value;
    void *agent_runtime_get_run_state = (void *)turbo_agent_runtime_get_run_state_json_value;
    void *agent_runtime_get_run_trace_events =
        (void *)turbo_agent_runtime_get_run_trace_events_json_value;
    void *agent_runtime_get_checkpoint_state =
        (void *)turbo_agent_runtime_get_checkpoint_state_json_value;
    void *agent_runtime_prepare_checkpoint_state_override =
        (void *)turbo_agent_runtime_prepare_checkpoint_state_override_json_value;
    void *agent_runtime_prepare_thread_state_override =
        (void *)turbo_agent_runtime_prepare_thread_state_override_json_value;
    void *agent_runtime_get_checkpoint_trace_events =
        (void *)turbo_agent_runtime_get_checkpoint_trace_events_json_value;
    void *agent_runtime_list_runs = (void *)turbo_agent_runtime_list_runs;
    void *agent_runtime_list_thread_lineage = (void *)turbo_agent_runtime_list_thread_lineage;
    void *agent_runtime_list_child_runs = (void *)turbo_agent_runtime_list_child_runs;
    void *agent_runtime_list_checkpoints = (void *)turbo_agent_runtime_list_checkpoints;
    void *agent_runtime_history = (void *)turbo_agent_runtime_load_history_events_json_value;
    void *agent_runtime_thread_history = (void *)turbo_agent_runtime_load_thread_history_events_json_value;
    void *agent_runtime_replay_history = (void *)turbo_agent_runtime_replay_history_json_value;
    void *agent_runtime_replay_thread_history =
        (void *)turbo_agent_runtime_replay_thread_history_json_value;
    void *agent_runtime_observe_history = (void *)turbo_agent_runtime_observe_history_json_value;
    void *agent_runtime_observe_thread_history =
        (void *)turbo_agent_runtime_observe_thread_history_json_value;
    void *agent_tool_registry = (void *)turbo_agent_tool_registry;
    void *agent_tool_count = (void *)turbo_agent_tool_count;
    void *agent_app_create = (void *)turbo_agent_app_create;
    void *agent_app_destroy = (void *)turbo_agent_app_destroy;
    void *agent_app_session = (void *)turbo_agent_app_session;
    void *agent_app_thread_id = (void *)turbo_agent_app_thread_id;
    void *agent_app_last_run_id = (void *)turbo_agent_app_last_run_id;
    void *agent_app_last_checkpoint_id = (void *)turbo_agent_app_last_checkpoint_id;
    void *agent_app_workflow_kind = (void *)turbo_agent_app_workflow_kind;
    void *agent_app_memory_namespace = (void *)turbo_agent_app_memory_namespace;
    void *agent_app_memory_store = (void *)turbo_agent_app_memory_store;
    void *agent_app_tool_registry = (void *)turbo_agent_app_tool_registry;
    void *agent_app_tool_count = (void *)turbo_agent_app_tool_count;
    void *agent_app_get_capabilities = (void *)turbo_agent_app_get_capabilities;
    void *agent_app_get_tool_schemas = (void *)turbo_agent_app_get_tool_schemas;
    void *agent_app_get_startup_diagnostics =
        (void *)turbo_agent_app_get_startup_diagnostics;
    void *agent_app_add_trace_json_value_sink = (void *)turbo_agent_app_add_trace_json_value_sink;
    void *agent_app_add_observer_json_value_sink = (void *)turbo_agent_app_add_observer_json_value_sink;
    void *agent_app_set_trace_history_enabled = (void *)turbo_agent_app_set_trace_history_enabled;
    void *agent_app_apply_thread_state_patch =
        (void *)turbo_agent_app_apply_thread_state_patch_json_value;
    void *agent_app_get_observability_index =
        (void *)turbo_agent_app_get_observability_index;
    void *agent_app_exec_start = (void *)turbo_agent_app_exec_start;
    void *agent_app_exec_resume = (void *)turbo_agent_app_exec_resume;
    void *agent_app_exec_fork = (void *)turbo_agent_app_exec_fork;
    void *agent_app_start_preset = (void *)turbo_agent_app_start_preset;
    void *agent_app_resume_preset = (void *)turbo_agent_app_resume_preset;
    void *agent_app_fork_preset = (void *)turbo_agent_app_fork_preset;
    void *agent_app_start_text = (void *)turbo_agent_app_start_text;
    void *agent_app_start_text_stream = (void *)turbo_agent_app_start_text_stream;
    void *agent_app_start_messages = (void *)turbo_agent_app_start_messages;
    void *agent_app_start_messages_stream =
        (void *)turbo_agent_app_start_messages_stream;
    void *agent_app_result_text = (void *)turbo_agent_app_result_text;
    void *agent_app_invoke_text = (void *)turbo_agent_app_invoke_text;
    void *agent_app_invoke_messages_text = (void *)turbo_agent_app_invoke_messages_text;
    void *agent_app_invoke_json = (void *)turbo_agent_app_invoke_json;
    void *agent_app_invoke_messages_json = (void *)turbo_agent_app_invoke_messages_json;
    void *agent_app_batch_text = (void *)turbo_agent_app_batch_text;
    void *agent_app_memory_get = (void *)turbo_agent_app_memory_get;
    void *agent_app_memory_put = (void *)turbo_agent_app_memory_put;
    void *agent_app_memory_put_context = (void *)turbo_agent_app_memory_put_context;
    void *agent_app_memory_delete = (void *)turbo_agent_app_memory_delete;
    void *agent_app_memory_list = (void *)turbo_agent_app_memory_list;
    void *agent_app_memory_list_records = (void *)turbo_agent_app_memory_list_records;
    void *agent_app_memory_get_record = (void *)turbo_agent_app_memory_get_record;
    void *agent_app_memory_put_record = (void *)turbo_agent_app_memory_put_record;
    void *agent_app_memory_validate_record = (void *)turbo_agent_app_memory_validate_record;
    void *agent_app_memory_query_records = (void *)turbo_agent_app_memory_query_records;
    void *agent_app_memory_query_records_ex = (void *)turbo_agent_app_memory_query_records_ex;
    void *agent_session_create = (void *)turbo_agent_session_create;
    void *agent_session_destroy = (void *)turbo_agent_session_destroy;
    void *agent_session_agent = (void *)turbo_agent_session_agent;
    void *agent_session_runtime = (void *)turbo_agent_session_runtime;
    void *agent_session_memory_store = (void *)turbo_agent_session_memory_store;
    void *agent_session_model = (void *)turbo_agent_session_model;
    void *agent_session_base_url = (void *)turbo_agent_session_base_url;
    void *agent_session_provider_name = (void *)turbo_agent_session_provider_name;
    void *agent_session_has_api_key = (void *)turbo_agent_session_has_api_key;
    void *agent_session_tool_registry = (void *)turbo_agent_session_tool_registry;
    void *agent_session_tool_count = (void *)turbo_agent_session_tool_count;
    void *agent_session_get_capabilities =
        (void *)turbo_agent_session_get_capabilities;
    void *agent_session_get_tool_schemas =
        (void *)turbo_agent_session_get_tool_schemas;
    void *agent_session_get_startup_diagnostics =
        (void *)turbo_agent_session_get_startup_diagnostics;
    void *agent_session_workflow_kind = (void *)turbo_agent_session_workflow_kind;
    void *agent_session_memory_namespace = (void *)turbo_agent_session_memory_namespace;
    void *agent_session_add_trace_json_value_sink =
        (void *)turbo_agent_session_add_trace_json_value_sink;
    void *agent_session_add_observer_json_value_sink =
        (void *)turbo_agent_session_add_observer_json_value_sink;
    void *agent_session_set_trace_history_enabled =
        (void *)turbo_agent_session_set_trace_history_enabled;
    void *agent_session_apply_thread_state_patch =
        (void *)turbo_agent_session_apply_thread_state_patch_json_value;
    void *agent_session_get_observability_index =
        (void *)turbo_agent_session_get_observability_index;
    void *agent_session_start_graph = (void *)turbo_agent_session_start_graph;
    void *agent_session_resume_graph = (void *)turbo_agent_session_resume_graph;
    void *agent_session_fork_graph = (void *)turbo_agent_session_fork_graph;
    void *agent_session_start_preset = (void *)turbo_agent_session_start_preset;
    void *agent_session_resume_preset = (void *)turbo_agent_session_resume_preset;
    void *agent_session_fork_preset = (void *)turbo_agent_session_fork_preset;
    void *agent_session_start_text = (void *)turbo_agent_session_start_text;
    void *agent_session_start_text_stream =
        (void *)turbo_agent_session_start_text_stream;
    void *agent_session_start_messages = (void *)turbo_agent_session_start_messages;
    void *agent_session_start_messages_stream =
        (void *)turbo_agent_session_start_messages_stream;
    void *agent_session_result_text = (void *)turbo_agent_session_result_text;
    void *agent_session_invoke_text = (void *)turbo_agent_session_invoke_text;
    void *agent_session_invoke_messages_text =
        (void *)turbo_agent_session_invoke_messages_text;
    void *agent_session_invoke_json = (void *)turbo_agent_session_invoke_json;
    void *agent_session_invoke_messages_json =
        (void *)turbo_agent_session_invoke_messages_json;
    void *agent_session_batch_text = (void *)turbo_agent_session_batch_text;
    void *agent_session_memory_get = (void *)turbo_agent_session_memory_get;
    void *agent_session_memory_put = (void *)turbo_agent_session_memory_put;
    void *agent_session_memory_put_context = (void *)turbo_agent_session_memory_put_context;
    void *agent_session_memory_delete = (void *)turbo_agent_session_memory_delete;
    void *agent_session_memory_list = (void *)turbo_agent_session_memory_list;
    void *agent_session_memory_list_records = (void *)turbo_agent_session_memory_list_records;
    void *agent_session_memory_get_record = (void *)turbo_agent_session_memory_get_record;
    void *agent_session_memory_put_record = (void *)turbo_agent_session_memory_put_record;
    void *agent_session_memory_validate_record =
        (void *)turbo_agent_session_memory_validate_record;
    void *agent_session_memory_query_records = (void *)turbo_agent_session_memory_query_records;
    void *agent_session_memory_query_records_ex =
        (void *)turbo_agent_session_memory_query_records_ex;
    void *agent_session_load_memory_context = (void *)turbo_agent_session_load_memory_context;
    void *agent_session_input_state = (void *)turbo_agent_session_create_input_state_json_value;
    void *agent_session_input_messages_state =
        (void *)turbo_agent_session_create_input_messages_state_json_value;
    void *agent_session_input_state_with_memory =
        (void *)turbo_agent_session_create_input_state_with_memory_json_value;
    void *agent_session_input_messages_state_with_memory =
        (void *)turbo_agent_session_create_input_messages_state_with_memory_json_value;
    void *agent_subagent_add_runtime = (void *)turbo_agent_subagent_add_tool_runtime;
    void *agent_subagent_add_registry = (void *)turbo_agent_subagent_add_tool_registry;
    void *agent_subgraph_node = (void *)turbo_agent_subgraph_node;
    void *agent_install_subgraph_node = (void *)turbo_agent_install_subgraph_node;
    void *agent_install_supervisor_loop = (void *)turbo_agent_install_supervisor_loop;
    void *agent_state_trace_events_json_value = (void *)turbo_agent_state_trace_events_json_value;
    void *agent_state_add_trace_event_json_value = (void *)turbo_agent_state_add_trace_event_json_value;
    void *agent_state_capture_trace_event_json_value =
        (void *)turbo_agent_state_capture_trace_event_json_value;
    void *agent_state_planner_event_version_json_value =
        (void *)turbo_agent_state_planner_event_version_json_value;
    void *agent_state_executor_event_version_json_value =
        (void *)turbo_agent_state_executor_event_version_json_value;
    void *agent_state_latest_planner_event_version_json_value =
        (void *)turbo_agent_state_latest_planner_event_version_json_value;
    void *agent_state_latest_executor_event_version_json_value =
        (void *)turbo_agent_state_latest_executor_event_version_json_value;
    void *agent_state_completed_steps_json_value =
        (void *)turbo_agent_state_completed_steps_json_value;
    void *agent_state_completed_step_json_value =
        (void *)turbo_agent_state_completed_step_json_value;
    void *agent_state_latest_completed_step_json_value =
        (void *)turbo_agent_state_latest_completed_step_json_value;
    void *agent_state_latest_tool_results_event =
        (void *)turbo_agent_state_latest_tool_results_event;
    void *agent_state_tool_results_outputs =
        (void *)turbo_agent_state_tool_results_outputs;
    void *agent_tool_result_output_item_create =
        (void *)turbo_agent_tool_result_output_item_create;
    void *agent_state_tool_result_child_thread_id =
        (void *)turbo_agent_state_tool_result_child_thread_id;
    void *agent_state_tool_result_child_run_id =
        (void *)turbo_agent_state_tool_result_child_run_id;
    void *agent_state_tool_result_child_checkpoint_id =
        (void *)turbo_agent_state_tool_result_child_checkpoint_id;
    void *agent_state_tool_result_child_status =
        (void *)turbo_agent_state_tool_result_child_status;
    void *agent_state_tool_result_parent_agent_run_id =
        (void *)turbo_agent_state_tool_result_parent_agent_run_id;
    void *agent_state_tool_result_parent_tool_call_id =
        (void *)turbo_agent_state_tool_result_parent_tool_call_id;
    void *agent_state_tool_result_parent_tool_name =
        (void *)turbo_agent_state_tool_result_parent_tool_name;
    void *agent_state_tool_result_parent_graph_run_id =
        (void *)turbo_agent_state_tool_result_parent_graph_run_id;
    void *agent_state_tool_result_call_frame_id =
        (void *)turbo_agent_state_tool_result_call_frame_id;
    void *agent_state_set_active_agent = (void *)turbo_agent_state_set_active_agent;
    void *agent_state_request_handoff = (void *)turbo_agent_state_request_handoff;
    void *agent_state_commit_handoff = (void *)turbo_agent_state_commit_handoff;
    void *agent_state_append_supervisor_inbox_message =
        (void *)turbo_agent_state_append_supervisor_inbox_message;
    void *agent_state_active_agent = (void *)turbo_agent_state_active_agent;
    void *agent_state_handoff_target_agent = (void *)turbo_agent_state_handoff_target_agent;
    void *agent_state_handoff_reason = (void *)turbo_agent_state_handoff_reason;
    void *agent_state_supervisor_inbox = (void *)turbo_agent_state_supervisor_inbox;
    void *agent_state_supervisor_inbox_count =
        (void *)turbo_agent_state_supervisor_inbox_count;
    void *agent_state_supervisor_inbox_at = (void *)turbo_agent_state_supervisor_inbox_at;
    void *agent_state_supervisor_handoff_history =
        (void *)turbo_agent_state_supervisor_handoff_history;
    void *agent_state_latest_handoff_event = (void *)turbo_agent_state_latest_handoff_event;
    void *agent_state_handoff_event_phase = (void *)turbo_agent_state_handoff_event_phase;
    void *agent_state_handoff_event_from_agent =
        (void *)turbo_agent_state_handoff_event_from_agent;
    void *agent_state_handoff_event_target_agent =
        (void *)turbo_agent_state_handoff_event_target_agent;
    void *agent_state_handoff_event_reason = (void *)turbo_agent_state_handoff_event_reason;
    void *agent_state_handoff_event_active_agent =
        (void *)turbo_agent_state_handoff_event_active_agent;
    void *tool_execute_json_value = (void *)turbo_tool_registry_execute_json_value;
    void *tool_runtime_invoke_json_value = (void *)turbo_tool_runtime_invoke_json_value;
    turbo_tool_registry_t *tool_registry = turbo_tool_registry_create();
    turbo_tool_runtime_t *tool_runtime = turbo_tool_runtime_native_create();
    turbo_action_tool_registry_t *action_registry = turbo_action_tool_registry_create();
    turbo_agent_memory_store_t memory_store = turbo_agent_memory_store_memory_create();
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_session_config_t app_session_config = {0};
    turbo_agent_app_t *app = NULL;
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_session_t *session = NULL;
    json_value_t *messages = turbo_prompt_messages_create();
    json_value_t *agent_state = turbo_agent_state_create();
    turbo_graph_checkpoint_t *checkpoint = NULL;
    const turbo_model_provider_t *provider = turbo_model_provider_by_name("openai");

    if (runtime_value) {
      turbo_graph_checkpoint_create_json_value("entry", 0, runtime_value, &checkpoint);
    }
    app_session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    app_config.session_config = &app_session_config;
    app = turbo_agent_app_create(&app_config);
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session = turbo_agent_session_create(&session_config);

    check_not_null(graph);
    check_not_null(chain);
    check_not_null(runtime_value);
    check_not_null(chain_state);
    check_not_null(chain_run_json_value_stream);
    check_not_null(graph_run_json_value_stream);
    check_not_null(graph_run_json_value_stream_controlled);
    check_not_null(graph_run_checkpoint_json_value);
    check_not_null(graph_run_checkpoint_json_value_stream);
    check_not_null(graph_run_checkpoint_json_value_stream_controlled);
    check_not_null(graph_run_log_create);
    check_not_null(graph_run_json_value_log);
    check_not_null(graph_checkpoint_clone);
    check_not_null(state_graph_create);
    check_not_null(state_graph_add_channel);
    check_not_null(state_graph_add_json_value_node);
    check_not_null(state_graph_add_subgraph_node);
    check_not_null(state_graph_add_json_value_edge);
    check_not_null(state_graph_snapshot_schema_version);
    check_not_null(state_graph_serialize_snapshot);
    check_not_null(state_graph_load_snapshot);
    check_not_null(state_graph_ctx_goto);
    check_not_null(state_graph_ctx_send);
    check_not_null(state_graph_start);
    check_not_null(state_graph_resume);
    check_not_null(state_graph_update_state);
    check_not_null(state_graph_resume_from_history);
    check_not_null(state_graph_fork_from_history);
    check_not_null(state_graph_get_state);
    check_not_null(state_graph_get_run);
    check_not_null(state_graph_get_thread);
    check_not_null(state_graph_get_latest_run);
    check_not_null(state_graph_get_pending_run);
    check_not_null(state_graph_get_thread_state);
    check_not_null(state_graph_get_history_entry);
    check_not_null(state_graph_get_history_entry_state);
    check_not_null(state_graph_get_checkpoint);
    check_not_null(state_graph_list_state_history);
    check_not_null(state_graph_list_checkpoints);
    check_not_null(state_graph_list_runs);
    check_not_null(state_graph_get_checkpoint_context);
    check_not_null(state_graph_get_branch_tree);
    check_not_null(state_graph_store_memory_create);
    check_not_null(state_graph_store_file_create);
    check_not_null(state_graph_store_destroy);
    check_not_null(state_graph_store_delete);
    check_not_null(state_graph_store_list);
    check_not_null(state_graph_store_get_snapshot_descriptor);
    check_not_null(state_graph_store_list_snapshot_descriptors);
    check_not_null(state_graph_store_list_snapshot_descriptors_filtered);
    check_not_null(state_graph_store_get_latest_snapshot_descriptor);
    check_not_null(state_graph_store_get_latest_snapshot_descriptor_for_thread);
    check_not_null(state_graph_store_get_latest_snapshot_descriptor_for_run);
    check_not_null(state_graph_store_get_latest_snapshot_descriptor_for_history_entry);
    check_not_null(state_graph_store_get_latest_snapshot_descriptor_for_checkpoint);
    check_not_null(state_graph_store_get_latest_snapshot_id);
    check_not_null(state_graph_store_get_latest_snapshot_id_for_thread);
    check_not_null(state_graph_store_get_latest_snapshot_id_for_run);
    check_not_null(state_graph_store_get_latest_snapshot_id_for_history_entry);
    check_not_null(state_graph_store_get_latest_snapshot_id_for_checkpoint);
    check_not_null(state_graph_store_load_latest_snapshot);
    check_not_null(state_graph_store_load_latest_snapshot_for_thread);
    check_not_null(state_graph_store_load_latest_snapshot_for_run);
    check_not_null(state_graph_store_load_latest_snapshot_for_history_entry);
    check_not_null(state_graph_store_load_latest_snapshot_for_checkpoint);
    check_not_null(state_graph_store_save_snapshot);
    check_not_null(state_graph_store_load_snapshot);
    check_not_null(prompt_json_value);
    check_not_null(prompt_message_schema);
    check_not_null(prompt_openai_codec);
    check_not_null(prompt_responses_codec);
    check_not_null(prompt_anthropic_codec);
    check_not_null(provider_wire_codec);
    check_not_null(provider_event_codec);
    check_not_null(provider_sse_event_codec);
    check_not_null(provider_event_json_value_codec);
    check_not_null(provider_event_emit_json_value);
    check_not_null(provider_sse_event_json_value_codec);
    check_not_null(provider_sse_emit_json_value);
    check_not_null(event_kind_json_value);
    check_not_null(event_stream_mode_accepts_json_value);
    check_not_null(event_stream_filter_sink_json_value);
    check_not_null(event_validate_any_json_value);
    check_not_null(event_schema_json_value);
    check_not_null(event_log_create);
    check_not_null(event_log_capture);
    check_not_null(event_log_events);
    check_not_null(event_log_reset);
    check_not_null(event_log_load_events);
    check_not_null(trace_event_schema_json_value);
    check_not_null(tool_result_event_schema_json_value);
    check_not_null(handoff_event_schema_json_value);
    check_not_null(trace_event_validate_json_value);
    check_not_null(trace_event_create_json_value);
    check_not_null(event_validate_json_value);
    check_not_null(event_create_json_value);
    check_not_null(tool_result_event_validate_json_value);
    check_not_null(tool_result_event_create_json_value);
    check_not_null(handoff_event_validate_json_value);
    check_not_null(handoff_event_create_json_value);
    check_not_null(runnable_create);
    check_not_null(runnable_invoke_json_value);
    check_not_null(runnable_invoke_json_value_stream);
    check_not_null(runnable_invoke_json_value_log);
    check_not_null(runnable_batch_json_value);
    check_not_null(runnable_pipe);
    check_not_null(runnable_wrap_json_value);
    check_not_null(runnable_from_state_graph);
    check_not_null(runnable_from_agent_session);
    check_not_null(runnable_from_agent_app);
    check_not_null(langchain_runnable_batch);
    check_not_null(langchain_runnable_wrap);
    check_not_null(langchain_runnable_from_session);
    check_not_null(langchain_runnable_from_agent);
    check_not_null(chain_run_json_value_log);
    check_not_null(agent_policy_default);
    check_not_null(agent_policy_validate_path);
    check_not_null(agent_policy_is_dangerous_command);
    check_not_null(agent_policy_allows_capability);
    check_not_null(agent_policy_check_action);
    check_not_null(agent_policy_requires_approval);
    check_not_null(knowledge_store_sqlite_open);
    check_not_null(knowledge_store_close);
    check_not_null(knowledge_store_upsert_text);
    check_not_null(knowledge_store_index_file);
    check_not_null(knowledge_store_index_directory);
    check_not_null(knowledge_store_index_directory_ex);
    check_not_null(knowledge_store_delete_document);
    check_not_null(knowledge_store_query);
    check_not_null(knowledge_store_query_ex);
    check_not_null(knowledge_store_list_documents);
    check_not_null(knowledge_store_get_document);
    check_not_null(knowledge_store_load_context);
    check_not_null(knowledge_store_load_context_ex);
    check_not_null(knowledge_store_build_context);
    check_not_null(knowledge_store_stats);
    check_not_null(knowledge_store_tool_graph);
    check_not_null(knowledge_store_add_tools);
    check_not_null(knowledge_store_add_action_tools);
    check_not_null(knowledge_context_node);
    check_not_null(knowledge_engineering_loop);
    check_not_null(retriever_create);
    check_not_null(retriever_destroy);
    check_not_null(retriever_query);
    check_not_null(retriever_build_context);
    check_not_null(retriever_load_context);
    check_not_null(retriever_context_node);
    check_not_null(retriever_engineering_loop);
    check_not_null(retriever_from_knowledge_store);
    check_not_null(retriever_from_vector_store);
    check_not_null(embedding_model_create);
    check_not_null(embedding_model_destroy);
    check_not_null(embedding_model_embed_text);
    check_not_null(embedding_model_create_hashing);
    check_not_null(vector_store_create_memory);
    check_not_null(vector_store_sqlite_open);
    check_not_null(vector_store_destroy);
    check_not_null(vector_store_upsert);
    check_not_null(vector_store_delete_document);
    check_not_null(vector_store_query);
    check_not_null(vector_store_index_text);
    check_not_null(vector_store_index_text_file);
    check_not_null(vector_store_index_directory);
    check_not_null(vector_store_index_directory_ex);
    check_not_null(vector_store_count);
    check_not_null(vector_store_tool_binding_create);
    check_not_null(vector_store_tool_binding_destroy);
    check_not_null(vector_store_tool_graph);
    check_not_null(vector_store_add_tools);
    check_not_null(vector_store_add_action_tools);
    check_not_null(vector_store_add_indexing_action_tools);
    check_not_null(text_splitter_split_text);
    check_not_null(document_loader_load_text_file);
    check_not_null(agent_trace_json_value_sink);
    check_not_null(agent_runtime_create);
    check_not_null(agent_runtime_destroy);
    check_not_null(agent_runtime_remote_create);
    check_not_null(agent_runtime_remote_destroy);
    check_not_null(agent_runtime_remote_dispatch);
    check_not_null(agent_runtime_remote_dispatch_text);
    check_not_null(agent_runtime_remote_handle_http);
    check_not_null(agent_runtime_remote_client_create);
    check_not_null(agent_runtime_remote_client_destroy);
    check_not_null(agent_runtime_remote_client_call);
    check_not_null(agent_runtime_remote_client_get_startup_diagnostics);
    check_not_null(agent_runtime_remote_client_start);
    check_not_null(agent_runtime_remote_client_resume);
    check_not_null(agent_runtime_remote_client_fork);
    check_not_null(agent_runtime_remote_client_get_thread_state);
    check_not_null(agent_runtime_remote_client_get_checkpoint_context);
    check_not_null(agent_runtime_remote_client_get_run);
    check_not_null(agent_runtime_remote_client_get_checkpoint);
    check_not_null(agent_runtime_remote_client_list_checkpoints);
    check_not_null(agent_runtime_remote_client_load_history_events);
    check_not_null(agent_runtime_remote_client_get_run_trace_events);
    check_not_null(agent_runtime_remote_client_get_checkpoint_trace_events);
    check_not_null(agent_runtime_remote_client_get_memory_record);
    check_not_null(agent_runtime_remote_client_delete_memory_record);
    check_not_null(agent_runtime_remote_client_put_memory_record);
    check_not_null(agent_runtime_remote_client_validate_memory_record);
    check_not_null(agent_runtime_remote_client_query_memory_records_ex);
    check_not_null(agent_runtime_remote_client_query_memory_records);
    check_not_null(agent_runtime_remote_client_list_memory_records);
    check_not_null(agent_runtime_remote_client_get_thread_timeline);
    check_not_null(agent_runtime_remote_client_get_branch_tree);
    check_not_null(agent_runtime_remote_client_get_observability_index);
    check_not_null(agent_runtime_remote_client_list_observability_indexes);
    check_not_null(agent_runtime_remote_client_list_observability_indexes_filtered);
    check_not_null(agent_runtime_remote_client_list_child_runs);
    check_not_null(agent_runtime_remote_client_get_supervisor_inspect);
    check_not_null(agent_runtime_remote_client_get_orchestration_inspect);
    check_not_null(agent_runtime_remote_client_get_child_inspect);
    check_not_null(agent_runtime_remote_client_get_child_orchestration_inspect);
    check_not_null(agent_runtime_remote_client_get_child_multi_agent_inspect);
    check_not_null(agent_runtime_remote_client_resume_thread_command);
    check_not_null(agent_runtime_remote_client_fork_thread_command);
    check_not_null(agent_remote_session_create);
    check_not_null(agent_remote_session_destroy);
    check_not_null(agent_remote_session_client);
    check_not_null(agent_remote_session_thread_id);
    check_not_null(agent_remote_session_last_run_id);
    check_not_null(agent_remote_session_last_checkpoint_id);
    check_not_null(agent_remote_session_get_startup_diagnostics);
    check_not_null(agent_remote_session_start);
    check_not_null(agent_remote_session_start_text);
    check_not_null(agent_remote_session_start_messages);
    check_not_null(agent_remote_session_resume);
    check_not_null(agent_remote_session_fork);
    check_not_null(agent_remote_session_invoke_text);
    check_not_null(agent_remote_session_invoke_messages_text);
    check_not_null(agent_remote_session_invoke_json);
    check_not_null(agent_remote_session_invoke_messages_json);
    check_not_null(agent_remote_session_get_thread);
    check_not_null(agent_remote_session_get_latest_run);
    check_not_null(agent_remote_session_get_pending_run);
    check_not_null(agent_remote_session_get_thread_state);
    check_not_null(agent_remote_session_memory_list_records);
    check_not_null(agent_remote_session_memory_delete_record);
    check_not_null(agent_remote_session_memory_get_record);
    check_not_null(agent_remote_session_memory_put_record);
    check_not_null(agent_remote_session_memory_validate_record);
    check_not_null(agent_remote_session_memory_query_records);
    check_not_null(agent_remote_session_memory_query_records_ex);
    check_not_null(agent_remote_session_get_checkpoint_context);
    check_not_null(agent_remote_session_get_observability_index);
    check_not_null(agent_remote_session_get_thread_timeline);
    check_not_null(agent_remote_session_load_thread_history_events);
    check_not_null(agent_remote_session_replay_thread_history);
    check_not_null(agent_remote_session_observe_thread_history);
    check_not_null(agent_remote_session_get_thread_trace_events);
    check_not_null(agent_remote_session_get_branch_tree);
    check_not_null(agent_remote_session_list_thread_lineage);
    check_not_null(agent_remote_session_get_supervisor_inbox);
    check_not_null(agent_remote_session_get_supervisor_handoff_history);
    check_not_null(agent_remote_session_get_supervisor_inspect);
    check_not_null(agent_remote_session_list_child_runs);
    check_not_null(agent_remote_session_get_orchestration_inspect);
    check_not_null(agent_remote_session_get_child_run);
    check_not_null(agent_remote_session_get_child_checkpoint);
    check_not_null(agent_remote_session_get_child_checkpoint_context);
    check_not_null(agent_remote_session_get_child_thread_timeline);
    check_not_null(agent_remote_session_get_child_branch_tree);
    check_not_null(agent_remote_session_list_child_checkpoints);
    check_not_null(agent_remote_session_load_child_history_events);
    check_not_null(agent_remote_session_get_child_trace_events);
    check_not_null(agent_remote_session_get_child_inspect);
    check_not_null(agent_remote_session_get_child_orchestration_inspect);
    check_not_null(agent_remote_session_get_child_multi_agent_inspect);
    check_not_null(agent_remote_session_resume_thread_command);
    check_not_null(agent_remote_session_fork_thread_command);
    check_not_null(agent_remote_app_create);
    check_not_null(agent_remote_app_destroy);
    check_not_null(agent_remote_app_session);
    check_not_null(agent_remote_app_graph_name);
    check_not_null(agent_remote_app_thread_id);
    check_not_null(agent_remote_app_last_run_id);
    check_not_null(agent_remote_app_last_checkpoint_id);
    check_not_null(agent_remote_app_get_startup_diagnostics);
    check_not_null(agent_remote_app_start);
    check_not_null(agent_remote_app_start_text);
    check_not_null(agent_remote_app_start_messages);
    check_not_null(agent_remote_app_resume);
    check_not_null(agent_remote_app_fork);
    check_not_null(agent_remote_app_invoke_text);
    check_not_null(agent_remote_app_invoke_messages_text);
    check_not_null(agent_remote_app_invoke_json);
    check_not_null(agent_remote_app_invoke_messages_json);
    check_not_null(agent_remote_app_get_thread);
    check_not_null(agent_remote_app_get_latest_run);
    check_not_null(agent_remote_app_get_pending_run);
    check_not_null(agent_remote_app_get_thread_state);
    check_not_null(agent_remote_app_memory_list_records);
    check_not_null(agent_remote_app_memory_delete_record);
    check_not_null(agent_remote_app_memory_get_record);
    check_not_null(agent_remote_app_memory_put_record);
    check_not_null(agent_remote_app_memory_validate_record);
    check_not_null(agent_remote_app_memory_query_records);
    check_not_null(agent_remote_app_memory_query_records_ex);
    check_not_null(agent_remote_app_get_checkpoint_context);
    check_not_null(agent_remote_app_get_observability_index);
    check_not_null(agent_remote_app_get_thread_timeline);
    check_not_null(agent_remote_app_load_thread_history_events);
    check_not_null(agent_remote_app_replay_thread_history);
    check_not_null(agent_remote_app_observe_thread_history);
    check_not_null(agent_remote_app_get_thread_trace_events);
    check_not_null(agent_remote_app_get_branch_tree);
    check_not_null(agent_remote_app_list_thread_lineage);
    check_not_null(agent_remote_app_get_supervisor_inbox);
    check_not_null(agent_remote_app_get_supervisor_handoff_history);
    check_not_null(agent_remote_app_get_supervisor_inspect);
    check_not_null(agent_remote_app_list_child_runs);
    check_not_null(agent_remote_app_get_orchestration_inspect);
    check_not_null(agent_remote_app_get_child_run);
    check_not_null(agent_remote_app_get_child_checkpoint);
    check_not_null(agent_remote_app_get_child_checkpoint_context);
    check_not_null(agent_remote_app_get_child_thread_timeline);
    check_not_null(agent_remote_app_get_child_branch_tree);
    check_not_null(agent_remote_app_list_child_checkpoints);
    check_not_null(agent_remote_app_load_child_history_events);
    check_not_null(agent_remote_app_get_child_trace_events);
    check_not_null(agent_remote_app_get_child_inspect);
    check_not_null(agent_remote_app_get_child_orchestration_inspect);
    check_not_null(agent_remote_app_get_child_multi_agent_inspect);
    check_not_null(agent_remote_app_resume_thread_command);
    check_not_null(agent_remote_app_fork_thread_command);
    check_not_null(agent_runtime_remote_chttp_mount);
#if 0
    check_not_null(agent_runtime_store_memory);
    check_not_null(agent_runtime_store_file);
    check_not_null(agent_runtime_start);
    check_not_null(agent_runtime_start_linked);
    check_not_null(agent_runtime_start_stream);
    check_not_null(agent_runtime_resume);
    check_not_null(agent_runtime_resume_stream);
    check_not_null(agent_runtime_fork);
    check_not_null(agent_runtime_fork_stream);
    check_not_null(agent_runtime_resume_checkpoint);
    check_not_null(agent_runtime_fork_checkpoint);
    check_not_null(agent_runtime_get_thread);
    check_not_null(agent_runtime_get_run);
    check_not_null(agent_runtime_get_latest_run);
    check_not_null(agent_runtime_get_pending_run);
    check_not_null(agent_runtime_get_thread_timeline);
    check_not_null(agent_runtime_get_observability_index);
    check_not_null(agent_runtime_list_observability_indexes);
    check_not_null(agent_runtime_list_observability_indexes_filtered);
    check_not_null(agent_runtime_get_checkpoint);
    check_not_null(agent_runtime_get_latest_checkpoint);
    check_not_null(agent_runtime_get_checkpoint_context);
    check_not_null(agent_runtime_get_thread_state);
    check_not_null(agent_runtime_get_thread_trace_events);
    check_not_null(agent_runtime_get_run_state);
    check_not_null(agent_runtime_get_run_trace_events);
    check_not_null(agent_runtime_get_checkpoint_state);
    check_not_null(agent_runtime_update_checkpoint_state);
    check_not_null(agent_runtime_update_thread_state);
    check_not_null(agent_runtime_get_checkpoint_trace_events);
    check_not_null(agent_runtime_list_runs);
    check_not_null(agent_runtime_list_thread_lineage);
    check_not_null(agent_runtime_list_child_runs);
    check_not_null(agent_runtime_list_checkpoints);
    check_not_null(agent_runtime_history);
    check_not_null(agent_runtime_thread_history);
    check_not_null(agent_runtime_replay_history);
    check_not_null(agent_runtime_replay_thread_history);
    check_not_null(agent_runtime_observe_history);
    check_not_null(agent_runtime_observe_thread_history);
    check_not_null(agent_runtime_apply_command);
    check_not_null(agent_runtime_apply_checkpoint_command);
    check_not_null(agent_runtime_apply_thread_command);
    check_not_null(agent_runtime_resume_command);
    check_not_null(agent_runtime_resume_checkpoint_command);
    check_not_null(agent_runtime_resume_thread_command);
    check_not_null(agent_runtime_fork_command);
    check_not_null(agent_runtime_fork_checkpoint_command);
    check_not_null(agent_runtime_fork_thread_command);
    check_not_null(agent_runtime_resume_checkpoint_state);
    check_not_null(agent_runtime_resume_thread_state);
    check_not_null(agent_runtime_fork_checkpoint_state);
    check_not_null(agent_runtime_fork_thread_state);
    check_not_null(agent_memory_store_memory);
    check_not_null(agent_memory_store_file);
    check_not_null(agent_memory_store_destroy);
    check_not_null(agent_memory_get);
    check_not_null(agent_memory_put);
    check_not_null(agent_memory_delete);
    check_not_null(agent_memory_list);
    check_not_null(agent_memory_list_records);
    check_not_null(agent_memory_get_record);
    check_not_null(agent_memory_put_record);
    check_not_null(agent_memory_validate_record);
    check_not_null(agent_memory_query_records);
    check_not_null(agent_memory_query_records_ex);
    check_not_null(agent_tool_registry);
    check_not_null(agent_tool_count);
    check_not_null(agent_app_create);
    check_not_null(agent_app_destroy);
    check_not_null(agent_app_session);
    check_not_null(agent_app_thread_id);
    check_not_null(agent_app_last_run_id);
    check_not_null(agent_app_last_checkpoint_id);
    check_not_null(agent_app_workflow_kind);
    check_not_null(agent_app_memory_namespace);
    check_not_null(agent_app_memory_store);
    check_not_null(agent_app_tool_registry);
    check_not_null(agent_app_tool_count);
    check_not_null(agent_app_get_capabilities);
    check_not_null(agent_app_get_tool_schemas);
    check_not_null(agent_app_get_startup_diagnostics);
    check_not_null(agent_app_add_trace_json_value_sink);
    check_not_null(agent_app_add_observer_json_value_sink);
    check_not_null(agent_app_set_trace_history_enabled);
    check_not_null(agent_app_get_thread);
    check_not_null(agent_app_get_run);
    check_not_null(agent_app_get_latest_run);
    check_not_null(agent_app_get_pending_run);
    check_not_null(agent_app_get_thread_timeline);
    check_not_null(agent_app_get_observability_index);
    check_not_null(agent_app_get_checkpoint);
    check_not_null(agent_app_get_latest_checkpoint);
    check_not_null(agent_app_get_checkpoint_context);
    check_not_null(agent_app_get_thread_state);
    check_not_null(agent_app_get_thread_trace_events);
    check_not_null(agent_app_get_run_state);
    check_not_null(agent_app_get_run_trace_events);
    check_not_null(agent_app_get_checkpoint_state);
    check_not_null(agent_app_update_checkpoint_state);
    check_not_null(agent_app_update_thread_state);
    check_not_null(agent_app_get_supervisor_inbox);
    check_not_null(agent_app_get_supervisor_handoff_history);
    check_not_null(agent_app_get_supervisor_inspect);
    check_not_null(agent_app_get_orchestration_inspect);
    check_not_null(agent_app_append_supervisor_inbox_message);
    check_not_null(agent_app_get_checkpoint_trace_events);
    check_not_null(agent_app_get_child_run);
    check_not_null(agent_app_get_child_checkpoint);
    check_not_null(agent_app_get_child_checkpoint_context);
    check_not_null(agent_app_get_child_thread_timeline);
    check_not_null(agent_app_get_child_branch_tree);
    check_not_null(agent_app_get_child_inspect);
    check_not_null(agent_app_get_child_orchestration_inspect);
    check_not_null(agent_app_get_child_multi_agent_inspect);
    check_not_null(agent_app_list_runs);
    check_not_null(agent_app_list_thread_lineage);
    check_not_null(agent_app_list_child_runs);
    check_not_null(agent_app_list_child_checkpoints);
    check_not_null(agent_app_list_checkpoints);
    check_not_null(agent_app_history);
    check_not_null(agent_app_thread_history);
    check_not_null(agent_app_replay_history);
    check_not_null(agent_app_replay_thread_history);
    check_not_null(agent_app_observe_history);
    check_not_null(agent_app_observe_thread_history);
    check_not_null(agent_app_start_stream);
    check_not_null(agent_app_resume_stream);
    check_not_null(agent_app_fork_stream);
    check_not_null(agent_app_child_history);
    check_not_null(agent_app_child_trace_events);
    check_not_null(agent_app_apply_command);
    check_not_null(agent_app_apply_checkpoint_command);
    check_not_null(agent_app_apply_thread_command);
    check_not_null(agent_app_resume_command);
    check_not_null(agent_app_resume_checkpoint_command);
    check_not_null(agent_app_resume_thread_command);
    check_not_null(agent_app_fork_command);
    check_not_null(agent_app_fork_checkpoint_command);
    check_not_null(agent_app_fork_thread_command);
    check_not_null(agent_app_resume_checkpoint);
    check_not_null(agent_app_fork_checkpoint);
    check_not_null(agent_app_resume_checkpoint_state);
    check_not_null(agent_app_resume_thread_state);
    check_not_null(agent_app_fork_checkpoint_state);
    check_not_null(agent_app_fork_thread_state);
    check_not_null(agent_app_resume_preset);
    check_not_null(agent_app_fork_preset);
    check_not_null(agent_app_resume_preset_command);
    check_not_null(agent_app_resume_thread_preset_command);
    check_not_null(agent_app_fork_preset_command);
    check_not_null(agent_app_fork_thread_preset_command);
    check_not_null(agent_app_start_text);
    check_not_null(agent_app_start_text_stream);
    check_not_null(agent_app_start_messages);
    check_not_null(agent_app_start_messages_stream);
    check_not_null(agent_app_invoke_text);
    check_not_null(agent_app_invoke_messages_text);
    check_not_null(agent_app_invoke_json);
    check_not_null(agent_app_invoke_messages_json);
    check_not_null(agent_app_batch_text);
    check_not_null(agent_app_memory_get);
    check_not_null(agent_app_memory_put);
    check_not_null(agent_app_memory_put_context);
    check_not_null(agent_app_memory_delete);
    check_not_null(agent_app_memory_list);
    check_not_null(agent_app_memory_list_records);
    check_not_null(agent_app_memory_get_record);
    check_not_null(agent_app_memory_put_record);
    check_not_null(agent_app_memory_validate_record);
    check_not_null(agent_app_memory_query_records);
    check_not_null(agent_app_memory_query_records_ex);
    check_not_null(agent_session_create);
    check_not_null(agent_session_destroy);
    check_not_null(agent_session_agent);
    check_not_null(agent_session_runtime);
    check_not_null(agent_session_memory_store);
    check_not_null(agent_session_model);
    check_not_null(agent_session_base_url);
    check_not_null(agent_session_provider_name);
    check_not_null(agent_session_has_api_key);
    check_not_null(agent_session_tool_registry);
    check_not_null(agent_session_tool_count);
    check_not_null(agent_session_get_capabilities);
    check_not_null(agent_session_get_tool_schemas);
    check_not_null(agent_session_get_startup_diagnostics);
    check_not_null(agent_session_workflow_kind);
    check_not_null(agent_session_memory_namespace);
    check_not_null(agent_session_add_trace_json_value_sink);
    check_not_null(agent_session_add_observer_json_value_sink);
    check_not_null(agent_session_set_trace_history_enabled);
    check_not_null(agent_session_get_thread);
    check_not_null(agent_session_get_run);
    check_not_null(agent_session_get_latest_run);
    check_not_null(agent_session_get_pending_run);
    check_not_null(agent_session_get_thread_timeline);
    check_not_null(agent_session_get_observability_index);
    check_not_null(agent_session_get_checkpoint);
    check_not_null(agent_session_get_latest_checkpoint);
    check_not_null(agent_session_get_checkpoint_context);
    check_not_null(agent_session_get_thread_state);
    check_not_null(agent_session_get_thread_trace_events);
    check_not_null(agent_session_get_run_state);
    check_not_null(agent_session_get_run_trace_events);
    check_not_null(agent_session_get_checkpoint_state);
    check_not_null(agent_session_update_checkpoint_state);
    check_not_null(agent_session_update_thread_state);
    check_not_null(agent_session_get_supervisor_inbox);
    check_not_null(agent_session_get_supervisor_handoff_history);
    check_not_null(agent_session_get_supervisor_inspect);
    check_not_null(agent_session_get_orchestration_inspect);
    check_not_null(agent_session_append_supervisor_inbox_message);
    check_not_null(agent_session_get_checkpoint_trace_events);
    check_not_null(agent_session_get_child_run);
    check_not_null(agent_session_get_child_checkpoint);
    check_not_null(agent_session_get_child_checkpoint_context);
    check_not_null(agent_session_get_child_thread_timeline);
    check_not_null(agent_session_get_child_branch_tree);
    check_not_null(agent_session_get_child_inspect);
    check_not_null(agent_session_get_child_orchestration_inspect);
    check_not_null(agent_session_get_child_multi_agent_inspect);
    check_not_null(agent_session_list_runs);
    check_not_null(agent_session_list_thread_lineage);
    check_not_null(agent_session_list_child_runs);
    check_not_null(agent_session_list_child_checkpoints);
    check_not_null(agent_session_list_checkpoints);
    check_not_null(agent_session_history);
    check_not_null(agent_session_thread_history);
    check_not_null(agent_session_replay_history);
    check_not_null(agent_session_replay_thread_history);
    check_not_null(agent_session_observe_history);
    check_not_null(agent_session_observe_thread_history);
    check_not_null(agent_session_start_stream);
    check_not_null(agent_session_resume_stream);
    check_not_null(agent_session_fork_stream);
    check_not_null(agent_session_child_history);
    check_not_null(agent_session_child_trace_events);
    check_not_null(agent_session_apply_command);
    check_not_null(agent_session_apply_checkpoint_command);
    check_not_null(agent_session_apply_thread_command);
    check_not_null(agent_session_resume_command);
    check_not_null(agent_session_resume_checkpoint_command);
    check_not_null(agent_session_resume_thread_command);
    check_not_null(agent_session_fork_command);
    check_not_null(agent_session_fork_checkpoint_command);
    check_not_null(agent_session_fork_thread_command);
    check_not_null(agent_session_resume_checkpoint);
    check_not_null(agent_session_fork_checkpoint);
    check_not_null(agent_session_resume_checkpoint_state);
    check_not_null(agent_session_resume_thread_state);
    check_not_null(agent_session_fork_checkpoint_state);
    check_not_null(agent_session_fork_thread_state);
    check_not_null(agent_session_resume_preset_command);
    check_not_null(agent_session_resume_thread_preset_command);
    check_not_null(agent_session_fork_preset_command);
    check_not_null(agent_session_fork_thread_preset_command);
    check_not_null(agent_session_start);
    check_not_null(agent_session_resume);
    check_not_null(agent_session_fork);
    check_not_null(agent_session_loop_graph);
    check_not_null(agent_session_review_graph);
    check_not_null(agent_session_engineering_graph);
    check_not_null(agent_session_knowledge_engineering_graph);
    check_not_null(agent_session_retriever_engineering_graph);
    check_not_null(agent_session_preset_graph);
    check_not_null(agent_session_input_state);
    check_not_null(agent_session_input_messages_state);
    check_not_null(agent_session_start_preset);
    check_not_null(agent_session_start_preset_text);
    check_not_null(agent_session_start_preset_text_with_memory);
    check_not_null(agent_session_start_preset_messages);
    check_not_null(agent_session_start_text);
    check_not_null(agent_session_start_text_stream);
    check_not_null(agent_session_start_messages);
    check_not_null(agent_session_start_messages_stream);
    check_not_null(agent_session_result_text);
    check_not_null(agent_session_invoke_preset_text);
    check_not_null(agent_session_invoke_text);
    check_not_null(agent_session_invoke_preset_text_with_memory);
    check_not_null(agent_session_invoke_preset_messages_text);
    check_not_null(agent_session_invoke_messages_text);
    check_not_null(agent_session_invoke_preset_json);
    check_not_null(agent_session_invoke_json);
    check_not_null(agent_session_invoke_preset_json_with_memory);
    check_not_null(agent_session_invoke_preset_messages_json);
    check_not_null(agent_session_invoke_messages_json);
    check_not_null(agent_session_batch_text);
    check_not_null(agent_session_memory_get);
    check_not_null(agent_session_memory_put);
    check_not_null(agent_session_memory_put_context);
    check_not_null(agent_session_memory_delete);
    check_not_null(agent_session_memory_list);
    check_not_null(agent_session_memory_list_records);
    check_not_null(agent_session_memory_get_record);
    check_not_null(agent_session_memory_put_record);
    check_not_null(agent_session_memory_validate_record);
    check_not_null(agent_session_memory_query_records);
    check_not_null(agent_session_memory_query_records_ex);
    check_not_null(agent_session_load_memory_context);
    check_not_null(agent_session_input_state_with_memory);
    check_not_null(agent_session_input_messages_state_with_memory);
    check_not_null(agent_session_resume_preset);
    check_not_null(agent_session_fork_preset);
#endif
    check_not_null(agent_runtime_store_memory);
    check_not_null(agent_runtime_store_file);
    check_not_null(agent_runtime_exec_start);
    check_not_null(agent_runtime_exec_resume);
    check_not_null(agent_runtime_exec_fork);
    check_not_null(agent_runtime_exec_start_controlled);
    check_not_null(agent_runtime_exec_resume_controlled);
    check_not_null(agent_runtime_exec_fork_controlled);
    check_not_null(agent_execution_start);
    check_not_null(agent_execution_resume);
    check_not_null(agent_execution_fork);
    check_not_null(agent_execution_retain);
    check_not_null(agent_execution_release);
    check_not_null(agent_execution_id);
    check_not_null(agent_execution_cancel);
    check_not_null(agent_execution_wait);
    check_not_null(agent_execution_get_status);
    check_not_null(agent_execution_result_code);
    check_not_null(agent_execution_take_result);
    check_not_null(agent_runtime_get_thread);
    check_not_null(agent_runtime_get_run);
    check_not_null(agent_runtime_get_latest_run);
    check_not_null(agent_runtime_get_pending_run);
    check_not_null(agent_runtime_get_thread_timeline);
    check_not_null(agent_runtime_get_observability_index);
    check_not_null(agent_runtime_list_observability_indexes);
    check_not_null(agent_runtime_list_observability_indexes_filtered);
    check_not_null(agent_runtime_get_branch_tree);
    check_not_null(agent_runtime_get_checkpoint);
    check_not_null(agent_runtime_get_latest_checkpoint);
    check_not_null(agent_runtime_get_checkpoint_context);
    check_not_null(agent_runtime_get_thread_state);
    check_not_null(agent_runtime_get_thread_head_state);
    check_not_null(agent_runtime_get_thread_trace_events);
    check_not_null(agent_runtime_get_thread_head_trace_events);
    check_not_null(agent_runtime_get_run_state);
    check_not_null(agent_runtime_get_run_trace_events);
    check_not_null(agent_runtime_get_checkpoint_state);
    check_not_null(agent_runtime_prepare_checkpoint_state_override);
    check_not_null(agent_runtime_prepare_thread_state_override);
    check_not_null(agent_runtime_get_checkpoint_trace_events);
    check_not_null(agent_runtime_list_runs);
    check_not_null(agent_runtime_list_thread_lineage);
    check_not_null(agent_runtime_list_child_runs);
    check_not_null(agent_runtime_list_checkpoints);
    check_not_null(agent_runtime_history);
    check_not_null(agent_runtime_thread_history);
    check_not_null(agent_runtime_replay_history);
    check_not_null(agent_runtime_replay_thread_history);
    check_not_null(agent_runtime_observe_history);
    check_not_null(agent_runtime_observe_thread_history);
    check_not_null(agent_tool_registry);
    check_not_null(agent_tool_count);
    check_not_null(agent_app_create);
    check_not_null(agent_app_destroy);
    check_not_null(agent_app_session);
    check_not_null(agent_app_thread_id);
    check_not_null(agent_app_last_run_id);
    check_not_null(agent_app_last_checkpoint_id);
    check_not_null(agent_app_workflow_kind);
    check_not_null(agent_app_memory_namespace);
    check_not_null(agent_app_memory_store);
    check_not_null(agent_app_tool_registry);
    check_not_null(agent_app_tool_count);
    check_not_null(agent_app_get_capabilities);
    check_not_null(agent_app_get_tool_schemas);
    check_not_null(agent_app_get_startup_diagnostics);
    check_not_null(agent_app_add_trace_json_value_sink);
    check_not_null(agent_app_add_observer_json_value_sink);
    check_not_null(agent_app_set_trace_history_enabled);
    check_not_null(agent_app_apply_thread_state_patch);
    check_not_null(agent_app_get_observability_index);
    check_not_null(agent_app_exec_start);
    check_not_null(agent_app_exec_resume);
    check_not_null(agent_app_exec_fork);
    check_not_null(agent_app_start_preset);
    check_not_null(agent_app_resume_preset);
    check_not_null(agent_app_fork_preset);
    check_not_null(agent_app_start_text);
    check_not_null(agent_app_start_text_stream);
    check_not_null(agent_app_start_messages);
    check_not_null(agent_app_start_messages_stream);
    check_not_null(agent_app_result_text);
    check_not_null(agent_app_invoke_text);
    check_not_null(agent_app_invoke_messages_text);
    check_not_null(agent_app_invoke_json);
    check_not_null(agent_app_invoke_messages_json);
    check_not_null(agent_app_batch_text);
    check_not_null(agent_app_memory_get);
    check_not_null(agent_app_memory_put);
    check_not_null(agent_app_memory_put_context);
    check_not_null(agent_app_memory_delete);
    check_not_null(agent_app_memory_list);
    check_not_null(agent_app_memory_list_records);
    check_not_null(agent_app_memory_get_record);
    check_not_null(agent_app_memory_put_record);
    check_not_null(agent_app_memory_validate_record);
    check_not_null(agent_app_memory_query_records);
    check_not_null(agent_app_memory_query_records_ex);
    check_not_null(agent_session_create);
    check_not_null(agent_session_destroy);
    check_not_null(agent_session_agent);
    check_not_null(agent_session_runtime);
    check_not_null(agent_session_memory_store);
    check_not_null(agent_session_model);
    check_not_null(agent_session_base_url);
    check_not_null(agent_session_provider_name);
    check_not_null(agent_session_has_api_key);
    check_not_null(agent_session_tool_registry);
    check_not_null(agent_session_tool_count);
    check_not_null(agent_session_get_capabilities);
    check_not_null(agent_session_get_tool_schemas);
    check_not_null(agent_session_get_startup_diagnostics);
    check_not_null(agent_session_workflow_kind);
    check_not_null(agent_session_memory_namespace);
    check_not_null(agent_session_add_trace_json_value_sink);
    check_not_null(agent_session_add_observer_json_value_sink);
    check_not_null(agent_session_set_trace_history_enabled);
    check_not_null(agent_session_apply_thread_state_patch);
    check_not_null(agent_session_get_observability_index);
    check_not_null(agent_session_start_graph);
    check_not_null(agent_session_resume_graph);
    check_not_null(agent_session_fork_graph);
    check_not_null(agent_session_start_preset);
    check_not_null(agent_session_resume_preset);
    check_not_null(agent_session_fork_preset);
    check_not_null(agent_session_start_text);
    check_not_null(agent_session_start_text_stream);
    check_not_null(agent_session_start_messages);
    check_not_null(agent_session_start_messages_stream);
    check_not_null(agent_session_result_text);
    check_not_null(agent_session_invoke_text);
    check_not_null(agent_session_invoke_messages_text);
    check_not_null(agent_session_invoke_json);
    check_not_null(agent_session_invoke_messages_json);
    check_not_null(agent_session_batch_text);
    check_not_null(agent_session_memory_get);
    check_not_null(agent_session_memory_put);
    check_not_null(agent_session_memory_put_context);
    check_not_null(agent_session_memory_delete);
    check_not_null(agent_session_memory_list);
    check_not_null(agent_session_memory_list_records);
    check_not_null(agent_session_memory_get_record);
    check_not_null(agent_session_memory_put_record);
    check_not_null(agent_session_memory_validate_record);
    check_not_null(agent_session_memory_query_records);
    check_not_null(agent_session_memory_query_records_ex);
    check_not_null(agent_session_load_memory_context);
    check_not_null(agent_session_input_state);
    check_not_null(agent_session_input_messages_state);
    check_not_null(agent_session_input_state_with_memory);
    check_not_null(agent_session_input_messages_state_with_memory);
    check_not_null(agent_subagent_add_runtime);
    check_not_null(agent_subagent_add_registry);
    check_not_null(agent_subgraph_node);
    check_not_null(agent_install_subgraph_node);
    check_not_null(agent_install_supervisor_loop);
    check_not_null(agent_state_trace_events_json_value);
    check_not_null(agent_state_add_trace_event_json_value);
    check_not_null(agent_state_capture_trace_event_json_value);
    check_not_null(agent_state_planner_event_version_json_value);
    check_not_null(agent_state_executor_event_version_json_value);
    check_not_null(agent_state_latest_planner_event_version_json_value);
    check_not_null(agent_state_latest_executor_event_version_json_value);
    check_not_null(agent_state_completed_steps_json_value);
    check_not_null(agent_state_completed_step_json_value);
    check_not_null(agent_state_latest_completed_step_json_value);
    check_not_null(agent_state_latest_tool_results_event);
    check_not_null(agent_state_tool_results_outputs);
    check_not_null(agent_tool_result_output_item_create);
    check_not_null(agent_state_tool_result_child_thread_id);
    check_not_null(agent_state_tool_result_child_run_id);
    check_not_null(agent_state_tool_result_child_checkpoint_id);
    check_not_null(agent_state_tool_result_child_status);
    check_not_null(agent_state_tool_result_parent_agent_run_id);
    check_not_null(agent_state_tool_result_parent_tool_call_id);
    check_not_null(agent_state_tool_result_parent_tool_name);
    check_not_null(agent_state_tool_result_parent_graph_run_id);
    check_not_null(agent_state_tool_result_call_frame_id);
    check_not_null(agent_state_set_active_agent);
    check_not_null(agent_state_request_handoff);
    check_not_null(agent_state_commit_handoff);
    check_not_null(agent_state_append_supervisor_inbox_message);
    check_not_null(agent_state_active_agent);
    check_not_null(agent_state_handoff_target_agent);
    check_not_null(agent_state_handoff_reason);
    check_not_null(agent_state_supervisor_inbox);
    check_not_null(agent_state_supervisor_inbox_count);
    check_not_null(agent_state_supervisor_inbox_at);
    check_not_null(agent_state_supervisor_handoff_history);
    check_not_null(agent_state_latest_handoff_event);
    check_not_null(agent_state_handoff_event_phase);
    check_not_null(agent_state_handoff_event_from_agent);
    check_not_null(agent_state_handoff_event_target_agent);
    check_not_null(agent_state_handoff_event_reason);
    check_not_null(agent_state_handoff_event_active_agent);
    check_not_null(tool_execute_json_value);
    check_not_null(tool_runtime_invoke_json_value);
    check_not_null(tool_registry);
    check_not_null(tool_runtime);
    check_not_null(action_registry);
    check_not_null(memory_store.get);
    check_not_null(memory_store.query);
    check_not_null(messages);
    check_not_null(agent_state);
    check_not_null(checkpoint);
    check_not_null(provider);
    check_not_null(app);
    check_not_null(session);

    turbo_agent_app_destroy(app);
    turbo_agent_session_destroy(session);
    turbo_agent_memory_store_destroy(&memory_store);
    turbo_graph_checkpoint_destroy(checkpoint);
    turbo_runtime_json_destroy(chain_state);
    turbo_runtime_json_destroy(runtime_value);
    turbo_free_json(&agent_state);
    turbo_free_json(&messages);
    turbo_action_tool_registry_destroy(action_registry);
    turbo_tool_runtime_destroy(tool_runtime);
    turbo_tool_registry_destroy(tool_registry);
    turbo_chain_destroy(chain);
    turbo_graph_destroy(graph);
  }
}
