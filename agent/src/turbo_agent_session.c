#include "turbo_agent_session.h"

#include "turbo_agent_config_internal.h"
#include "turbo_agent_core_internal.h"
#include "turbo_agent_context_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_inbox_internal.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_session_internal.h"
#include "turbo_agent_session_diagnostics_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include "turbo_agent_session_workflow_internal.h"
#include "turbo_model_provider.h"
#include <json_parser.h>
#include "turbo_prompt.h"
#include "turbo_retriever.h"

#include <cmeta_uuid.h>

#include <stdlib.h>
#include <string.h>

struct turbo_agent_session_s {
  turbo_agent_runtime_t *runtime;
  turbo_agent_t *agent;
  turbo_agent_context_t *context;
  int rc;
  turbo_agent_inbox_t *inbox;
  size_t max_follow_ups_per_execution;
  turbo_agent_memory_store_t memory_store;
  turbo_agent_session_workflow_kind_t workflow_kind;
  tstr thread_id;
  tstr last_run_id;
  tstr last_checkpoint_id;
  tstr memory_namespace;
  tstr parent_agent_run_id;
  tstr parent_tool_call_id;
  tstr parent_tool_name;
  tstr parent_graph_run_id;
  tstr call_frame_id;
  tstr model;
  tstr base_url;
  tstr provider_name;
  int has_api_key;
  tstr knowledge_query;
  tstr knowledge_kind;
  tstr knowledge_uri_prefix;
  turbo_agent_knowledge_context_config_t knowledge_context_config;
  tstr retriever_query;
  tstr retriever_kind;
  tstr retriever_uri_prefix;
  tstr retriever_scope;
  turbo_retriever_context_config_t retriever_context_config;
};

static int turbo_agent_session_apply_claimed_message(turbo_agent_session_t *session,
                                                     turbo_agent_inbox_kind_t kind,
                                                     const json_value_t *record,
                                                     json_value_t *state) {
  json_value_t *event = NULL;
  const json_value_t *payload;
  const char *inbox_id;
  const char *content;
  char event_id[SALTS_UUID_STRING_SIZE];
  cmeta_uuid_t uuid;

  if (!session || !session->inbox || !record || !state) return -1;
  payload = json_object_get(record, "payload");
  inbox_id = json_get_string(record, "inbox_id");
  content = payload ? json_get_string(payload, "content") : NULL;
  if (!inbox_id || !content || content[0] == '\0' || cmeta_uuid_v7_generate(&uuid) != SALTS_OK ||
      cmeta_uuid_format(&uuid, event_id, sizeof(event_id)) != SALTS_OK) {
    (void)turbo_agent_inbox_requeue(session->inbox, inbox_id ? inbox_id : "");
    return -1;
  }
  event = turbo_agent_event_create("inbox_message");
  if (!event) {
    (void)turbo_agent_inbox_requeue(session->inbox, inbox_id);
    return -1;
  }
  json_object_set_string(event, "event_id", event_id);
  json_object_set_string(event, "inbox_id", inbox_id);
  json_object_set_string(event, "inbox_kind",
                               kind == TURBO_AGENT_INBOX_STEER ? "steer" : "follow_up");
  json_object_set_string(event, "content", content);
  if (turbo_agent_inbox_bind_applied_event(session->inbox, inbox_id, event_id) != SALTS_OK ||
      turbo_agent_state_add_user_message(state, content) != 0 ||
      turbo_agent_append_event(state, event) != 0) {
    (void)turbo_agent_inbox_requeue(session->inbox, inbox_id);
    turbo_runtime_json_destroy(event);
    return -1;
  }
  event = NULL;
  return 0;
}

