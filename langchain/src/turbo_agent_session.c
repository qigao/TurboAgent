#include "turbo_agent_session.h"

#include "turbo_agent_core_internal.h"
#include "turbo_agent_context_internal.h"
#include "turbo_agent_event_internal.h"
#include "turbo_agent_inbox_internal.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_session_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"
#include "turbo_agent_workflow.h"
#include "turbo_model_provider.h"
#include <json_parser.h>
#include "turbo_prompt.h"
#include "turbo_retriever.h"
#include "turbo_tool_schema.h"

#include <salts_uuid.h>

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
  turbo_agent_knowledge_store_t *knowledge_store;
  tstr knowledge_query;
  tstr knowledge_kind;
  tstr knowledge_uri_prefix;
  size_t knowledge_limit;
  turbo_agent_knowledge_context_config_t knowledge_context_config;
  turbo_retriever_t *retriever;
  tstr retriever_query;
  tstr retriever_kind;
  tstr retriever_uri_prefix;
  size_t retriever_limit;
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
  salts_uuid_t uuid;

  if (!session || !session->inbox || !record || !state) return -1;
  payload = turbo_json_object_get(record, "payload");
  inbox_id = turbo_json_get_string(record, "inbox_id");
  content = payload ? turbo_json_get_string(payload, "content") : NULL;
  if (!inbox_id || !content || content[0] == '\0' || salts_uuid_v7_generate(&uuid) != SALTS_OK ||
      salts_uuid_format(&uuid, event_id, sizeof(event_id)) != SALTS_OK) {
    (void)turbo_agent_inbox_requeue(session->inbox, inbox_id ? inbox_id : "");
    return -1;
  }
  event = turbo_agent_event_create("inbox_message");
  if (!event) {
    (void)turbo_agent_inbox_requeue(session->inbox, inbox_id);
    return -1;
  }
  turbo_json_object_set_string(event, "event_id", event_id);
  turbo_json_object_set_string(event, "inbox_id", inbox_id);
  turbo_json_object_set_string(event, "inbox_kind",
                               kind == TURBO_AGENT_INBOX_STEER ? "steer" : "follow_up");
  turbo_json_object_set_string(event, "content", content);
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

typedef struct turbo_agent_session_observer_bridge_s {
  turbo_agent_observer_json_value_sink_t sink;
} turbo_agent_session_observer_bridge_t;

static int turbo_agent_session_should_load_env(const turbo_agent_session_config_t *config) {
  return config && config->load_env ? 1 : 0;
}

static int turbo_agent_session_should_create_agent(const turbo_agent_config_t *config) {
  return config && (config->model || config->api_key || config->base_url || config->endpoint_path ||
                    config->instructions || config->structured_output_name ||
                    config->structured_output_schema_json || config->http_client ||
                    config->transport_fn || config->provider || config->tool_registry);
}

static void turbo_agent_session_replace_string(tstr *slot, const char *text) {
  tstr copy = turbo_agent_util_strdup(text);

  if (!slot) {
    tstr_free(copy);
    return;
  }
  tstr_free(*slot);
  *slot = copy;
}

static const char *
turbo_agent_session_workflow_kind_name(turbo_agent_session_workflow_kind_t kind) {
  switch (kind) {
  case TURBO_AGENT_SESSION_WORKFLOW_LOOP:
    return "loop";
  case TURBO_AGENT_SESSION_WORKFLOW_REVIEW:
    return "review";
  case TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING:
    return "engineering";
  case TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING:
    return "knowledge_engineering";
  case TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING:
    return "retriever_engineering";
  default:
    return "unknown";
  }
}

static void turbo_agent_session_json_set_optional_string(json_value_t *object, const char *key,
                                                         const char *value) {
  if (!object || !key) {
    return;
  }
  if (value && value[0] != '\0') {
    turbo_json_object_set_string(object, key, value);
  } else {
    turbo_json_object_set_null(object, key);
  }
}

static int turbo_agent_session_json_object_take(json_value_t *object, const char *key,
                                                json_value_t **value) {
  if (!object || !key || !value || !*value) {
    return -1;
  }
  turbo_json_object_add(object, key, *value);
  *value = NULL;
  return 0;
}

static int turbo_agent_session_json_array_add_string(json_value_t *array, const char *value) {
  json_value_t *string_value;

  if (!array || !value) {
    return -1;
  }
  string_value = turbo_json_create_string(value);
  if (!string_value) {
    return -1;
  }
  turbo_json_array_add(array, string_value);
  return 0;
}

static const char *
turbo_agent_session_selected_tool_schema_format(const turbo_agent_session_t *session) {
  const char *provider_name = session ? session->provider_name : NULL;

  if (!provider_name || provider_name[0] == '\0') {
    return "openai_responses";
  }
  if (strcmp(provider_name, "openai_compatible_chat_completions") == 0 ||
      strcmp(provider_name, "openai_compatible_chat") == 0) {
    return "openai_compatible_chat";
  }
  if (strcmp(provider_name, "openai_chat_completions") == 0 ||
      strcmp(provider_name, "openai_chat") == 0) {
    return "openai_chat";
  }
  if (strcmp(provider_name, "anthropic_messages") == 0 || strcmp(provider_name, "anthropic") == 0) {
    return "anthropic";
  }
  return "openai_responses";
}

static json_value_t *
turbo_agent_session_build_tool_schema_format(const turbo_tool_registry_t *registry,
                                             const char *format) {
  if (!format) {
    return NULL;
  }
  if (strcmp(format, "openai_responses") == 0) {
    return turbo_tool_schema_build_openai_tools(registry);
  }
  if (strcmp(format, "openai_chat") == 0) {
    return turbo_tool_schema_build_openai_chat_tools(registry);
  }
  if (strcmp(format, "openai_compatible_chat") == 0) {
    return turbo_tool_schema_build_openai_compatible_chat_tools(registry);
  }
  if (strcmp(format, "anthropic") == 0) {
    return turbo_tool_schema_build_anthropic_tools(registry);
  }
  return NULL;
}

static int turbo_agent_session_add_tool_schema_format_status(json_value_t *status,
                                                             const char *format, int ok,
                                                             int selected) {
  json_value_t *entry;

  if (!status || !format) {
    return -1;
  }
  entry = turbo_json_create_object();
  if (!entry) {
    return -1;
  }
  turbo_json_object_set_bool(entry, "ok", ok ? true : false);
  turbo_json_object_set_bool(entry, "selected", selected ? true : false);
  if (!ok) {
    turbo_json_object_set_string(entry, "reason", "invalid_tool_schema");
  } else {
    turbo_json_object_set_null(entry, "reason");
  }
  turbo_json_object_add(status, format, entry);
  return 0;
}

static int turbo_agent_session_get_tool_schema_status(const turbo_agent_session_t *session,
                                                      json_value_t **out_status_json,
                                                      int *out_selected_ok) {
  static const char *formats[] = {"openai_responses", "openai_chat", "openai_compatible_chat",
                                  "anthropic"};
  const turbo_tool_registry_t *registry;
  const char *selected_format;
  json_value_t *status = NULL;
  size_t i;
  int selected_ok = 0;
  int rc = -1;

  if (!session || !out_status_json || !out_selected_ok) {
    return -1;
  }
  *out_status_json = NULL;
  *out_selected_ok = 0;

  registry = turbo_agent_session_tool_registry(session);
  selected_format = turbo_agent_session_selected_tool_schema_format(session);
  status = turbo_json_create_object();
  if (!status) {
    return -1;
  }

  turbo_json_object_set_string(status, "selected_format", selected_format);
  for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    json_value_t *schema = turbo_agent_session_build_tool_schema_format(registry, formats[i]);
    int format_ok = schema ? 1 : 0;
    int selected = strcmp(selected_format, formats[i]) == 0;

    if (selected) {
      selected_ok = format_ok;
    }
    turbo_free_json(&schema);
    if (turbo_agent_session_add_tool_schema_format_status(status, formats[i], format_ok,
                                                          selected) != 0) {
      goto cleanup;
    }
  }

  *out_status_json = status;
  *out_selected_ok = selected_ok;
  status = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&status);
  return rc;
}

static int turbo_agent_session_get_workflow_diagnostics(const turbo_agent_session_t *session,
                                                        json_value_t **out_workflow_json,
                                                        const char **out_error_code) {
  json_value_t *workflow = NULL;
  const char *error_code = NULL;
  int requires_knowledge_store = 0;
  int requires_retriever = 0;
  int ready = 1;

  if (!session || !out_workflow_json || !out_error_code) {
    return -1;
  }
  *out_workflow_json = NULL;
  *out_error_code = NULL;

  workflow = turbo_json_create_object();
  if (!workflow) {
    return -1;
  }

  switch (session->workflow_kind) {
  case TURBO_AGENT_SESSION_WORKFLOW_LOOP:
  case TURBO_AGENT_SESSION_WORKFLOW_REVIEW:
  case TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING:
    break;
  case TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING:
    requires_knowledge_store = 1;
    break;
  case TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING:
    requires_retriever = 1;
    break;
  default:
    ready = 0;
    error_code = "invalid_workflow_kind";
    break;
  }

  if (ready && !session->runtime) {
    ready = 0;
    error_code = "missing_runtime";
  }
  if (ready && !session->agent) {
    ready = 0;
    error_code = "missing_agent";
  }
  if (ready && requires_knowledge_store && !session->knowledge_store) {
    ready = 0;
    error_code = "missing_knowledge_store";
  }
  if (ready && requires_retriever && !session->retriever) {
    ready = 0;
    error_code = "missing_retriever";
  }

  turbo_json_object_set_bool(workflow, "ok", ready ? true : false);
  turbo_json_object_set_number(workflow, "workflow_kind_id", (double)session->workflow_kind);
  turbo_json_object_set_string(workflow, "workflow_kind",
                               turbo_agent_session_workflow_kind_name(session->workflow_kind));
  turbo_json_object_set_bool(workflow, "requires_agent", true);
  turbo_json_object_set_bool(workflow, "requires_runtime", true);
  turbo_json_object_set_bool(workflow, "requires_knowledge_store",
                             requires_knowledge_store ? true : false);
  turbo_json_object_set_bool(workflow, "requires_retriever", requires_retriever ? true : false);
  if (error_code) {
    turbo_json_object_set_string(workflow, "reason", error_code);
  } else {
    turbo_json_object_set_null(workflow, "reason");
  }

  *out_workflow_json = workflow;
  *out_error_code = error_code;
  return 0;
}