static int turbo_agent_session_apply_pending_steer(turbo_agent_t *agent, json_value_t *state,
                                                   void *user_data) {
  turbo_agent_session_t *session = (turbo_agent_session_t *)user_data;
  turbo_agent_execution_context_t context = {0};
  json_value_t *record = NULL;
  int rc;

  if (!session || !agent || !state) return -1;
  if (session->inbox) {
    turbo_agent_execution_context_get(&context);
    if (!context.run_id || context.run_id[0] == '\0') return -1;
    rc = turbo_agent_inbox_claim(session->inbox, TURBO_AGENT_INBOX_STEER, context.run_id, &record);
    if (rc != SALTS_ENOENT && rc != SALTS_ECANCELED && rc != SALTS_EBUSY) {
      if (rc != SALTS_OK || !record) return -1;
      rc = turbo_agent_session_apply_claimed_message(session, TURBO_AGENT_INBOX_STEER, record,
                                                     state);
      turbo_runtime_json_destroy(record);
      if (rc != SALTS_OK) return rc;
    }
  }
  return session->context ? turbo_agent_context_prepare(session->context, state, 0) : SALTS_OK;
}

static int turbo_agent_session_handle_context_overflow(turbo_agent_t *agent, json_value_t *state,
                                                       int transport_status,
                                                       const char *response_json,
                                                       void *user_data) {
  turbo_agent_session_t *session = (turbo_agent_session_t *)user_data;

  if (!session || !agent || !state || !session->context) return 0;
  return turbo_agent_context_handle_overflow(session->context, state, transport_status,
                                             response_json);
}

#include "turbo_agent_runtime_session_internal.h"

static int turbo_agent_session_should_load_env(const turbo_agent_session_config_t *config) {
  return config && config->load_env ? 1 : 0;
}

static int turbo_agent_session_should_create_agent(const turbo_agent_config_t *config) {
  return config && (config->model || config->api_key || config->base_url || config->endpoint_path ||
                    config->instructions || config->structured_output_name ||
                    config->structured_output_schema_json || config->http_client ||
                    config->transport_fn || config->provider || config->tool_registry);
}

static int turbo_agent_session_capture_summary(turbo_agent_session_t *session,
                                               const json_value_t *summary) {
  if (!session) return -1;
  return turbo_agent_util_capture_summary_ids(summary, &session->thread_id,
                                              &session->last_run_id,
                                              &session->last_checkpoint_id);
}

static const char *turbo_agent_session_resolve_checkpoint_id(const turbo_agent_session_t *session,
                                                             const char *checkpoint_id) {
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    return checkpoint_id;
  }
  return session ? session->last_checkpoint_id : NULL;
}

static const char *
turbo_agent_session_resolve_parent_agent_run_id(const turbo_agent_session_t *session,
                                                const char *parent_agent_run_id) {
  turbo_agent_execution_context_t current_context = {0};

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    return parent_agent_run_id;
  }
  if (session && session->parent_agent_run_id && session->parent_agent_run_id[0] != '\0') {
    return session->parent_agent_run_id;
  }
  turbo_agent_execution_context_get(&current_context);
  return current_context.run_id;
}

static int turbo_agent_session_has_parent_link(const turbo_agent_session_t *session,
                                               turbo_agent_runtime_parent_link_t *out_link) {
  turbo_agent_execution_context_t current_context = {0};
  const char *resolved_parent_agent_run_id;
  const char *resolved_parent_tool_call_id;
  const char *resolved_parent_tool_name;
  const char *resolved_parent_graph_run_id;
  const char *resolved_call_frame_id;
  int has_link = 0;

  if (out_link) {
    memset(out_link, 0, sizeof(*out_link));
  }
  if (!session) {
    return 0;
  }
  turbo_agent_execution_context_get(&current_context);
  resolved_parent_agent_run_id =
      (session->parent_agent_run_id && session->parent_agent_run_id[0] != '\0')
          ? session->parent_agent_run_id
          : current_context.run_id;
  resolved_parent_tool_call_id =
      (session->parent_tool_call_id && session->parent_tool_call_id[0] != '\0')
          ? session->parent_tool_call_id
          : current_context.tool_call_id;
  resolved_parent_tool_name = (session->parent_tool_name && session->parent_tool_name[0] != '\0')
                                  ? session->parent_tool_name
                                  : current_context.tool_name;
  resolved_parent_graph_run_id =
      (session->parent_graph_run_id && session->parent_graph_run_id[0] != '\0')
          ? session->parent_graph_run_id
          : current_context.run_id;
  resolved_call_frame_id = (session->call_frame_id && session->call_frame_id[0] != '\0')
                               ? session->call_frame_id
                               : current_context.tool_call_id;
  if (out_link) {
    out_link->parent_agent_run_id = resolved_parent_agent_run_id;
    out_link->parent_tool_call_id = resolved_parent_tool_call_id;
    out_link->parent_tool_name = resolved_parent_tool_name;
    out_link->parent_graph_run_id = resolved_parent_graph_run_id;
    out_link->call_frame_id = resolved_call_frame_id;
  }
  has_link = (resolved_parent_agent_run_id && resolved_parent_agent_run_id[0] != '\0') ||
             (resolved_parent_tool_call_id && resolved_parent_tool_call_id[0] != '\0') ||
             (resolved_parent_tool_name && resolved_parent_tool_name[0] != '\0') ||
             (resolved_parent_graph_run_id && resolved_parent_graph_run_id[0] != '\0') ||
             (resolved_call_frame_id && resolved_call_frame_id[0] != '\0');
  return has_link;
}

static int turbo_agent_session_run_follow_ups(turbo_agent_session_t *session, turbo_graph_t *graph,
                                              const turbo_graph_run_options_t *options,
                                              turbo_event_sink_json_value_fn event_sink,
                                              void *event_sink_user_data,
                                              turbo_cancel_token_t *cancel_token,
                                              json_value_t **summary_slot,
                                              json_value_t **out_state) {
  size_t follow_up_count;

  if (!session || !session->inbox || !graph || !summary_slot || !out_state) {
    return SALTS_EINVAL;
  }
  for (follow_up_count = 0; follow_up_count < session->max_follow_ups_per_execution;
       ++follow_up_count) {
    turbo_agent_runtime_parent_link_t parent_link = {0};
    turbo_agent_runtime_exec_options_t exec_options = {0};
    json_value_t *record = NULL;
    json_value_t *next_summary = NULL;
    json_value_t *next_state = NULL;
    json_value_t *previous_summary;
    json_value_t *previous_state;
    const char *status = *summary_slot ? json_get_string(*summary_slot, "status") : NULL;
    int run_rc;
    int rc;

    if (!status || strcmp(status, "completed") != 0) return SALTS_OK;
    rc = turbo_agent_inbox_claim(session->inbox, TURBO_AGENT_INBOX_FOLLOW_UP, session->last_run_id,
                                 &record);
    if (rc == SALTS_ENOENT || rc == SALTS_ECANCELED) return SALTS_OK;
    if (rc != SALTS_OK || !record) return rc != SALTS_OK ? rc : SALTS_EIO;
    rc = turbo_agent_session_apply_claimed_message(session, TURBO_AGENT_INBOX_FOLLOW_UP, record,
                                                   *out_state);
    turbo_runtime_json_destroy(record);
    if (rc != SALTS_OK) return rc;

    rc = turbo_agent_session_prepare_start_options_internal(
        session, event_sink, event_sink_user_data, &parent_link, &exec_options);
    if (rc != SALTS_OK) return rc;
    run_rc = cancel_token
                 ? turbo_agent_runtime_exec_start_controlled(
                       session->runtime, graph, *out_state, options, &exec_options,
                       cancel_token, &next_summary, &next_state)
                 : turbo_agent_runtime_exec_start(session->runtime, graph, *out_state, options,
                                                  &exec_options, &next_summary, &next_state);
    if (run_rc != SALTS_OK) {
      turbo_runtime_json_destroy(next_state);
      turbo_runtime_json_destroy(next_summary);
      return run_rc;
    }

    previous_summary = *summary_slot;
    previous_state = *out_state;
    *summary_slot = next_summary;
    *out_state = next_state;
    turbo_runtime_json_destroy(previous_summary);
    turbo_runtime_json_destroy(previous_state);

    rc = turbo_agent_session_capture_summary(session, next_summary);
    if (rc == SALTS_OK) {
      rc = turbo_agent_inbox_commit_bound_claim(session->inbox);
    }
    if (rc != SALTS_OK) return rc;
  }
  return SALTS_OK;
}