static int
turbo_agent_session_get_structured_output_diagnostics(const turbo_agent_session_t *session,
                                                      json_value_t **out_structured_output_json,
                                                      const char **out_error_code) {
  json_value_t *diagnostics = NULL;
  json_value_t *schema = NULL;
  const char *schema_json;
  const char *error_code = NULL;
  int configured;

  if (!session || !out_structured_output_json || !out_error_code) {
    return -1;
  }
  *out_structured_output_json = NULL;
  *out_error_code = NULL;

  diagnostics = turbo_json_create_object();
  if (!diagnostics) {
    return -1;
  }

  schema_json = (session->agent && session->agent->structured_output_schema_json)
                    ? session->agent->structured_output_schema_json
                    : NULL;
  configured = schema_json && schema_json[0] != '\0';
  turbo_json_object_set_bool(diagnostics, "configured", configured ? true : false);

  if (configured &&
      (turbo_parse_json((const uint8_t *)schema_json, strlen(schema_json), &schema) != 0 ||
       !schema || turbo_json_type(schema) != TURBO_JSON_OBJECT)) {
    error_code = "invalid_structured_output_schema";
  }

  turbo_json_object_set_bool(diagnostics, "ok", error_code ? false : true);
  if (session->agent && session->agent->structured_output_name &&
      session->agent->structured_output_name[0] != '\0') {
    turbo_json_object_set_string(diagnostics, "name", session->agent->structured_output_name);
  } else {
    turbo_json_object_set_null(diagnostics, "name");
  }
  turbo_json_object_set_bool(diagnostics, "strict",
                             (session->agent && session->agent->structured_output_strict) ? true
                                                                                          : false);
  if (error_code) {
    turbo_json_object_set_string(diagnostics, "reason", error_code);
  } else {
    turbo_json_object_set_null(diagnostics, "reason");
  }

  turbo_free_json(&schema);
  *out_structured_output_json = diagnostics;
  *out_error_code = error_code;
  return 0;
}

static int turbo_agent_session_get_supervisor_array_json_local(
    turbo_agent_session_t *session, const json_value_t *(*selector)(const json_value_t *state),
    json_value_t **out_array_json) {
  json_value_t *state_json_value = NULL;
  json_value_t *state_json = NULL;
  json_value_t *array_json = NULL;
  const json_value_t *selected = NULL;
  int rc = -1;

  if (!session || !selector || !out_array_json) {
    return -1;
  }
  *out_array_json = NULL;

  if (turbo_agent_session_get_thread_state_json_value(session, &state_json_value) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  state_json = turbo_json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }

  selected = selector(state_json);
  if (selected && turbo_json_type(selected) == TURBO_JSON_ARRAY) {
    array_json = turbo_json_clone(selected);
  } else {
    array_json = turbo_json_create_array();
  }
  if (!array_json) {
    goto cleanup;
  }

  *out_array_json = array_json;
  array_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&array_json);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(state_json_value);
  return rc;
}

static int turbo_agent_session_build_supervisor_inbox_override_local(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    json_value_t **out_state_override) {
  json_value_t *state_json_value = NULL;
  json_value_t *override_json_value = NULL;
  json_value_t *state_json = NULL;
  int rc = -1;

  if (!session || !text || !out_state_override) {
    return -1;
  }
  *out_state_override = NULL;

  if (turbo_agent_session_get_thread_state_json_value(session, &state_json_value) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  state_json = turbo_json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }
  if (turbo_agent_state_append_supervisor_inbox_message(state_json, source_agent, text) != 0) {
    goto cleanup;
  }
  override_json_value = turbo_json_clone(state_json);
  if (!override_json_value) {
    goto cleanup;
  }

  *out_state_override = override_json_value;
  override_json_value = NULL;
  rc = 0;

cleanup:
  turbo_runtime_json_destroy(override_json_value);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(state_json_value);
  return rc;
}

static int turbo_agent_session_build_supervisor_inspect_local(turbo_agent_session_t *session,
                                                              json_value_t **out_inspect_json) {
  json_value_t *state_json_value = NULL;
  json_value_t *control_json_value = NULL;
  json_value_t *workflow_json_value = NULL;
  json_value_t *state_json = NULL;
  json_value_t *control_json = NULL;
  json_value_t *workflow_json = NULL;
  json_value_t *inspect_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *latest_handoff_event_json = NULL;
  json_value_t *supervisor_json = NULL;
  const json_value_t *state_inbox = NULL;
  const json_value_t *state_history = NULL;
  const json_value_t *state_latest_handoff_event = NULL;
  const json_value_t *control_supervisor = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_thread_state_json_value(session, &state_json_value) != 0 ||
      !state_json_value) {
    goto cleanup;
  }
  state_json = turbo_json_clone(state_json_value);
  if (!state_json) {
    goto cleanup;
  }
  control_json_value = turbo_agent_state_control_snapshot_json_value(state_json_value);
  workflow_json_value = turbo_agent_state_workflow_snapshot_json_value(state_json_value);
  if (!control_json_value || !workflow_json_value) {
    goto cleanup;
  }
  control_json = turbo_json_clone(control_json_value);
  workflow_json = turbo_json_clone(workflow_json_value);
  if (!control_json || !workflow_json) {
    goto cleanup;
  }

  state_inbox = turbo_agent_state_supervisor_inbox(state_json);
  state_history = turbo_agent_state_supervisor_handoff_history(state_json);
  state_latest_handoff_event = turbo_agent_state_latest_handoff_event(state_json);
  control_supervisor = turbo_json_object_get(control_json, "supervisor");

  inbox_json = state_inbox && turbo_json_type(state_inbox) == TURBO_JSON_ARRAY
                   ? turbo_json_clone(state_inbox)
                   : turbo_json_create_array();
  history_json = state_history && turbo_json_type(state_history) == TURBO_JSON_ARRAY
                     ? turbo_json_clone(state_history)
                     : turbo_json_create_array();
  latest_handoff_event_json =
      state_latest_handoff_event && turbo_json_type(state_latest_handoff_event) == TURBO_JSON_OBJECT
          ? turbo_json_clone(state_latest_handoff_event)
          : NULL;
  supervisor_json = control_supervisor && turbo_json_type(control_supervisor) == TURBO_JSON_OBJECT
                        ? turbo_json_clone(control_supervisor)
                        : turbo_json_create_object();
  inspect_json = turbo_json_create_object();
  if (!inbox_json || !history_json || !supervisor_json || !inspect_json ||
      (state_latest_handoff_event && !latest_handoff_event_json)) {
    goto cleanup;
  }

  turbo_json_object_add(inspect_json, "supervisor", supervisor_json);
  supervisor_json = NULL;
  turbo_json_object_add(inspect_json, "inbox", inbox_json);
  inbox_json = NULL;
  turbo_json_object_add(inspect_json, "handoff_history", history_json);
  history_json = NULL;
  if (latest_handoff_event_json) {
    turbo_json_object_add(inspect_json, "latest_handoff_event", latest_handoff_event_json);
    latest_handoff_event_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_handoff_event");
  }
  turbo_json_object_add(inspect_json, "control_snapshot", control_json);
  control_json = NULL;
  turbo_json_object_add(inspect_json, "workflow_snapshot", workflow_json);
  workflow_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&supervisor_json);
  turbo_free_json(&latest_handoff_event_json);
  turbo_free_json(&history_json);
  turbo_free_json(&inbox_json);
  turbo_free_json(&workflow_json);
  turbo_free_json(&control_json);
  turbo_free_json(&state_json);
  turbo_runtime_json_destroy(workflow_json_value);
  turbo_runtime_json_destroy(control_json_value);
  turbo_runtime_json_destroy(state_json_value);
  return rc;
}