static int turbo_agent_session_apply_effective_defaults(turbo_agent_config_t *config) {
  const turbo_model_provider_t *provider;

  if (!config) {
    return -1;
  }
  if (!config->model && (config->api_key || config->transport_fn || config->http_client ||
                         config->base_url || config->provider)) {
    config->model = TURBO_AGENT_DEFAULT_MODEL;
  }
  provider = turbo_agent_resolve_provider(config->provider, config->api_mode, &config->api_mode);
  config->provider = provider;
  if (!config->base_url && (config->api_key || config->http_client)) {
    config->base_url =
        turbo_model_provider_select_base_url(provider, NULL, NULL, "https://api.openai.com/v1");
  }
  return 0;
}

/* Only called while constructing an unpublished, zero-initialized Session.
 * Earlier copies remain owned by Session and are released by destroy on failure.
 * tstr_dup maps NULL to an owned empty string, preserving existing accessors. */
static int turbo_agent_session_copy_optional_config(
    turbo_agent_session_t *session, const turbo_agent_session_config_t *config) {
  const struct {
    const char *source;
    tstr *destination;
  } strings[] = {
      {config->thread_id, &session->thread_id},
      {config->memory_namespace, &session->memory_namespace},
      {config->parent_agent_run_id, &session->parent_agent_run_id},
      {config->parent_tool_call_id, &session->parent_tool_call_id},
      {config->parent_tool_name, &session->parent_tool_name},
      {config->parent_graph_run_id, &session->parent_graph_run_id},
      {config->call_frame_id, &session->call_frame_id},
      {config->knowledge_query, &session->knowledge_query},
      {config->knowledge_kind, &session->knowledge_kind},
      {config->knowledge_uri_prefix, &session->knowledge_uri_prefix},
      {config->retriever_query, &session->retriever_query},
      {config->retriever_kind, &session->retriever_kind},
      {config->retriever_uri_prefix, &session->retriever_uri_prefix},
      {config->retriever_scope, &session->retriever_scope},
  };
  size_t i;

  for (i = 0; i < sizeof(strings) / sizeof(strings[0]); ++i) {
    *strings[i].destination = tstr_dup(strings[i].source);
    if (!*strings[i].destination) return -1;
  }
  return 0;
}

CXX_C_API turbo_agent_session_t *
turbo_agent_session_create(const turbo_agent_session_config_t *config) {
  turbo_agent_session_t *session;
  turbo_agent_runtime_store_t runtime_store;
  turbo_agent_config_t effective_config = {0};
  const turbo_model_provider_t *provider;
  int create_agent;

  if (!config) {
    return NULL;
  }
  runtime_store = config->runtime_store;
  if (!runtime_store.put || !runtime_store.get || !runtime_store.list) {
    runtime_store = turbo_agent_runtime_store_memory_create();
  }
  if (!runtime_store.put || !runtime_store.get || !runtime_store.list) {
    return NULL;
  }

  session = (turbo_agent_session_t *)calloc(1, sizeof(*session));
  if (!session) {
    if (runtime_store.user_data_free) {
      runtime_store.user_data_free(runtime_store.user_data);
    }
    return NULL;
  }

  session->runtime = turbo_agent_runtime_create(&runtime_store);
  if (!session->runtime) {
    if (runtime_store.user_data_free) {
      runtime_store.user_data_free(runtime_store.user_data);
    }
    turbo_agent_session_destroy(session);
    return NULL;
  }

  effective_config = config->agent_config;
  if (turbo_agent_session_should_load_env(config)) {
    turbo_agent_config_apply_env(&effective_config, config->env_path, config->overwrite_env);
  }
  create_agent = turbo_agent_session_should_create_agent(&effective_config);

  if (create_agent) {
    turbo_agent_session_apply_effective_defaults(&effective_config);
    session->agent = turbo_agent_create(&effective_config);
    if (!session->agent) {
      turbo_agent_session_destroy(session);
      return NULL;
    }
  }

  provider = effective_config.provider;
  session->model = turbo_agent_util_strdup(create_agent ? effective_config.model : NULL);
  if (!session->model) goto failure;
  session->base_url = turbo_agent_util_strdup(create_agent ? effective_config.base_url : NULL);
  if (!session->base_url) goto failure;
  session->provider_name =
      turbo_agent_util_strdup((create_agent && provider) ? provider->name : NULL);
  if (!session->provider_name) goto failure;
  session->has_api_key =
      (create_agent && effective_config.api_key && effective_config.api_key[0] != '\0') ? 1 : 0;
  session->workflow_kind = config->workflow_kind;
  if (turbo_agent_session_copy_optional_config(session, config) != 0) goto failure;
  session->knowledge_context_config.store = config->knowledge_store;
  session->knowledge_context_config.query = session->knowledge_query;
  session->knowledge_context_config.kind = session->knowledge_kind;
  session->knowledge_context_config.uri_prefix = session->knowledge_uri_prefix;
  session->knowledge_context_config.limit = config->knowledge_limit;
  session->retriever_context_config.retriever = config->retriever;
  session->retriever_context_config.query = session->retriever_query;
  session->retriever_context_config.kind = session->retriever_kind;
  session->retriever_context_config.uri_prefix = session->retriever_uri_prefix;
  session->retriever_context_config.limit = config->retriever_limit;
  session->retriever_context_config.scope = session->retriever_scope;
  /* MemoryStore transfers only after all fallible Session construction succeeds. */
  session->memory_store = config->memory_store;
  return session;

failure:
  turbo_agent_session_destroy(session);
  return NULL;
}

CXX_C_API void turbo_agent_session_destroy(turbo_agent_session_t *session) {
  if (!session) {
    return;
  }
  if (session->agent && session->agent->before_turn_user_data == session) {
    session->agent->before_turn = NULL;
    session->agent->before_turn_user_data = NULL;
  }
  if (session->agent && session->agent->context_overflow_user_data == session) {
    session->agent->context_overflow = NULL;
    session->agent->context_overflow_user_data = NULL;
  }
  if (session->inbox) {
    (void)turbo_agent_inbox_close(session->inbox);
    turbo_agent_inbox_destroy(session->inbox);
  }
  turbo_agent_context_destroy(session->context);
  turbo_agent_destroy(session->agent);
  turbo_agent_runtime_destroy(session->runtime);
  turbo_agent_memory_store_destroy(&session->memory_store);
  tstr_free(session->thread_id);
  tstr_free(session->last_run_id);
  tstr_free(session->last_checkpoint_id);
  tstr_free(session->memory_namespace);
  tstr_free(session->parent_agent_run_id);
  tstr_free(session->parent_tool_call_id);
  tstr_free(session->parent_tool_name);
  tstr_free(session->parent_graph_run_id);
  tstr_free(session->call_frame_id);
  tstr_free(session->model);
  tstr_free(session->base_url);
  tstr_free(session->provider_name);
  tstr_free(session->knowledge_query);
  tstr_free(session->knowledge_kind);
  tstr_free(session->knowledge_uri_prefix);
  tstr_free(session->retriever_query);
  tstr_free(session->retriever_kind);
  tstr_free(session->retriever_uri_prefix);
  tstr_free(session->retriever_scope);
  free(session);
}

CXX_C_API int turbo_agent_session_inbox_configure(turbo_agent_session_t *session,
                                                  const turbo_agent_inbox_config_t *config) {
  turbo_agent_inbox_t *inbox;
  if (!session || !config || !session->runtime || !session->thread_id ||
      session->thread_id[0] == '\0') {
    return SALTS_EINVAL;
  }
  if (session->inbox) {
    return SALTS_EALREADY;
  }
  {
    int rc = turbo_agent_inbox_create(session->runtime, session->thread_id, config, &inbox);
    if (rc != SALTS_OK) {
      return rc;
    }
  }
  session->inbox = inbox;
  session->max_follow_ups_per_execution = config->max_follow_ups_per_execution;
  if (session->agent) {
    session->agent->before_turn = turbo_agent_session_apply_pending_steer;
    session->agent->before_turn_user_data = session;
  }
  return SALTS_OK;
}