static int turbo_agent_session_build_orchestration_inspect_local(turbo_agent_session_t *session,
                                                                 json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *child_runs_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *thread_timeline_json_value = NULL;
  int rc = -1;

  if (!session || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_thread_timeline_json_value(session, &thread_timeline_json_value) ==
          0 &&
      thread_timeline_json_value) {
    thread_timeline_json = turbo_json_clone(thread_timeline_json_value);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_list_thread_lineage(session, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_branch_tree(session, &branch_tree_json) != 0 || !branch_tree_json) {
    goto cleanup;
  }
  if (session->runtime && session->last_run_id && session->last_run_id[0] != '\0') {
    if (turbo_agent_runtime_list_child_runs(session->runtime, session->last_run_id,
                                            &child_runs_json) != 0) {
      goto cleanup;
    }
  } else {
    child_runs_json = turbo_json_create_array();
  }
  if (!child_runs_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  turbo_json_object_add(inspect_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  turbo_json_object_add(inspect_json, "child_runs", child_runs_json);
  child_runs_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_runs_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_free_json(&supervisor_inspect_json);
  return rc;
}

static int turbo_agent_session_observer_event_from_trace(const json_value_t *trace_event,
                                                         json_value_t **out_observer_event) {
  const char *observer_type;
  const char *name;
  json_value_t *observer_event = NULL;

  if (!trace_event || !out_observer_event) {
    return -1;
  }
  *out_observer_event = NULL;

  name = turbo_runtime_json_value_as_string(turbo_json_object_get(trace_event, "name"));
  observer_type = turbo_agent_util_observer_type_for_trace_name(name);
  if (!observer_type) {
    return 0;
  }

  observer_event = turbo_json_create_object();
  if (!observer_event ||
      turbo_agent_util_json_value_object_set_string(observer_event, "kind", "observer") != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "type", observer_type) != 0 ||
      turbo_agent_util_json_value_object_set_clone(observer_event, "event", trace_event) != 0 ||
      turbo_agent_util_json_value_object_set_string(observer_event, "name", name) != 0 ||
      turbo_agent_util_json_value_object_set_string(
          observer_event, "detail",
          turbo_runtime_json_value_as_string(turbo_json_object_get(trace_event, "detail"))) != 0 ||
      turbo_agent_util_json_value_object_set_string(
          observer_event, "payload",
          turbo_runtime_json_value_as_string(turbo_json_object_get(trace_event, "payload"))) != 0 ||
      turbo_agent_util_json_value_object_set_int64(
          observer_event, "status",
          turbo_runtime_json_value_as_int64(turbo_json_object_get(trace_event, "status"), 0)) !=
          0) {
    turbo_runtime_json_destroy(observer_event);
    return -1;
  }

  *out_observer_event = observer_event;
  return 1;
}

static void turbo_agent_session_observer_bridge_free(void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;

  if (!bridge) {
    return;
  }
  if (bridge->sink.user_data_free) {
    bridge->sink.user_data_free(bridge->sink.user_data);
  }
  free(bridge);
}

static void turbo_agent_session_capture_observer_event(turbo_agent_t *agent_unused,
                                                       const json_value_t *event, void *user_data) {
  turbo_agent_session_observer_bridge_t *bridge =
      (turbo_agent_session_observer_bridge_t *)user_data;
  json_value_t *observer_event = NULL;
  int rc;

  (void)agent_unused;
  if (!bridge || !bridge->sink.callback || !event) {
    return;
  }
  rc = turbo_agent_session_observer_event_from_trace(event, &observer_event);
  if (rc <= 0 || !observer_event) {
    turbo_runtime_json_destroy(observer_event);
    return;
  }
  bridge->sink.callback(observer_event, bridge->sink.user_data);
  turbo_runtime_json_destroy(observer_event);
}

static int turbo_agent_session_load_memory_context_record(json_value_t *state,
                                                          const json_value_t *record) {
  const char *kind;
  const json_value_t *metadata;
  const json_value_t *path_value;
  const char *scope;
  const char *path = NULL;
  const char *text;

  if (!state || !record || turbo_json_type(record) != TURBO_JSON_OBJECT) {
    return -1;
  }
  kind = turbo_json_get_string(record, "kind");
  if (!kind || strcmp(kind, "context") != 0) {
    return 0;
  }
  if (turbo_agent_memory_validate_record(record) != 0) {
    return -1;
  }
  metadata = turbo_json_object_get(record, "metadata");
  if (!metadata || turbo_json_type(metadata) != TURBO_JSON_OBJECT) {
    return -1;
  }
  scope = turbo_json_get_string(metadata, "scope");
  text = turbo_json_get_string(record, "text");
  path_value = turbo_json_object_get(metadata, "path");
  if (!scope || scope[0] == '\0' || !text || text[0] == '\0' || !path_value) {
    return -1;
  }
  if (turbo_json_type(path_value) == TURBO_JSON_STRING) {
    path = turbo_json_get_string(metadata, "path");
  } else if (turbo_json_type(path_value) != TURBO_JSON_NULL) {
    return -1;
  }
  return turbo_agent_state_add_memory_context_layer(state, scope, path, text);
}

static int turbo_agent_session_capture_summary(turbo_agent_session_t *session,
                                               const json_value_t *summary) {
  const char *thread_id;
  const char *run_id;
  json_value_t *checkpoint_value;
  const char *checkpoint_id = NULL;

  if (!session || !summary) {
    return -1;
  }
  thread_id = turbo_json_get_string(summary, "thread_id");
  run_id = turbo_json_get_string(summary, "run_id");
  checkpoint_value = turbo_json_object_get(summary, "checkpoint_id");
  if (!thread_id || !run_id) {
    return -1;
  }
  if (checkpoint_value && turbo_json_type(checkpoint_value) == TURBO_JSON_STRING) {
    checkpoint_id = turbo_json_get_string(summary, "checkpoint_id");
  }

  turbo_agent_session_replace_string(&session->thread_id, thread_id);
  turbo_agent_session_replace_string(&session->last_run_id, run_id);
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    turbo_agent_session_replace_string(&session->last_checkpoint_id, checkpoint_id);
  }
  return 0;
}

static const char *turbo_agent_session_resolve_checkpoint_id(const turbo_agent_session_t *session,
                                                             const char *checkpoint_id) {
  if (checkpoint_id && checkpoint_id[0] != '\0') {
    return checkpoint_id;
  }
  return session ? session->last_checkpoint_id : NULL;
}

static const char *turbo_agent_session_resolve_run_id(const turbo_agent_session_t *session,
                                                      const char *run_id) {
  if (run_id && run_id[0] != '\0') {
    return run_id;
  }
  return session ? session->last_run_id : NULL;
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
    const char *status = *summary_slot ? turbo_json_get_string(*summary_slot, "status") : NULL;
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

    exec_options.thread_id = session->thread_id;
    exec_options.event_sink = event_sink;
    exec_options.event_sink_user_data = event_sink_user_data;
    if (turbo_agent_session_has_parent_link(session, &parent_link)) {
      exec_options.parent_link = &parent_link;
    }
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

static const char *turbo_agent_session_output_item_child_run_id(const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_run_id(output_item);
}

static const char *
turbo_agent_session_output_item_child_checkpoint_id(const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_checkpoint_id(output_item);
}

static const char *
turbo_agent_session_output_item_child_thread_id(const json_value_t *output_item) {
  return turbo_agent_state_tool_result_child_thread_id(output_item);
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
  provider = config->provider ? config->provider : turbo_model_provider_openai_responses();
  config->provider = provider;
  if (!config->base_url && (config->api_key || config->http_client)) {
    config->base_url =
        turbo_model_provider_select_base_url(provider, NULL, NULL, "https://api.openai.com/v1");
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
  session->base_url = turbo_agent_util_strdup(create_agent ? effective_config.base_url : NULL);
  session->provider_name =
      turbo_agent_util_strdup((create_agent && provider) ? provider->name : NULL);
  session->has_api_key =
      (create_agent && effective_config.api_key && effective_config.api_key[0] != '\0') ? 1 : 0;
  session->memory_store = config->memory_store;
  session->workflow_kind = config->workflow_kind;
  session->thread_id = turbo_agent_util_strdup(
      (config->thread_id && config->thread_id[0] != '\0') ? config->thread_id : NULL);
  session->memory_namespace = turbo_agent_util_strdup(
      (config->memory_namespace && config->memory_namespace[0] != '\0') ? config->memory_namespace
                                                                        : NULL);
  session->parent_agent_run_id = turbo_agent_util_strdup(
      (config->parent_agent_run_id && config->parent_agent_run_id[0] != '\0')
          ? config->parent_agent_run_id
          : NULL);
  session->parent_tool_call_id = turbo_agent_util_strdup(
      (config->parent_tool_call_id && config->parent_tool_call_id[0] != '\0')
          ? config->parent_tool_call_id
          : NULL);
  session->parent_tool_name = turbo_agent_util_strdup(
      (config->parent_tool_name && config->parent_tool_name[0] != '\0') ? config->parent_tool_name
                                                                        : NULL);
  session->parent_graph_run_id = turbo_agent_util_strdup(
      (config->parent_graph_run_id && config->parent_graph_run_id[0] != '\0')
          ? config->parent_graph_run_id
          : NULL);
  session->call_frame_id = turbo_agent_util_strdup(
      (config->call_frame_id && config->call_frame_id[0] != '\0') ? config->call_frame_id : NULL);
  session->knowledge_store = config->knowledge_store;
  session->knowledge_query = turbo_agent_util_strdup(
      (config->knowledge_query && config->knowledge_query[0] != '\0') ? config->knowledge_query
                                                                      : NULL);
  session->knowledge_kind = turbo_agent_util_strdup(
      (config->knowledge_kind && config->knowledge_kind[0] != '\0') ? config->knowledge_kind
                                                                    : NULL);
  session->knowledge_uri_prefix = turbo_agent_util_strdup(
      (config->knowledge_uri_prefix && config->knowledge_uri_prefix[0] != '\0')
          ? config->knowledge_uri_prefix
          : NULL);
  session->knowledge_limit = config->knowledge_limit;
  session->knowledge_context_config.store = session->knowledge_store;
  session->knowledge_context_config.query = session->knowledge_query;
  session->knowledge_context_config.kind = session->knowledge_kind;
  session->knowledge_context_config.uri_prefix = session->knowledge_uri_prefix;
  session->knowledge_context_config.limit = session->knowledge_limit;
  session->retriever = config->retriever;
  session->retriever_query = turbo_agent_util_strdup(
      (config->retriever_query && config->retriever_query[0] != '\0') ? config->retriever_query
                                                                      : NULL);
  session->retriever_kind = turbo_agent_util_strdup(
      (config->retriever_kind && config->retriever_kind[0] != '\0') ? config->retriever_kind
                                                                    : NULL);
  session->retriever_uri_prefix = turbo_agent_util_strdup(
      (config->retriever_uri_prefix && config->retriever_uri_prefix[0] != '\0')
          ? config->retriever_uri_prefix
          : NULL);
  session->retriever_limit = config->retriever_limit;
  session->retriever_scope = turbo_agent_util_strdup(
      (config->retriever_scope && config->retriever_scope[0] != '\0') ? config->retriever_scope
                                                                      : NULL);
  session->retriever_context_config.retriever = session->retriever;
  session->retriever_context_config.query = session->retriever_query;
  session->retriever_context_config.kind = session->retriever_kind;
  session->retriever_context_config.uri_prefix = session->retriever_uri_prefix;
  session->retriever_context_config.limit = session->retriever_limit;
  session->retriever_context_config.scope = session->retriever_scope;
  return session;
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

CXX_C_API int turbo_agent_session_get_capabilities(const turbo_agent_session_t *session,
                                                   json_value_t **out_capabilities_json) {
  json_value_t *capabilities;

  if (!session || !out_capabilities_json) {
    return -1;
  }
  *out_capabilities_json = NULL;
  capabilities = turbo_json_create_object();
  if (!capabilities) {
    return -1;
  }

  turbo_json_object_set_bool(capabilities, "has_agent", session->agent ? 1 : 0);
  turbo_json_object_set_bool(capabilities, "has_runtime", session->runtime ? 1 : 0);
  turbo_json_object_set_bool(capabilities, "has_memory_store",
                             turbo_agent_session_memory_store(session) ? 1 : 0);
  turbo_json_object_set_bool(capabilities, "has_tool_registry",
                             turbo_agent_session_tool_registry(session) ? 1 : 0);
  turbo_json_object_set_number(capabilities, "tool_count",
                               (double)turbo_agent_session_tool_count(session));
  turbo_json_object_set_number(capabilities, "workflow_kind_id", (double)session->workflow_kind);
  turbo_json_object_set_string(capabilities, "workflow_kind",
                               turbo_agent_session_workflow_kind_name(session->workflow_kind));
  turbo_agent_session_json_set_optional_string(capabilities, "memory_namespace",
                                               session->memory_namespace);
  turbo_agent_session_json_set_optional_string(capabilities, "model", session->model);
  turbo_agent_session_json_set_optional_string(capabilities, "base_url", session->base_url);
  turbo_agent_session_json_set_optional_string(capabilities, "provider", session->provider_name);
  turbo_json_object_set_bool(capabilities, "has_api_key", session->has_api_key ? 1 : 0);
  turbo_json_object_set_bool(capabilities, "has_knowledge_store", session->knowledge_store ? 1 : 0);
  turbo_json_object_set_bool(capabilities, "has_retriever", session->retriever ? 1 : 0);

  *out_capabilities_json = capabilities;
  return 0;
}

CXX_C_API int turbo_agent_session_get_tool_schemas(const turbo_agent_session_t *session,
                                                   json_value_t **out_tool_schemas_json) {
  const turbo_tool_registry_t *registry;
  json_value_t *registry_json_value = NULL;
  json_value_t *schemas = NULL;
  json_value_t *registry_json = NULL;
  json_value_t *openai_responses = NULL;
  json_value_t *openai_chat = NULL;
  json_value_t *openai_compatible_chat = NULL;
  json_value_t *anthropic = NULL;
  int rc = -1;

  if (!session || !out_tool_schemas_json) {
    return -1;
  }
  *out_tool_schemas_json = NULL;
  registry = turbo_agent_session_tool_registry(session);

  registry_json_value = turbo_tool_schema_build_registry_json_value(registry);
  if (!registry_json_value) {
    goto cleanup;
  }
  registry_json = turbo_json_clone(registry_json_value);
  if (!registry_json) {
    goto cleanup;
  }
  openai_responses = turbo_tool_schema_build_openai_tools(registry);
  openai_chat = turbo_tool_schema_build_openai_chat_tools(registry);
  openai_compatible_chat = turbo_tool_schema_build_openai_compatible_chat_tools(registry);
  anthropic = turbo_tool_schema_build_anthropic_tools(registry);
  schemas = turbo_json_create_object();
  if (!openai_responses || !openai_chat || !openai_compatible_chat || !anthropic || !schemas) {
    goto cleanup;
  }

  turbo_json_object_set_bool(schemas, "has_tool_registry", registry ? 1 : 0);
  turbo_json_object_set_number(schemas, "tool_count",
                               (double)turbo_agent_session_tool_count(session));
  if (turbo_agent_session_json_object_take(schemas, "registry", &registry_json) != 0 ||
      turbo_agent_session_json_object_take(schemas, "openai_responses", &openai_responses) != 0 ||
      turbo_agent_session_json_object_take(schemas, "openai_chat", &openai_chat) != 0 ||
      turbo_agent_session_json_object_take(schemas, "openai_compatible_chat",
                                           &openai_compatible_chat) != 0 ||
      turbo_agent_session_json_object_take(schemas, "anthropic", &anthropic) != 0) {
    goto cleanup;
  }

  *out_tool_schemas_json = schemas;
  schemas = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&schemas);
  turbo_free_json(&registry_json);
  turbo_free_json(&openai_responses);
  turbo_free_json(&openai_chat);
  turbo_free_json(&openai_compatible_chat);
  turbo_free_json(&anthropic);
  turbo_runtime_json_destroy(registry_json_value);
  return rc;
}

CXX_C_API int turbo_agent_session_get_startup_diagnostics(const turbo_agent_session_t *session,
                                                          json_value_t **out_diagnostics_json) {
  json_value_t *diagnostics = NULL;
  json_value_t *errors = NULL;
  json_value_t *capabilities = NULL;
  json_value_t *tool_schemas = NULL;
  json_value_t *tool_schema_status = NULL;
  json_value_t *workflow = NULL;
  json_value_t *structured_output = NULL;
  const char *workflow_error = NULL;
  const char *structured_output_error = NULL;
  int selected_tool_schema_ok = 0;
  int ok = 1;
  int rc = -1;

  if (!session || !out_diagnostics_json) {
    return -1;
  }
  *out_diagnostics_json = NULL;

  diagnostics = turbo_json_create_object();
  errors = turbo_json_create_array();
  if (!diagnostics || !errors) {
    goto cleanup;
  }
  turbo_json_object_set_number(diagnostics, "schema_version", 1.0);

  if (turbo_agent_session_get_capabilities(session, &capabilities) != 0 || !capabilities) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "capabilities_unavailable") != 0) {
      goto cleanup;
    }
    turbo_json_object_set_null(diagnostics, "capabilities");
  } else if (turbo_agent_session_json_object_take(diagnostics, "capabilities", &capabilities) !=
             0) {
    goto cleanup;
  }

  if (turbo_agent_session_get_workflow_diagnostics(session, &workflow, &workflow_error) != 0 ||
      !workflow) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "workflow_diagnostics_unavailable") !=
        0) {
      goto cleanup;
    }
    turbo_json_object_set_null(diagnostics, "workflow");
  } else {
    if (workflow_error) {
      ok = 0;
      if (turbo_agent_session_json_array_add_string(errors, workflow_error) != 0) {
        goto cleanup;
      }
    }
    if (turbo_agent_session_json_object_take(diagnostics, "workflow", &workflow) != 0) {
      goto cleanup;
    }
  }

  if (turbo_agent_session_get_structured_output_diagnostics(session, &structured_output,
                                                            &structured_output_error) != 0 ||
      !structured_output) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(
            errors, "structured_output_diagnostics_unavailable") != 0) {
      goto cleanup;
    }
    turbo_json_object_set_null(diagnostics, "structured_output");
  } else {
    if (structured_output_error) {
      ok = 0;
      if (turbo_agent_session_json_array_add_string(errors, structured_output_error) != 0) {
        goto cleanup;
      }
    }
    if (turbo_agent_session_json_object_take(diagnostics, "structured_output",
                                             &structured_output) != 0) {
      goto cleanup;
    }
  }

  if (turbo_agent_session_get_tool_schema_status(session, &tool_schema_status,
                                                 &selected_tool_schema_ok) != 0 ||
      !tool_schema_status) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "tool_schema_status_unavailable") != 0) {
      goto cleanup;
    }
    turbo_json_object_set_null(diagnostics, "tool_schema_status");
  } else {
    if (!selected_tool_schema_ok) {
      ok = 0;
      if (turbo_agent_session_json_array_add_string(errors, "invalid_tool_schemas") != 0) {
        goto cleanup;
      }
    }
    if (turbo_agent_session_json_object_take(diagnostics, "tool_schema_status",
                                             &tool_schema_status) != 0) {
      goto cleanup;
    }
  }

  if (turbo_agent_session_get_tool_schemas(session, &tool_schemas) != 0 || !tool_schemas) {
    if (!selected_tool_schema_ok) {
      ok = 0;
    }
    turbo_json_object_set_null(diagnostics, "tool_schemas");
  } else if (turbo_agent_session_json_object_take(diagnostics, "tool_schemas", &tool_schemas) !=
             0) {
    goto cleanup;
  }

  turbo_json_object_set_bool(diagnostics, "ok", ok ? 1 : 0);
  turbo_json_object_add(diagnostics, "errors", errors);
  errors = NULL;

  *out_diagnostics_json = diagnostics;
  diagnostics = NULL;
  rc = 0;