int turbo_agent_session_prepare_start_options_internal(
    turbo_agent_session_t *session, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, turbo_agent_runtime_parent_link_t *parent_link,
    turbo_agent_runtime_exec_options_t *out_options) {
  if (!session || !session->runtime || !parent_link || !out_options) {
    return SALTS_EINVAL;
  }
  memset(parent_link, 0, sizeof(*parent_link));
  memset(out_options, 0, sizeof(*out_options));
  out_options->thread_id = session->thread_id;
  out_options->event_sink = event_sink;
  out_options->event_sink_user_data = event_sink_user_data;
  if (turbo_agent_session_has_parent_link(session, parent_link)) {
    out_options->parent_link = parent_link;
  }
  return SALTS_OK;
}

int turbo_agent_session_prepare_resume_options_internal(
    turbo_agent_session_t *session,
    const turbo_agent_session_exec_options_t *session_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    turbo_agent_runtime_exec_options_t *out_options) {
  if (!session || !session->runtime || !session_options || !out_options) {
    return SALTS_EINVAL;
  }
  if (session_options->scope != TURBO_SESSION_SCOPE_CHECKPOINT &&
      session_options->scope != TURBO_SESSION_SCOPE_THREAD) {
    return SALTS_EINVAL;
  }
  if (session_options->input_kind != TURBO_SESSION_INPUT_OVERRIDE &&
      session_options->input_kind != TURBO_SESSION_INPUT_PATCH &&
      session_options->input_kind != TURBO_SESSION_INPUT_COMMAND) {
    return SALTS_EINVAL;
  }

  memset(out_options, 0, sizeof(*out_options));
  out_options->scope = (turbo_agent_runtime_scope_t)session_options->scope;
  out_options->input_kind = (turbo_agent_runtime_input_kind_t)session_options->input_kind;
  if (out_options->scope == TURBO_RUNTIME_SCOPE_THREAD) {
    if (!session->thread_id || !session->thread_id[0]) {
      return SALTS_EINVAL;
    }
    out_options->thread_id = session->thread_id;
  } else {
    out_options->checkpoint_id = turbo_agent_session_resolve_checkpoint_id(
        session, session_options->checkpoint_id);
    if (!out_options->checkpoint_id || !out_options->checkpoint_id[0]) {
      return SALTS_EINVAL;
    }
  }
  out_options->event_sink = event_sink;
  out_options->event_sink_user_data = event_sink_user_data;
  return SALTS_OK;
}