cleanup:
  turbo_free_json(&diagnostics);
  turbo_free_json(&errors);
  turbo_free_json(&capabilities);
  turbo_free_json(&tool_schemas);
  turbo_free_json(&tool_schema_status);
  turbo_free_json(&workflow);
  turbo_free_json(&structured_output);
  return rc;
}

CXX_C_API int
turbo_agent_session_add_trace_json_value_sink(turbo_agent_session_t *session,
                                              const turbo_agent_trace_json_value_sink_t *sink) {
  if (!session || !session->agent || !sink) {
    return -1;
  }
  return turbo_agent_add_trace_json_value_sink(session->agent, sink);
}

CXX_C_API int turbo_agent_session_add_observer_json_value_sink(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink) {
  turbo_agent_session_observer_bridge_t *bridge;
  turbo_agent_trace_json_value_sink_t trace_sink = {0};

  if (!session || !session->agent || !sink || !sink->callback) {
    return -1;
  }
  bridge = (turbo_agent_session_observer_bridge_t *)calloc(1, sizeof(*bridge));
  if (!bridge) {
    return -1;
  }
  bridge->sink = *sink;
  trace_sink.callback = turbo_agent_session_capture_observer_event;
  trace_sink.user_data = bridge;
  trace_sink.user_data_free = turbo_agent_session_observer_bridge_free;
  if (turbo_agent_add_trace_json_value_sink(session->agent, &trace_sink) != 0) {
    free(bridge);
    return -1;
  }
  return 0;
}

CXX_C_API int turbo_agent_session_set_trace_history_enabled(turbo_agent_session_t *session,
                                                            int enabled) {
  if (!session || !session->agent) {
    return -1;
  }
  return turbo_agent_set_trace_history_enabled(session->agent, enabled);
}

CXX_C_API int turbo_agent_session_get_thread(turbo_agent_session_t *session,
                                             json_value_t **out_thread_json) {
  if (!session || !session->runtime || !session->thread_id || !out_thread_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread(session->runtime, session->thread_id, out_thread_json);
}

CXX_C_API int turbo_agent_session_get_run(turbo_agent_session_t *session, const char *run_id,
                                          json_value_t **out_run_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(session->runtime, resolved_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_latest_run(turbo_agent_session_t *session,
                                                 json_value_t **out_run_json) {
  if (!session || !session->runtime || !session->thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_run(session->runtime, session->thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_pending_run(turbo_agent_session_t *session,
                                                  json_value_t **out_run_json) {
  if (!session || !session->runtime || !session->thread_id || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_pending_run(session->runtime, session->thread_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint(turbo_agent_session_t *session,
                                                 const char *checkpoint_id,
                                                 json_value_t **out_checkpoint_json) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(session->runtime, resolved_checkpoint_id,
                                            out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_latest_checkpoint(turbo_agent_session_t *session,
                                                        const char *run_id,
                                                        json_value_t **out_checkpoint_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_latest_checkpoint(session->runtime, resolved_run_id,
                                                   out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_thread_state_json_value(turbo_agent_session_t *session,
                                                              json_value_t **out_state) {
  if (!session || !session->runtime || !session->thread_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_state_json_value(session->runtime, session->thread_id,
                                                         out_state);
}

CXX_C_API int turbo_agent_session_get_thread_head_state_json_value(turbo_agent_session_t *session,
                                                                   json_value_t **out_state) {
  if (!session || !session->runtime || !session->thread_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_head_state_json_value(session->runtime, session->thread_id,
                                                              out_state);
}

CXX_C_API int turbo_agent_session_get_thread_trace_events_json_value(turbo_agent_session_t *session,
                                                                     json_value_t **out_events) {
  if (!session || !session->runtime || !session->thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_trace_events_json_value(session->runtime,
                                                                session->thread_id, out_events);
}

CXX_C_API int
turbo_agent_session_get_thread_head_trace_events_json_value(turbo_agent_session_t *session,
                                                            json_value_t **out_events) {
  if (!session || !session->runtime || !session->thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_head_trace_events_json_value(
      session->runtime, session->thread_id, out_events);
}

CXX_C_API int turbo_agent_session_get_run_state_json_value(turbo_agent_session_t *session,
                                                           const char *run_id,
                                                           json_value_t **out_state) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_run_state_json_value(session->runtime, resolved_run_id, out_state);
}

CXX_C_API int turbo_agent_session_get_run_trace_events_json_value(turbo_agent_session_t *session,
                                                                  const char *run_id,
                                                                  json_value_t **out_events) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_json_value(session->runtime, resolved_run_id,
                                                             out_events);
}

CXX_C_API int turbo_agent_session_get_checkpoint_state_json_value(turbo_agent_session_t *session,
                                                                  const char *checkpoint_id,
                                                                  json_value_t **out_state) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_state) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_state_json_value(session->runtime,
                                                             resolved_checkpoint_id, out_state);
}

CXX_C_API int turbo_agent_session_prepare_checkpoint_state_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !state_patch ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_state_override_json_value(
      session->runtime, resolved_checkpoint_id, state_patch, out_state_override);
}

CXX_C_API int
turbo_agent_session_prepare_thread_state_override_json_value(turbo_agent_session_t *session,
                                                             const json_value_t *state_patch,
                                                             json_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_state_override_json_value(
      session->runtime, session->thread_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_state_patch_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *state_patch,
    json_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !state_patch ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_apply_state_patch_json_value(session->runtime, resolved_checkpoint_id,
                                                          state_patch, out_state_override);
}

CXX_C_API int
turbo_agent_session_apply_thread_state_patch_json_value(turbo_agent_session_t *session,
                                                        const json_value_t *state_patch,
                                                        json_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !state_patch || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_state_override_json_value(
      session->runtime, session->thread_id, state_patch, out_state_override);
}

CXX_C_API int turbo_agent_session_get_supervisor_inbox(turbo_agent_session_t *session,
                                                       json_value_t **out_inbox_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_inbox, out_inbox_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_handoff_history(turbo_agent_session_t *session,
                                                                 json_value_t **out_history_json) {
  return turbo_agent_session_get_supervisor_array_json_local(
      session, turbo_agent_state_supervisor_handoff_history, out_history_json);
}

CXX_C_API int turbo_agent_session_get_supervisor_inspect(turbo_agent_session_t *session,
                                                         json_value_t **out_inspect_json) {
  return turbo_agent_session_build_supervisor_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_get_orchestration_inspect(turbo_agent_session_t *session,
                                                            json_value_t **out_inspect_json) {
  return turbo_agent_session_build_orchestration_inspect_local(session, out_inspect_json);
}

CXX_C_API int turbo_agent_session_get_observability_index(turbo_agent_session_t *session,
                                                          json_value_t **out_index_json) {
  if (!session || !session->runtime || !session->thread_id || !out_index_json) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_observability_index(session->runtime, session->thread_id,
                                                            out_index_json);
}

CXX_C_API int turbo_agent_session_append_supervisor_inbox_message_json_value(
    turbo_agent_session_t *session, const char *source_agent, const char *text,
    json_value_t **out_state_override) {
  return turbo_agent_session_build_supervisor_inbox_override_local(session, source_agent, text,
                                                                   out_state_override);
}

CXX_C_API int turbo_agent_session_get_checkpoint_trace_events_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, json_value_t **out_events) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_trace_events_json_value(
      session->runtime, resolved_checkpoint_id, out_events);
}

CXX_C_API int turbo_agent_session_get_child_run(turbo_agent_session_t *session,
                                                const json_value_t *output_item,
                                                json_value_t **out_run_json) {
  const char *child_run_id = turbo_agent_session_output_item_child_run_id(output_item);

  if (!session || !session->runtime || !child_run_id || child_run_id[0] == '\0' || !out_run_json) {
    return -1;
  }
  return turbo_agent_runtime_get_run(session->runtime, child_run_id, out_run_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint(turbo_agent_session_t *session,
                                                       const json_value_t *output_item,
                                                       json_value_t **out_checkpoint_json) {
  const char *child_checkpoint_id =
      turbo_agent_session_output_item_child_checkpoint_id(output_item);

  if (!session || !session->runtime || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_checkpoint_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint(session->runtime, child_checkpoint_id,
                                            out_checkpoint_json);
}

CXX_C_API int turbo_agent_session_get_child_checkpoint_context(turbo_agent_session_t *session,
                                                               const json_value_t *output_item,
                                                               json_value_t **out_context_json) {
  const char *child_checkpoint_id =
      turbo_agent_session_output_item_child_checkpoint_id(output_item);

  if (!session || !session->runtime || !child_checkpoint_id || child_checkpoint_id[0] == '\0' ||
      !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(session->runtime, child_checkpoint_id,
                                                    out_context_json);
}

CXX_C_API int turbo_agent_session_get_child_thread_timeline_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item, json_value_t **out_timeline) {
  const char *child_thread_id = turbo_agent_session_output_item_child_thread_id(output_item);

  if (!session || !session->runtime || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_json_value(session->runtime, child_thread_id,
                                                            out_timeline);
}

CXX_C_API int turbo_agent_session_get_child_branch_tree(turbo_agent_session_t *session,
                                                        const json_value_t *output_item,
                                                        json_value_t **out_branch_tree_json) {
  const char *child_thread_id = turbo_agent_session_output_item_child_thread_id(output_item);

  if (!session || !session->runtime || !child_thread_id || child_thread_id[0] == '\0' ||
      !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(session->runtime, child_thread_id,
                                             out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_child_inspect(turbo_agent_session_t *session,
                                                    const json_value_t *output_item,
                                                    json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *run_json = NULL;
  json_value_t *checkpoints_json = NULL;
  json_value_t *latest_checkpoint_json = NULL;
  json_value_t *checkpoint_context_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *history_events_json_value = NULL;
  json_value_t *trace_events_json_value = NULL;
  json_value_t *thread_timeline_json_value = NULL;
  const char *child_checkpoint_id;

  if (!session || !session->runtime || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  child_checkpoint_id = turbo_agent_session_output_item_child_checkpoint_id(output_item);
  if (turbo_agent_session_get_child_run(session, output_item, &run_json) != 0 || !run_json) {
    goto cleanup;
  }
  if (turbo_agent_session_list_child_checkpoints(session, output_item, &checkpoints_json) != 0 ||
      !checkpoints_json) {
    goto cleanup;
  }
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    if (turbo_agent_session_get_child_checkpoint(session, output_item, &latest_checkpoint_json) !=
            0 ||
        !latest_checkpoint_json) {
      goto cleanup;
    }
    if (turbo_agent_session_get_child_checkpoint_context(session, output_item,
                                                         &checkpoint_context_json) != 0 ||
        !checkpoint_context_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_load_child_history_events_json_value(session, output_item,
                                                               &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = turbo_json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_trace_events_json_value(session, output_item,
                                                            &trace_events_json_value) != 0 ||
      !trace_events_json_value) {
    goto cleanup;
  }
  trace_events_json = turbo_json_clone(trace_events_json_value);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_thread_timeline_json_value(session, output_item,
                                                               &thread_timeline_json_value) == 0 &&
      thread_timeline_json_value) {
    thread_timeline_json = turbo_json_clone(thread_timeline_json_value);
    if (!thread_timeline_json) {
      goto cleanup;
    }
  }
  if (turbo_agent_session_get_child_branch_tree(session, output_item, &branch_tree_json) != 0) {
    turbo_free_json(&branch_tree_json);
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "run", run_json);
  run_json = NULL;
  turbo_json_object_add(inspect_json, "checkpoints", checkpoints_json);
  checkpoints_json = NULL;
  if (latest_checkpoint_json) {
    turbo_json_object_add(inspect_json, "latest_checkpoint", latest_checkpoint_json);
    latest_checkpoint_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "latest_checkpoint");
  }
  if (checkpoint_context_json) {
    turbo_json_object_add(inspect_json, "checkpoint_context", checkpoint_context_json);
    checkpoint_context_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "checkpoint_context");
  }
  turbo_json_object_add(inspect_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(inspect_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  if (thread_timeline_json) {
    turbo_json_object_add(inspect_json, "thread_timeline", thread_timeline_json);
    thread_timeline_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "thread_timeline");
  }
  if (branch_tree_json) {
    turbo_json_object_add(inspect_json, "branch_tree", branch_tree_json);
    branch_tree_json = NULL;
  } else {
    turbo_json_object_set_null(inspect_json, "branch_tree");
  }

  *out_inspect_json = inspect_json;
  inspect_json = NULL;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_timeline_json);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  turbo_free_json(&trace_events_json);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_free_json(&history_events_json);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_free_json(&checkpoint_context_json);
  turbo_free_json(&latest_checkpoint_json);
  turbo_free_json(&checkpoints_json);
  turbo_free_json(&run_json);
  return *out_inspect_json ? 0 : -1;
}

CXX_C_API int turbo_agent_session_get_child_orchestration_inspect(turbo_agent_session_t *session,
                                                                  const json_value_t *output_item,
                                                                  json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *child_inspect_json = NULL;
  const char *parent_agent_run_id;
  const char *parent_tool_call_id;
  const char *parent_tool_name;
  const char *parent_graph_run_id;
  const char *call_frame_id;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_child_inspect(session, output_item, &child_inspect_json) != 0 ||
      !child_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }

  parent_agent_run_id = turbo_agent_state_tool_result_parent_agent_run_id(output_item);
  parent_tool_call_id = turbo_agent_state_tool_result_parent_tool_call_id(output_item);
  parent_tool_name = turbo_agent_state_tool_result_parent_tool_name(output_item);
  parent_graph_run_id = turbo_agent_state_tool_result_parent_graph_run_id(output_item);
  call_frame_id = turbo_agent_state_tool_result_call_frame_id(output_item);

  if (parent_agent_run_id && parent_agent_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_agent_run_id", parent_agent_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_agent_run_id");
  }
  if (parent_tool_call_id && parent_tool_call_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_call_id", parent_tool_call_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_call_id");
  }
  if (parent_tool_name && parent_tool_name[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_tool_name", parent_tool_name);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_tool_name");
  }
  if (parent_graph_run_id && parent_graph_run_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "parent_graph_run_id", parent_graph_run_id);
  } else {
    turbo_json_object_set_null(inspect_json, "parent_graph_run_id");
  }
  if (call_frame_id && call_frame_id[0] != '\0') {
    turbo_json_object_set_string(inspect_json, "call_frame_id", call_frame_id);
  } else {
    turbo_json_object_set_null(inspect_json, "call_frame_id");
  }
  turbo_json_object_add(inspect_json, "child_inspect", child_inspect_json);
  child_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_session_get_child_multi_agent_inspect(turbo_agent_session_t *session,
                                                                const json_value_t *output_item,
                                                                json_value_t **out_inspect_json) {
  json_value_t *inspect_json = NULL;
  json_value_t *supervisor_inspect_json = NULL;
  json_value_t *orchestration_inspect_json = NULL;
  json_value_t *child_orchestration_inspect_json = NULL;

  if (!session || !output_item || !out_inspect_json) {
    return -1;
  }
  *out_inspect_json = NULL;

  if (turbo_agent_session_get_supervisor_inspect(session, &supervisor_inspect_json) != 0 ||
      !supervisor_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_orchestration_inspect(session, &orchestration_inspect_json) != 0 ||
      !orchestration_inspect_json) {
    goto cleanup;
  }
  if (turbo_agent_session_get_child_orchestration_inspect(session, output_item,
                                                          &child_orchestration_inspect_json) != 0 ||
      !child_orchestration_inspect_json) {
    goto cleanup;
  }

  inspect_json = turbo_json_create_object();
  if (!inspect_json) {
    goto cleanup;
  }
  turbo_json_object_add(inspect_json, "supervisor_inspect", supervisor_inspect_json);
  supervisor_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "orchestration_inspect", orchestration_inspect_json);
  orchestration_inspect_json = NULL;
  turbo_json_object_add(inspect_json, "child_orchestration_inspect",
                        child_orchestration_inspect_json);
  child_orchestration_inspect_json = NULL;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  return 0;

cleanup:
  turbo_free_json(&inspect_json);
  turbo_free_json(&child_orchestration_inspect_json);
  turbo_free_json(&orchestration_inspect_json);
  turbo_free_json(&supervisor_inspect_json);
  return -1;
}

CXX_C_API int turbo_agent_session_list_runs(turbo_agent_session_t *session,
                                            json_value_t **out_runs_json) {
  if (!session || !session->runtime || !session->thread_id || !out_runs_json) {
    return -1;
  }
  return turbo_agent_runtime_list_runs(session->runtime, session->thread_id, out_runs_json);
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

CXX_C_API int turbo_agent_session_list_child_checkpoints(turbo_agent_session_t *session,
                                                         const json_value_t *output_item,
                                                         json_value_t **out_checkpoints_json) {
  const char *child_run_id = turbo_agent_session_output_item_child_run_id(output_item);

  if (!session || !session->runtime || !child_run_id || child_run_id[0] == '\0' ||
      !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(session->runtime, child_run_id, out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_list_checkpoints(turbo_agent_session_t *session,
                                                   const char *run_id,
                                                   json_value_t **out_checkpoints_json) {
  const char *resolved_run_id = turbo_agent_session_resolve_run_id(session, run_id);

  if (!session || !session->runtime || !resolved_run_id || !out_checkpoints_json) {
    return -1;
  }
  return turbo_agent_runtime_list_checkpoints(session->runtime, resolved_run_id,
                                              out_checkpoints_json);
}

CXX_C_API int turbo_agent_session_list_thread_lineage(turbo_agent_session_t *session,
                                                      json_value_t **out_lineage_json) {
  if (!session || !session->runtime || !session->thread_id || !out_lineage_json) {
    return -1;
  }
  return turbo_agent_runtime_list_thread_lineage(session->runtime, session->thread_id,
                                                 out_lineage_json);
}

CXX_C_API int turbo_agent_session_get_branch_tree(turbo_agent_session_t *session,
                                                  json_value_t **out_branch_tree_json) {
  if (!session || !session->runtime || !session->thread_id || !out_branch_tree_json) {
    return -1;
  }
  return turbo_agent_runtime_get_branch_tree(session->runtime, session->thread_id,
                                             out_branch_tree_json);
}

CXX_C_API int turbo_agent_session_get_checkpoint_context(turbo_agent_session_t *session,
                                                         const char *checkpoint_id,
                                                         json_value_t **out_context_json) {
  if (!session || !session->runtime || !checkpoint_id || !checkpoint_id[0] || !out_context_json) {
    return -1;
  }
  return turbo_agent_runtime_get_checkpoint_context(session->runtime, checkpoint_id,
                                                    out_context_json);
}

CXX_C_API int turbo_agent_session_load_history_events_json_value(turbo_agent_session_t *session,
                                                                 const char *run_id,
                                                                 const char *checkpoint_id,
                                                                 json_value_t **out_events) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !out_events) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_load_history_events_json_value(session->runtime, resolved_run_id,
                                                            resolved_checkpoint_id, out_events);
}

CXX_C_API int
turbo_agent_session_load_thread_history_events_json_value(turbo_agent_session_t *session,
                                                          json_value_t **out_events) {
  if (!session || !session->runtime || !session->thread_id || !out_events) {
    return -1;
  }
  return turbo_agent_runtime_load_thread_history_events_json_value(session->runtime,
                                                                   session->thread_id, out_events);
}

CXX_C_API int turbo_agent_session_replay_history_json_value(
    turbo_agent_session_t *session, const char *run_id, const char *checkpoint_id,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !event_sink) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_replay_history_json_value(
      session->runtime, resolved_run_id, resolved_checkpoint_id, event_sink, event_sink_user_data);
}

CXX_C_API int
turbo_agent_session_replay_thread_history_json_value(turbo_agent_session_t *session,
                                                     turbo_event_sink_json_value_fn event_sink,
                                                     void *event_sink_user_data) {
  if (!session || !session->runtime || !session->thread_id || !event_sink) {
    return -1;
  }
  return turbo_agent_runtime_replay_thread_history_json_value(session->runtime, session->thread_id,
                                                              event_sink, event_sink_user_data);
}

CXX_C_API int
turbo_agent_session_observe_history_json_value(turbo_agent_session_t *session, const char *run_id,
                                               const char *checkpoint_id,
                                               const turbo_agent_observer_json_value_sink_t *sink) {
  const char *resolved_run_id = NULL;
  const char *resolved_checkpoint_id = NULL;

  if (!session || !session->runtime || !sink || !sink->callback) {
    return -1;
  }
  if (run_id && run_id[0] != '\0') {
    resolved_run_id = run_id;
  } else if (checkpoint_id && checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = checkpoint_id;
  } else if (session->last_run_id && session->last_run_id[0] != '\0') {
    resolved_run_id = session->last_run_id;
  } else if (session->last_checkpoint_id && session->last_checkpoint_id[0] != '\0') {
    resolved_checkpoint_id = session->last_checkpoint_id;
  } else {
    return -1;
  }
  return turbo_agent_runtime_observe_history_json_value(session->runtime, resolved_run_id,
                                                        resolved_checkpoint_id, sink);
}

CXX_C_API int turbo_agent_session_observe_thread_history_json_value(
    turbo_agent_session_t *session, const turbo_agent_observer_json_value_sink_t *sink) {
  if (!session || !session->runtime || !session->thread_id || !sink || !sink->callback) {
    return -1;
  }
  return turbo_agent_runtime_observe_thread_history_json_value(session->runtime, session->thread_id,
                                                               sink);
}

CXX_C_API int turbo_agent_session_get_thread_timeline_json_value(turbo_agent_session_t *session,
                                                                 json_value_t **out_timeline) {
  if (!session || !session->runtime || !session->thread_id || !out_timeline) {
    return -1;
  }
  return turbo_agent_runtime_get_thread_timeline_json_value(session->runtime, session->thread_id,
                                                            out_timeline);
}

CXX_C_API int turbo_agent_session_apply_command_json_value(turbo_agent_session_t *session,
                                                           const char *checkpoint_id,
                                                           const json_value_t *command,
                                                           json_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      session->runtime, resolved_checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_session_prepare_checkpoint_command_override_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  const char *resolved_checkpoint_id =
      turbo_agent_session_resolve_checkpoint_id(session, checkpoint_id);

  if (!session || !session->runtime || !resolved_checkpoint_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      session->runtime, resolved_checkpoint_id, command, out_state_override);
}

CXX_C_API int turbo_agent_session_apply_checkpoint_command_json_value(
    turbo_agent_session_t *session, const char *checkpoint_id, const json_value_t *command,
    json_value_t **out_state_override) {
  if (!session || !session->runtime || !checkpoint_id || !checkpoint_id[0] || !command ||
      !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_checkpoint_command_override_json_value(
      session->runtime, checkpoint_id, command, out_state_override);
}

CXX_C_API int
turbo_agent_session_prepare_thread_command_override_json_value(turbo_agent_session_t *session,
                                                               const json_value_t *command,
                                                               json_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_command_override_json_value(
      session->runtime, session->thread_id, command, out_state_override);
}

CXX_C_API int
turbo_agent_session_apply_thread_command_json_value(turbo_agent_session_t *session,
                                                    const json_value_t *command,
                                                    json_value_t **out_state_override) {
  if (!session || !session->runtime || !session->thread_id || !command || !out_state_override) {
    return -1;
  }
  return turbo_agent_runtime_prepare_thread_command_override_json_value(
      session->runtime, session->thread_id, command, out_state_override);
}

/* === Core graph execution =========================================== */

CXX_C_API int turbo_agent_session_start_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *state,
    const turbo_graph_run_options_t *options, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_agent_runtime_parent_link_t parent_link = {0};
  turbo_agent_runtime_exec_options_t exec_options = {0};
  json_value_t *local_summary_json = NULL;
  json_value_t **summary_slot = out_summary_json ? out_summary_json : &local_summary_json;
  int rc;

  if (!session || !session->runtime || !graph || !state || !out_state) {
    return -1;
  }
  if (out_summary_json) {
    *out_summary_json = NULL;
  }

  exec_options.thread_id = session->thread_id;
  exec_options.event_sink = event_sink;
  exec_options.event_sink_user_data = event_sink_user_data;

  if (turbo_agent_session_has_parent_link(session, &parent_link)) {
    exec_options.parent_link = &parent_link;
  }

  rc = turbo_agent_runtime_exec_start(session->runtime, graph, state, options, &exec_options,
                                      summary_slot, out_state);

  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *summary_slot);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_inbox_commit_bound_claim(session->inbox);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_session_run_follow_ups(session, graph, options, event_sink,
                                            event_sink_user_data, NULL, summary_slot, out_state);
  }
  if (!out_summary_json) {
    turbo_free_json(&local_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_resume_graph(
    turbo_agent_session_t *session, turbo_graph_t *graph, const json_value_t *input,
    const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_agent_runtime_exec_options_t rt_options = {0};
  json_value_t *local_summary_json = NULL;
  json_value_t **summary_slot = out_summary_json ? out_summary_json : &local_summary_json;
  int rc;

  if (!session || !session->runtime || !graph || !exec_options || !out_state) {
    return -1;
  }
  if (out_summary_json) {
    *out_summary_json = NULL;
  }

  rt_options.scope = (turbo_agent_runtime_scope_t)exec_options->scope;
  rt_options.input_kind = (turbo_agent_runtime_input_kind_t)exec_options->input_kind;
  if (rt_options.scope == TURBO_RUNTIME_SCOPE_THREAD) {
    if (!session->thread_id) return -1;
    rt_options.thread_id = session->thread_id;
  } else {
    rt_options.checkpoint_id = exec_options->checkpoint_id
                                   ? exec_options->checkpoint_id
                                   : turbo_agent_session_resolve_checkpoint_id(session, NULL);
    if (!rt_options.checkpoint_id) return -1;
  }

  rt_options.event_sink = event_sink;
  rt_options.event_sink_user_data = event_sink_user_data;

  rc = turbo_agent_runtime_exec_resume(session->runtime, graph, input, options, &rt_options,
                                       summary_slot, out_state);

  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *summary_slot);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_inbox_commit_bound_claim(session->inbox);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_session_run_follow_ups(session, graph, options, event_sink,
                                            event_sink_user_data, NULL, summary_slot, out_state);
  }
  if (!out_summary_json) {
    turbo_free_json(&local_summary_json);
  }
  return rc;
}

CXX_C_API int turbo_agent_session_fork_graph(turbo_agent_session_t *session, turbo_graph_t *graph,
                                             const json_value_t *input,
                                             const turbo_graph_run_options_t *options,
                                             const turbo_agent_session_exec_options_t *exec_options,
                                             turbo_event_sink_json_value_fn event_sink,
                                             void *event_sink_user_data,
                                             json_value_t **out_summary_json,
                                             json_value_t **out_state) {
  turbo_agent_runtime_exec_options_t rt_options = {0};
  json_value_t *local_summary_json = NULL;
  json_value_t **summary_slot = out_summary_json ? out_summary_json : &local_summary_json;
  int rc;

  if (!session || !session->runtime || !graph || !exec_options || !out_state) {
    return -1;
  }
  if (out_summary_json) {
    *out_summary_json = NULL;
  }

  rt_options.scope = (turbo_agent_runtime_scope_t)exec_options->scope;
  rt_options.input_kind = (turbo_agent_runtime_input_kind_t)exec_options->input_kind;
  if (rt_options.scope == TURBO_RUNTIME_SCOPE_THREAD) {
    if (!session->thread_id) return -1;
    rt_options.thread_id = session->thread_id;
  } else {
    rt_options.checkpoint_id = exec_options->checkpoint_id
                                   ? exec_options->checkpoint_id
                                   : turbo_agent_session_resolve_checkpoint_id(session, NULL);
    if (!rt_options.checkpoint_id) return -1;
  }

  rt_options.event_sink = event_sink;
  rt_options.event_sink_user_data = event_sink_user_data;

  rc = turbo_agent_runtime_exec_fork(session->runtime, graph, input, options, &rt_options,
                                     summary_slot, out_state);

  if (rc == 0) {
    rc = turbo_agent_session_capture_summary(session, *summary_slot);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_inbox_commit_bound_claim(session->inbox);
  }
  if (rc == 0 && session->inbox) {
    rc = turbo_agent_session_run_follow_ups(session, graph, options, event_sink,
                                            event_sink_user_data, NULL, summary_slot, out_state);
  }
  if (!out_summary_json) {
    turbo_free_json(&local_summary_json);
  }
  return rc;
}

/* === Preset execution =============================================== */

CXX_C_API int turbo_agent_session_start_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *state, const turbo_graph_run_options_t *options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_start_graph(session, graph, state, options, event_sink,
                                       event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_resume_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_resume_graph(session, graph, input, options, exec_options, event_sink,
                                        event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_fork_preset(
    turbo_agent_session_t *session, turbo_agent_session_workflow_kind_t kind,
    const json_value_t *input, const turbo_graph_run_options_t *options,
    const turbo_agent_session_exec_options_t *exec_options,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  turbo_graph_t *graph;
  int rc;

  graph = turbo_agent_session_create_preset_graph(session, kind);
  if (!graph) {
    return -1;
  }
  rc = turbo_agent_session_fork_graph(session, graph, input, options, exec_options, event_sink,
                                      event_sink_user_data, out_summary_json, out_state);
  turbo_graph_destroy(graph);
  return rc;
}

CXX_C_API int turbo_agent_session_load_child_history_events_json_value(
    turbo_agent_session_t *session, const json_value_t *output_item, json_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_load_history_events_json_value(session->runtime, NULL,
                                                              child_checkpoint_id, out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_load_history_events_json_value(session->runtime, child_run_id, NULL,
                                                            out_events);
}

CXX_C_API int turbo_agent_session_get_child_trace_events_json_value(turbo_agent_session_t *session,
                                                                    const json_value_t *output_item,
                                                                    json_value_t **out_events) {
  const char *child_checkpoint_id;
  const char *child_run_id;

  if (!session || !session->runtime || !output_item || !out_events) {
    return -1;
  }

  child_checkpoint_id = turbo_agent_state_tool_result_child_checkpoint_id(output_item);
  if (child_checkpoint_id && child_checkpoint_id[0] != '\0') {
    return turbo_agent_runtime_get_checkpoint_trace_events_json_value(
        session->runtime, child_checkpoint_id, out_events);
  }

  child_run_id = turbo_agent_state_tool_result_child_run_id(output_item);
  if (!child_run_id || child_run_id[0] == '\0') {
    return -1;
  }
  return turbo_agent_runtime_get_run_trace_events_json_value(session->runtime, child_run_id,
                                                             out_events);
}

static turbo_graph_t *turbo_agent_session_create_graph_base(const turbo_agent_session_t *session,
                                                            const char *graph_name) {
  turbo_graph_t *graph;

  if (!session || !session->agent || !graph_name) {
    return NULL;
  }
  graph = turbo_graph_create(graph_name);
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_loop_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_loop(graph, session->agent, "model", "tools", "end", 1) !=
      TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_review_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-review-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_review_loop(graph, session->agent, session->agent, "planner",
                                      "plan_commit", "plan_step", "review", "executor", "tools",
                                      "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_engineering_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-engineering-loop");
  if (!graph) {
    return NULL;
  }
  if (turbo_agent_install_engineering_loop(
          graph, session->agent, session->agent, "planner", "plan_commit", "plan_step", "review",
          "executor", "tools", "detect_failure", "replan_route", "replan_prepare", "plan_advance",
          "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_knowledge_engineering_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-knowledge-engineering-loop");
  if (!graph || !session->knowledge_store) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  if (turbo_agent_install_knowledge_engineering_loop(
          graph, &session->knowledge_context_config, session->agent, session->agent, "knowledge",
          "planner", "plan_commit", "plan_step", "review", "executor", "tools", "detect_failure",
          "replan_route", "replan_prepare", "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_retriever_engineering_graph(const turbo_agent_session_t *session) {
  turbo_graph_t *graph;

  graph = turbo_agent_session_create_graph_base(session, "session-retriever-engineering-loop");
  if (!graph || !session->retriever) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  if (turbo_agent_install_retriever_engineering_loop(
          graph, &session->retriever_context_config, session->agent, session->agent, "retriever",
          "planner", "plan_commit", "plan_step", "review", "executor", "tools", "detect_failure",
          "replan_route", "replan_prepare", "plan_advance", "end", 1) != TURBO_GRAPH_EXEC_OK) {
    turbo_graph_destroy(graph);
    return NULL;
  }
  return graph;
}

CXX_C_API turbo_graph_t *
turbo_agent_session_create_preset_graph(const turbo_agent_session_t *session,
                                        turbo_agent_session_workflow_kind_t kind) {
  switch (kind) {
  case TURBO_AGENT_SESSION_WORKFLOW_LOOP:
    return turbo_agent_session_create_loop_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_REVIEW:
    return turbo_agent_session_create_review_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING:
    return turbo_agent_session_create_engineering_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING:
    return turbo_agent_session_create_knowledge_engineering_graph(session);
  case TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING:
    return turbo_agent_session_create_retriever_engineering_graph(session);
  default:
    return NULL;
  }
}

CXX_C_API json_value_t *turbo_agent_session_create_input_state_json_value(const char *user_text) {
  json_value_t *state;
  json_value_t *json_value_state;

  state = turbo_agent_state_create();
  if (!state) {
    return NULL;
  }
  if (user_text && user_text[0] != '\0' &&
      turbo_agent_state_add_user_message(state, user_text) != 0) {
    turbo_free_json(&state);
    return NULL;
  }
  json_value_state = turbo_json_clone(state);
  turbo_free_json(&state);
  return json_value_state;
}

CXX_C_API json_value_t *
turbo_agent_session_create_input_messages_state_json_value(const json_value_t *messages) {
  json_value_t *state;
  json_value_t *input;
  json_value_t *json_value_state = NULL;
  size_t i;

  if (!messages || turbo_json_type(messages) != TURBO_JSON_ARRAY) {
    return NULL;
  }

  state = turbo_agent_state_create();
  if (!state) {
    return NULL;
  }
  input = turbo_json_object_get(state, "input");
  if (!input || turbo_json_type(input) != TURBO_JSON_ARRAY) {
    turbo_free_json(&state);
    return NULL;
  }

  for (i = 0; i < turbo_runtime_json_value_size(messages); ++i) {
    const json_value_t *message = turbo_json_array_get(messages, i);
    json_value_t *message_json;

    if (!message || turbo_prompt_message_validate_json_value(message) != TURBO_PROMPT_OK) {
      turbo_free_json(&state);
      return NULL;
    }
    message_json = turbo_json_clone(message);
    if (!message_json) {
      turbo_free_json(&state);
      return NULL;
    }
    turbo_json_array_add(input, message_json);
  }

  json_value_state = turbo_json_clone(state);
  turbo_free_json(&state);
  return json_value_state;
}

CXX_C_API json_value_t *turbo_agent_session_create_input_state_with_memory_json_value(
    const turbo_agent_session_t *session, const char *user_text, const char *namespace_prefix) {
  json_value_t *state;
  json_value_t *json_state;
  json_value_t *bound = NULL;

  state = turbo_agent_session_create_input_state_json_value(user_text);
  if (!state) {
    return NULL;
  }
  json_state = turbo_json_clone(state);
  turbo_runtime_json_destroy(state);
  if (!json_state) {
    return NULL;
  }
  if (turbo_agent_session_load_memory_context(session, json_state, namespace_prefix) != 0) {
    turbo_free_json(&json_state);
    return NULL;
  }
  bound = turbo_json_clone(json_state);
  turbo_free_json(&json_state);
  return bound;
}

CXX_C_API json_value_t *turbo_agent_session_create_input_messages_state_with_memory_json_value(
    const turbo_agent_session_t *session, const json_value_t *messages,
    const char *namespace_prefix) {
  json_value_t *state;
  json_value_t *json_state;
  json_value_t *bound = NULL;

  state = turbo_agent_session_create_input_messages_state_json_value(messages);
  if (!state) {
    return NULL;
  }
  json_state = turbo_json_clone(state);
  turbo_runtime_json_destroy(state);
  if (!json_state) {
    return NULL;
  }
  if (turbo_agent_session_load_memory_context(session, json_state, namespace_prefix) != 0) {
    turbo_free_json(&json_state);
    return NULL;
  }
  bound = turbo_json_clone(json_state);
  turbo_free_json(&json_state);
  return bound;
}

static int turbo_agent_session_batch_add_text_result(json_value_t *results, size_t index, int ok,
                                                     json_value_t **summary_json,
                                                     const char *output_text,
                                                     const char *error_text) {
  json_value_t *item;

  if (!results) {
    return -1;
  }
  item = turbo_json_create_object();
  if (!item) {
    return -1;
  }
  turbo_json_object_set_number(item, "index", (double)index);
  turbo_json_object_set_bool(item, "ok", ok ? 1 : 0);
  if (summary_json && *summary_json) {
    turbo_json_object_add(item, "summary", *summary_json);
    *summary_json = NULL;
  } else {
    turbo_json_object_set_null(item, "summary");
  }
  if (output_text) {
    turbo_json_object_set_string(item, "output_text", output_text);
  } else {
    turbo_json_object_set_null(item, "output_text");
  }
  if (error_text) {
    turbo_json_object_set_string(item, "error", error_text);
  } else {
    turbo_json_object_set_null(item, "error");
  }
  turbo_json_array_add(results, item);
  return 0;
}

CXX_C_API int turbo_agent_session_start_messages(turbo_agent_session_t *session,
                                                 const json_value_t *messages,
                                                 const turbo_graph_run_options_t *options,
                                                 json_value_t **out_summary_json,
                                                 json_value_t **out_state) {
  json_value_t *state;
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_messages_state_with_memory_json_value(
        session, messages, session->memory_namespace);
  } else {
    state = turbo_agent_session_create_input_messages_state_json_value(messages);
  }
  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options, NULL, NULL,
                                        out_summary_json, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_messages_stream(
    turbo_agent_session_t *session, const json_value_t *messages,
    const turbo_graph_run_options_t *options, turbo_event_stream_mode_t stream_mode,
    turbo_event_sink_json_value_fn event_sink, void *event_sink_user_data,
    json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *state;
  turbo_event_stream_filter_t filter = {0};
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_messages_state_with_memory_json_value(
        session, messages, session->memory_namespace);
  } else {
    state = turbo_agent_session_create_input_messages_state_json_value(messages);
  }
  if (!state) {
    return -1;
  }
  if (event_sink) {
    filter.mode = stream_mode;
    filter.sink = event_sink;
    filter.sink_user_data = event_sink_user_data;
    rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options,
                                          turbo_event_stream_filter_sink_json_value, &filter,
                                          out_summary_json, out_state);
  } else {
    rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options, NULL,
                                          NULL, out_summary_json, out_state);
  }
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_text(turbo_agent_session_t *session, const char *user_text,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_summary_json,
                                             json_value_t **out_state) {
  json_value_t *state;
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_state_with_memory_json_value(
        session, user_text, session->memory_namespace);
  } else {
    state = turbo_agent_session_create_input_state_json_value(user_text);
  }
  if (!state) {
    return -1;
  }
  rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options, NULL, NULL,
                                        out_summary_json, out_state);
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API int turbo_agent_session_start_text_stream(
    turbo_agent_session_t *session, const char *user_text, const turbo_graph_run_options_t *options,
    turbo_event_stream_mode_t stream_mode, turbo_event_sink_json_value_fn event_sink,
    void *event_sink_user_data, json_value_t **out_summary_json, json_value_t **out_state) {
  json_value_t *state;
  turbo_event_stream_filter_t filter = {0};
  int rc;

  if (!session) {
    return -1;
  }
  if (session->memory_namespace && session->memory_namespace[0] != '\0') {
    state = turbo_agent_session_create_input_state_with_memory_json_value(
        session, user_text, session->memory_namespace);
  } else {
    state = turbo_agent_session_create_input_state_json_value(user_text);
  }
  if (!state) {
    return -1;
  }
  if (event_sink) {
    filter.mode = stream_mode;
    filter.sink = event_sink;
    filter.sink_user_data = event_sink_user_data;
    rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options,
                                          turbo_event_stream_filter_sink_json_value, &filter,
                                          out_summary_json, out_state);
  } else {
    rc = turbo_agent_session_start_preset(session, session->workflow_kind, state, options, NULL,
                                          NULL, out_summary_json, out_state);
  }
  turbo_runtime_json_destroy(state);
  return rc;
}

CXX_C_API char *turbo_agent_session_result_text(const json_value_t *state) {
  json_value_t *json_state;
  const char *text;
  char *copy = NULL;
  size_t len;

  if (!state) {
    return NULL;
  }
  json_state = turbo_json_clone(state);
  if (!json_state) {
    return NULL;
  }
  text = turbo_agent_state_final_answer_text(json_state);
  if (text && text[0] != '\0') {
    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (copy) {
      memcpy(copy, text, len + 1);
    }
  }
  turbo_free_json(&json_state);
  return copy;
}

CXX_C_API int turbo_agent_session_invoke_text(turbo_agent_session_t *session, const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              char **out_text, json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!session || !out_text) {
    return -1;
  }
  *out_text = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_text(session, user_text, options, out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_json_destroy(out_state);
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_json_destroy(out_state);
  if (!text) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_json(turbo_agent_session_t *session, const char *user_text,
                                              const turbo_graph_run_options_t *options,
                                              json_value_t **out_json,
                                              json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!session || !out_json) {
    return -1;
  }
  *out_json = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_text(session, user_text, options, out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_json_destroy(out_state);
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return rc;
  }
  json_state = turbo_json_clone(out_state);
  turbo_runtime_json_destroy(out_state);
  if (!json_state) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &json);
  turbo_free_json(&json_state);
  if (rc != 0 || !json) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_messages_text(turbo_agent_session_t *session,
                                                       const json_value_t *messages,
                                                       const turbo_graph_run_options_t *options,
                                                       char **out_text,
                                                       json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  char *text;
  int rc;

  if (!session || !out_text) {
    return -1;
  }
  *out_text = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_messages(session, messages, options, out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_json_destroy(out_state);
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return rc;
  }
  text = turbo_agent_session_result_text(out_state);
  turbo_runtime_json_destroy(out_state);
  if (!text) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return -1;
  }
  *out_text = text;
  return 0;
}

CXX_C_API int turbo_agent_session_invoke_messages_json(turbo_agent_session_t *session,
                                                       const json_value_t *messages,
                                                       const turbo_graph_run_options_t *options,
                                                       json_value_t **out_json,
                                                       json_value_t **out_summary_json) {
  json_value_t *out_state = NULL;
  json_value_t *json = NULL;
  json_value_t *json_state = NULL;
  int rc;

  if (!session || !out_json) {
    return -1;
  }
  *out_json = NULL;
  if (out_summary_json) {
    *out_summary_json = NULL;
  }
  rc = turbo_agent_session_start_messages(session, messages, options, out_summary_json, &out_state);
  if (rc != 0) {
    turbo_runtime_json_destroy(out_state);
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return rc;
  }
  json_state = turbo_json_clone(out_state);
  turbo_runtime_json_destroy(out_state);
  if (!json_state) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    return -1;
  }
  rc = turbo_agent_state_parse_final_output_json(json_state, &json);
  turbo_free_json(&json_state);
  if (rc != 0 || !json) {
    if (out_summary_json) {
      turbo_free_json(out_summary_json);
    }
    turbo_free_json(&json);
    return -1;
  }
  *out_json = json;
  return 0;
}

CXX_C_API int turbo_agent_session_batch_text(turbo_agent_session_t *session,
                                             const char *const *user_texts, size_t count,
                                             const turbo_graph_run_options_t *options,
                                             json_value_t **out_results_json) {
  json_value_t *results;
  size_t i;

  if (!session || (!user_texts && count > 0) || !out_results_json) {
    return -1;
  }
  *out_results_json = NULL;
  results = turbo_json_create_array();
  if (!results) {
    return -1;
  }
  for (i = 0; i < count; ++i) {
    json_value_t *summary = NULL;
    char *text = NULL;
    int rc;

    if (!user_texts[i]) {
      if (turbo_agent_session_batch_add_text_result(results, i, 0, NULL, NULL, "invalid input") !=
          0) {
        turbo_free_json(&results);
        return -1;
      }
      continue;
    }

    rc = turbo_agent_session_invoke_text(session, user_texts[i], options, &text, &summary);
    if (turbo_agent_session_batch_add_text_result(results, i, rc == 0, &summary,
                                                  rc == 0 ? text : NULL,
                                                  rc == 0 ? NULL : "invoke failed") != 0) {
      free(text);
      turbo_free_json(&summary);
      turbo_free_json(&results);
      return -1;
    }
    free(text);
    turbo_free_json(&summary);
  }
  *out_results_json = results;
  return 0;
}

CXX_C_API int turbo_agent_session_memory_get(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             char **out_value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->get) {
    return -1;
  }
  return turbo_agent_memory_get(store, memory_namespace, key, out_value_json);
}

CXX_C_API int turbo_agent_session_memory_put(const turbo_agent_session_t *session,
                                             const char *memory_namespace, const char *key,
                                             const char *value_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->put) {
    return -1;
  }
  return turbo_agent_memory_put(store, memory_namespace, key, value_json);
}

CXX_C_API int turbo_agent_session_memory_put_context(const turbo_agent_session_t *session,
                                                     const char *memory_namespace, const char *key,
                                                     const char *scope, const char *path,
                                                     const char *text) {
  json_value_t *payload;
  char *serialized;
  int rc;

  if (!scope || scope[0] == '\0' || !text || text[0] == '\0') {
    return -1;
  }
  payload = turbo_json_create_object();
  if (!payload) {
    return -1;
  }
  turbo_json_object_set_string(payload, "scope", scope);
  turbo_json_object_set_string(payload, "path", path ? path : "");
  turbo_json_object_set_string(payload, "text", text);
  serialized = turbo_json_serialize(payload, NULL);
  turbo_free_json(&payload);
  if (!serialized) {
    return -1;
  }
  rc = turbo_agent_session_memory_put(session, memory_namespace, key, serialized);
  turbo_json_serialize_free(serialized);
  return rc;
}

CXX_C_API int turbo_agent_session_memory_delete(const turbo_agent_session_t *session,
                                                const char *memory_namespace, const char *key) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->remove) {
    return -1;
  }
  return turbo_agent_memory_delete(store, memory_namespace, key);
}

CXX_C_API int turbo_agent_session_memory_list(const turbo_agent_session_t *session,
                                              const char *namespace_prefix,
                                              json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->list) {
    return -1;
  }
  return turbo_agent_memory_list(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_list_records(const turbo_agent_session_t *session,
                                                      const char *namespace_prefix,
                                                      json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_list_records(store, namespace_prefix, out_records_json);
}

CXX_C_API int turbo_agent_session_memory_get_record(const turbo_agent_session_t *session,
                                                    const char *memory_namespace, const char *key,
                                                    json_value_t **out_record_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->get) {
    return -1;
  }
  return turbo_agent_memory_get_record(store, memory_namespace, key, out_record_json);
}

CXX_C_API int turbo_agent_session_memory_put_record(const turbo_agent_session_t *session,
                                                    const json_value_t *record_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || !store->put) {
    return -1;
  }
  return turbo_agent_memory_put_record(store, record_json);
}

CXX_C_API int turbo_agent_session_memory_validate_record(const json_value_t *record_json) {
  return turbo_agent_memory_validate_record(record_json);
}

CXX_C_API int turbo_agent_session_memory_query_records(const turbo_agent_session_t *session,
                                                       const char *namespace_prefix,
                                                       const char *kind, const char *key_prefix,
                                                       const char *text_substring,
                                                       json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_query_records(store, namespace_prefix, kind, key_prefix, text_substring,
                                          out_records_json);
}

CXX_C_API int
turbo_agent_session_memory_query_records_ex(const turbo_agent_session_t *session,
                                            const turbo_agent_memory_query_options_t *options,
                                            json_value_t **out_records_json) {
  const turbo_agent_memory_store_t *store = turbo_agent_session_memory_store(session);

  if (!store || (!store->list && !store->query)) {
    return -1;
  }
  return turbo_agent_memory_query_records_ex(store, options, out_records_json);
}

CXX_C_API int turbo_agent_session_load_memory_context(const turbo_agent_session_t *session,
                                                      json_value_t *state,
                                                      const char *namespace_prefix) {
  json_value_t *records = NULL;
  size_t i;
  int rc;

  if (!session || !state) {
    return -1;
  }
  rc = turbo_agent_session_memory_query_records(session, namespace_prefix, "context", NULL, NULL,
                                                &records);
  if (rc != 0 || !records || turbo_json_type(records) != TURBO_JSON_ARRAY) {
    turbo_free_json(&records);
    return -1;
  }
  for (i = 0; i < turbo_json_array_size(records); ++i) {
    if (turbo_agent_session_load_memory_context_record(state, turbo_json_array_get(records, i)) !=
        0) {
      turbo_free_json(&records);
      return -1;
    }
  }
  turbo_free_json(&records);
  return 0;
}