int turbo_agent_session_complete_execution_internal(
    turbo_agent_session_t *session, turbo_graph_t *graph,
    const turbo_graph_run_options_t *graph_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    turbo_cancel_token_t *cancel_token, json_value_t **summary,
    json_value_t **state) {
  int rc;

  if (!session || !graph || !summary || !*summary || !state || !*state) {
    return SALTS_EINVAL;
  }
  rc = turbo_agent_session_capture_summary(session, *summary);
  if (rc == SALTS_OK && session->inbox) {
    rc = turbo_agent_inbox_commit_bound_claim(session->inbox);
  }
  if (rc == SALTS_OK && session->inbox) {
    rc = turbo_agent_session_run_follow_ups(
        session, graph, graph_options, event_sink, event_sink_user_data,
        cancel_token, summary, state);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_context_configure(
    turbo_agent_session_t *session, const turbo_agent_context_policy_t *policy) {
  turbo_agent_context_t *context;
  int rc;

  if (!session || !policy || !session->runtime || !session->agent || !session->thread_id ||
      session->thread_id[0] == '\0') {
    return SALTS_EINVAL;
  }
  if (session->context) return SALTS_EALREADY;
  rc = turbo_agent_context_create(session->runtime, session->agent, session->thread_id, policy,
                                  &context);
  if (rc != SALTS_OK) return rc;
  session->context = context;
  session->agent->before_turn = turbo_agent_session_apply_pending_steer;
  session->agent->before_turn_user_data = session;
  session->agent->context_overflow = turbo_agent_session_handle_context_overflow;
  session->agent->context_overflow_user_data = session;
  return SALTS_OK;
}

CXX_C_API int turbo_agent_session_context_compact(turbo_agent_session_t *session,
                                                  const json_value_t *state) {
  if (!session || !session->context || !state) return SALTS_EINVAL;
  return turbo_agent_context_prepare(session->context, (json_value_t *)state, 1);
}

CXX_C_API int turbo_agent_session_context_status(turbo_agent_session_t *session,
                                                 json_value_t **out_status) {
  if (!session || !session->context) {
    if (out_status) *out_status = NULL;
    return SALTS_EINVAL;
  }
  return turbo_agent_context_status(session->context, out_status);
}

CXX_C_API int turbo_agent_session_enqueue(turbo_agent_session_t *session,
                                          turbo_agent_inbox_kind_t kind,
                                          const json_value_t *message, uint64_t timeout_ms,
                                          char **out_inbox_id) {
  if (!session || !session->inbox) {
    if (out_inbox_id) {
      *out_inbox_id = NULL;
    }
    return SALTS_EINVAL;
  }
  return turbo_agent_inbox_enqueue(session->inbox, kind, message, timeout_ms, out_inbox_id);
}

CXX_C_API int turbo_agent_session_inbox_status(turbo_agent_session_t *session, const char *inbox_id,
                                               json_value_t **out_status) {
  if (!session || !session->inbox) {
    if (out_status) {
      *out_status = NULL;
    }
    return SALTS_EINVAL;
  }
  return turbo_agent_inbox_status(session->inbox, inbox_id, out_status);
}

CXX_C_API int turbo_agent_session_inbox_claim(turbo_agent_session_t *session,
                                              turbo_agent_inbox_kind_t kind, const char *run_id,
                                              json_value_t **out_record) {
  if (!session || !session->inbox) {
    if (out_record) {
      *out_record = NULL;
    }
    return SALTS_EINVAL;
  }
  return turbo_agent_inbox_claim(session->inbox, kind, run_id, out_record);
}

CXX_C_API int turbo_agent_session_inbox_bind_applied_event(turbo_agent_session_t *session,
                                                            const char *inbox_id,
                                                            const char *applied_event_id) {
  return session && session->inbox
             ? turbo_agent_inbox_bind_applied_event(session->inbox, inbox_id, applied_event_id)
             : SALTS_EINVAL;
}

CXX_C_API int turbo_agent_session_inbox_mark_applied(turbo_agent_session_t *session,
                                                     const char *inbox_id,
                                                     const char *applied_event_id) {
  return session && session->inbox
             ? turbo_agent_inbox_mark_applied(session->inbox, inbox_id, applied_event_id)
             : SALTS_EINVAL;
}

CXX_C_API int turbo_agent_session_inbox_requeue(turbo_agent_session_t *session,
                                                const char *inbox_id) {
  return session && session->inbox ? turbo_agent_inbox_requeue(session->inbox, inbox_id)
                                   : SALTS_EINVAL;
}

CXX_C_API int turbo_agent_session_inbox_close(turbo_agent_session_t *session) {
  return session && session->inbox ? turbo_agent_inbox_close(session->inbox) : SALTS_EINVAL;
}

CXX_C_API turbo_agent_t *turbo_agent_session_agent(const turbo_agent_session_t *session) {
  return session ? session->agent : NULL;
}

CXX_C_API turbo_agent_runtime_t *turbo_agent_session_runtime(const turbo_agent_session_t *session) {
  return session ? session->runtime : NULL;
}

CXX_C_API const turbo_agent_memory_store_t *
turbo_agent_session_memory_store(const turbo_agent_session_t *session) {
  if (!session) {
    return NULL;
  }
  if (!session->memory_store.get && !session->memory_store.put && !session->memory_store.list &&
      !session->memory_store.query && !session->memory_store.remove) {
    return NULL;
  }
  return &session->memory_store;
}

CXX_C_API const turbo_tool_registry_t *
turbo_agent_session_tool_registry(const turbo_agent_session_t *session) {
  return session ? turbo_agent_tool_registry(session->agent) : NULL;
}

CXX_C_API int turbo_agent_session_tool_executor_configure(
    turbo_agent_session_t *session,
    const turbo_agent_tool_executor_config_t *config) {
  if (!session || !session->agent) return SALTS_EINVAL;
  return turbo_agent_tool_executor_configure(session->agent, config);
}

CXX_C_API size_t turbo_agent_session_tool_count(const turbo_agent_session_t *session) {
  return turbo_agent_tool_count(session ? session->agent : NULL);
}

CXX_C_API const char *turbo_agent_session_model(const turbo_agent_session_t *session) {
  return session ? session->model : NULL;
}

CXX_C_API const char *turbo_agent_session_base_url(const turbo_agent_session_t *session) {
  return session ? session->base_url : NULL;
}

CXX_C_API const char *turbo_agent_session_provider_name(const turbo_agent_session_t *session) {
  return session ? session->provider_name : NULL;
}

CXX_C_API int turbo_agent_session_has_api_key(const turbo_agent_session_t *session) {
  return session ? session->has_api_key : 0;
}

CXX_C_API const char *turbo_agent_session_thread_id(const turbo_agent_session_t *session) {
  return session ? session->thread_id : NULL;
}

CXX_C_API const char *turbo_agent_session_last_run_id(const turbo_agent_session_t *session) {
  return session ? session->last_run_id : NULL;
}

CXX_C_API const char *turbo_agent_session_last_checkpoint_id(const turbo_agent_session_t *session) {
  return session ? session->last_checkpoint_id : NULL;
}

CXX_C_API turbo_agent_session_workflow_kind_t
turbo_agent_session_workflow_kind(const turbo_agent_session_t *session) {
  return session ? session->workflow_kind : TURBO_AGENT_SESSION_WORKFLOW_LOOP;
}

CXX_C_API const char *turbo_agent_session_memory_namespace(const turbo_agent_session_t *session) {
  return session ? session->memory_namespace : NULL;
}

int turbo_agent_session_diagnostic_view_internal(
    const turbo_agent_session_t *session, turbo_agent_session_diagnostic_view_t *out_view) {
  if (!session || !out_view) return -1;
  *out_view = (turbo_agent_session_diagnostic_view_t){
      .has_agent = session->agent != NULL,
      .has_runtime = session->runtime != NULL,
      .has_memory_store = turbo_agent_session_memory_store(session) != NULL,
      .registry = turbo_agent_session_tool_registry(session),
      .tool_count = turbo_agent_session_tool_count(session),
      .workflow_kind = session->workflow_kind,
      .memory_namespace = session->memory_namespace,
      .model = session->model,
      .base_url = session->base_url,
      .provider_name = session->provider_name,
      .has_api_key = session->has_api_key,
      .has_knowledge_store = session->knowledge_context_config.store != NULL,
      .has_retriever = session->retriever_context_config.retriever != NULL,
      .structured_output_name = session->agent ? session->agent->structured_output_name : NULL,
      .structured_output_schema_json = session->agent ? session->agent->structured_output_schema_json : NULL,
      .structured_output_strict = session->agent ? session->agent->structured_output_strict : 0,
  };
  return 0;
}

CXX_C_API int turbo_agent_session_list_child_runs(turbo_agent_session_t *session,
                                                  const char *parent_agent_run_id,
                                                  json_value_t **out_runs_json) {
  const char *resolved_parent_agent_run_id =
      turbo_agent_session_resolve_parent_agent_run_id(session, parent_agent_run_id);

  if (!session || !session->runtime || !resolved_parent_agent_run_id || !out_runs_json) {
    return -1;
  }
  return turbo_agent_runtime_list_child_runs(session->runtime, resolved_parent_agent_run_id,
                                             out_runs_json);
}

const turbo_agent_knowledge_context_config_t *
turbo_agent_session_knowledge_context_internal(const turbo_agent_session_t *session) {
  return session ? &session->knowledge_context_config : NULL;
}

const turbo_retriever_context_config_t *
turbo_agent_session_retriever_context_internal(const turbo_agent_session_t *session) {
  return session ? &session->retriever_context_config : NULL;
}
