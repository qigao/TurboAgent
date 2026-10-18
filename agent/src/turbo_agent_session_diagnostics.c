#include "turbo_agent_session_diagnostics_internal.h"
#include "turbo_runtime_json.h"
#include "turbo_tool_schema.h"
#include "turbo_agent_tool_authority_internal.h"

#include <string.h>

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

/* Scalar constructors are consumed on both success and failure. */
static int diagnostic_set(json_value_t *object, const char *key, json_value_t *value) {
  if (turbo_runtime_json_object_set(object, key, value) != TURBO_RUNTIME_JSON_OK) {
    json_free(value);
    return -1;
  }
  return 0;
}

static int diagnostic_optional_string(json_value_t *object, const char *key, const char *value) {
  return diagnostic_set(object, key,
                        value && value[0] ? json_create_string(value) : json_create_null());
}

/* Named children stay with the caller unless insertion succeeds. */
static int turbo_agent_session_json_object_take(json_value_t *object, const char *key,
                                                json_value_t **value) {
  if (!value || turbo_runtime_json_object_set(object, key, *value) != TURBO_RUNTIME_JSON_OK) {
    return -1;
  }
  *value = NULL;
  return 0;
}

static int turbo_agent_session_json_array_add_string(json_value_t *array, const char *value) {
  json_value_t *string_value = value ? json_create_string(value) : NULL;
  if (turbo_runtime_json_array_append(array, string_value) != TURBO_RUNTIME_JSON_OK) {
    json_free(string_value);
    return -1;
  }
  return 0;
}

static const char *
turbo_agent_session_selected_tool_schema_format(const turbo_agent_session_diagnostic_view_t *view) {
  const char *provider_name = view ? view->provider_name : NULL;

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
  entry = json_create_object();
  if (!entry) {
    return -1;
  }
  if (diagnostic_set(entry, "ok", json_create_bool(ok ? true : false)) != 0) goto cleanup;
  if (diagnostic_set(entry, "selected", json_create_bool(selected ? true : false)) != 0)
    goto cleanup;
  if (!ok) {
    if (diagnostic_set(entry, "reason", json_create_string("invalid_tool_schema")) != 0)
      goto cleanup;
  } else {
    if (diagnostic_set(entry, "reason", json_create_null()) != 0) goto cleanup;
  }
  if (turbo_agent_session_json_object_take(status, format, &entry) != 0) goto cleanup;
  return 0;

cleanup:
  json_free(entry);
  return -1;
}

static int
turbo_agent_session_get_tool_schema_status(const turbo_agent_session_diagnostic_view_t *view,
                                           json_value_t **out_status_json, int *out_selected_ok) {
  static const char *formats[] = {"openai_responses", "openai_chat", "openai_compatible_chat",
                                  "anthropic"};
  const turbo_tool_registry_t *registry;
  turbo_tool_registry_t *visible_registry = NULL;
  const char *selected_format;
  json_value_t *status = NULL;
  size_t i;
  int selected_ok = 0;
  int rc = -1;

  if (!view || !out_status_json || !out_selected_ok) {
    return -1;
  }
  *out_status_json = NULL;
  *out_selected_ok = 0;

  registry = view->registry;
  if (!registry || turbo_agent_tool_authority_project(&view->tool_policy, registry,
                                                       &visible_registry) != TURBO_TOOL_OK) {
    return -1;
  }
  registry = visible_registry;
  selected_format = turbo_agent_session_selected_tool_schema_format(view);
  status = json_create_object();
  if (!status) {
    turbo_tool_registry_destroy(visible_registry);
    return -1;
  }

  if (diagnostic_set(status, "selected_format", json_create_string(selected_format)) != 0)
    goto cleanup;
  for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    json_value_t *schema = turbo_agent_session_build_tool_schema_format(registry, formats[i]);
    int format_ok = schema ? 1 : 0;
    int selected = strcmp(selected_format, formats[i]) == 0;

    if (selected) {
      selected_ok = format_ok;
    }
    json_free(schema);
    schema = NULL;
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
  json_free(status);
  status = NULL;
  turbo_tool_registry_destroy(visible_registry);
  return rc;
}

static int
turbo_agent_session_get_workflow_diagnostics(const turbo_agent_session_diagnostic_view_t *view,
                                             json_value_t **out_workflow_json,
                                             const char **out_error_code) {
  json_value_t *workflow = NULL;
  const char *error_code = NULL;
  int requires_knowledge_store = 0;
  int requires_retriever = 0;
  int ready = 1;

  if (!view || !out_workflow_json || !out_error_code) {
    return -1;
  }
  *out_workflow_json = NULL;
  *out_error_code = NULL;

  workflow = json_create_object();
  if (!workflow) {
    return -1;
  }

  switch (view->workflow_kind) {
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

  if (ready && !view->has_runtime) {
    ready = 0;
    error_code = "missing_runtime";
  }
  if (ready && !view->has_agent) {
    ready = 0;
    error_code = "missing_agent";
  }
  if (ready && requires_knowledge_store && !view->has_knowledge_store) {
    ready = 0;
    error_code = "missing_knowledge_store";
  }
  if (ready && requires_retriever && !view->has_retriever) {
    ready = 0;
    error_code = "missing_retriever";
  }

  if (diagnostic_set(workflow, "ok", json_create_bool(ready ? true : false)) != 0) goto cleanup;
  if (diagnostic_set(workflow, "workflow_kind_id",
                     json_create_number((double)view->workflow_kind)) != 0)
    goto cleanup;
  if (diagnostic_set(
          workflow, "workflow_kind",
          json_create_string(turbo_agent_session_workflow_kind_name(view->workflow_kind))) != 0)
    goto cleanup;
  if (diagnostic_set(workflow, "requires_agent", json_create_bool(true)) != 0) goto cleanup;
  if (diagnostic_set(workflow, "requires_runtime", json_create_bool(true)) != 0) goto cleanup;
  if (diagnostic_set(workflow, "requires_knowledge_store",
                     json_create_bool(requires_knowledge_store ? true : false)) != 0)
    goto cleanup;
  if (diagnostic_set(workflow, "requires_retriever",
                     json_create_bool(requires_retriever ? true : false)) != 0)
    goto cleanup;
  if (error_code) {
    if (diagnostic_set(workflow, "reason", json_create_string(error_code)) != 0) goto cleanup;
  } else {
    if (diagnostic_set(workflow, "reason", json_create_null()) != 0) goto cleanup;
  }

  *out_workflow_json = workflow;
  *out_error_code = error_code;
  return 0;

cleanup:
  json_free(workflow);
  return -1;
}

static int turbo_agent_session_get_structured_output_diagnostics(
    const turbo_agent_session_diagnostic_view_t *view, json_value_t **out_structured_output_json,
    const char **out_error_code) {
  json_value_t *diagnostics = NULL;
  json_value_t *schema = NULL;
  const char *schema_json;
  const char *error_code = NULL;
  int configured;

  if (!view || !out_structured_output_json || !out_error_code) {
    return -1;
  }
  *out_structured_output_json = NULL;
  *out_error_code = NULL;

  diagnostics = json_create_object();
  if (!diagnostics) {
    return -1;
  }

  schema_json = (view->has_agent && view->structured_output_schema_json)
                    ? view->structured_output_schema_json
                    : NULL;
  configured = schema_json && schema_json[0] != '\0';
  if (diagnostic_set(diagnostics, "configured", json_create_bool(configured ? true : false)) != 0)
    goto cleanup;

  if (configured) {
    schema = json_parse(schema_json, strlen(schema_json));
  }
  if (configured && (!schema || json_type(schema) != JSON_OBJECT)) {
    error_code = "invalid_structured_output_schema";
  }

  if (diagnostic_set(diagnostics, "ok", json_create_bool(error_code ? false : true)) != 0)
    goto cleanup;
  if (view->has_agent && view->structured_output_name && view->structured_output_name[0] != '\0') {
    if (diagnostic_set(diagnostics, "name", json_create_string(view->structured_output_name)) != 0)
      goto cleanup;
  } else {
    if (diagnostic_set(diagnostics, "name", json_create_null()) != 0) goto cleanup;
  }
  if (diagnostic_set(diagnostics, "strict",
                     json_create_bool(
                         (view->has_agent && view->structured_output_strict) ? true : false)) != 0)
    goto cleanup;
  if (error_code) {
    if (diagnostic_set(diagnostics, "reason", json_create_string(error_code)) != 0) goto cleanup;
  } else {
    if (diagnostic_set(diagnostics, "reason", json_create_null()) != 0) goto cleanup;
  }

  json_free(schema);
  schema = NULL;
  *out_structured_output_json = diagnostics;
  *out_error_code = error_code;
  return 0;

cleanup:
  json_free(schema);
  json_free(diagnostics);
  return -1;
}

static int turbo_agent_session_build_capabilities(const turbo_agent_session_diagnostic_view_t *view,
                                                  json_value_t **out_capabilities_json) {
  json_value_t *capabilities;

  if (!view || !out_capabilities_json) {
    return -1;
  }
  *out_capabilities_json = NULL;
  capabilities = json_create_object();
  if (!capabilities) {
    return -1;
  }

  if (diagnostic_set(capabilities, "has_agent", json_create_bool(view->has_agent ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "has_runtime", json_create_bool(view->has_runtime ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "has_memory_store",
                     json_create_bool(view->has_memory_store ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "has_tool_registry", json_create_bool(view->registry ? 1 : 0)) !=
      0)
    goto cleanup;
  if (diagnostic_set(capabilities, "tool_count", json_create_number((double)view->tool_count)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "workflow_kind_id",
                     json_create_number((double)view->workflow_kind)) != 0)
    goto cleanup;
  if (diagnostic_set(
          capabilities, "workflow_kind",
          json_create_string(turbo_agent_session_workflow_kind_name(view->workflow_kind))) != 0)
    goto cleanup;
  if (diagnostic_optional_string(capabilities, "memory_namespace", view->memory_namespace) != 0)
    goto cleanup;
  if (diagnostic_optional_string(capabilities, "model", view->model) != 0) goto cleanup;
  if (diagnostic_optional_string(capabilities, "base_url", view->base_url) != 0) goto cleanup;
  if (diagnostic_optional_string(capabilities, "provider", view->provider_name) != 0) goto cleanup;
  if (diagnostic_set(capabilities, "has_api_key", json_create_bool(view->has_api_key ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "has_knowledge_store",
                     json_create_bool(view->has_knowledge_store ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(capabilities, "has_retriever",
                     json_create_bool(view->has_retriever ? 1 : 0)) != 0)
    goto cleanup;

  *out_capabilities_json = capabilities;
  return 0;

cleanup:
  json_free(capabilities);
  return -1;
}

static int turbo_agent_session_build_tool_schemas(const turbo_agent_session_diagnostic_view_t *view,
                                                  json_value_t **out_tool_schemas_json) {
  const turbo_tool_registry_t *registry;
  json_value_t *schemas = NULL;
  json_value_t *registry_json = NULL;
  json_value_t *openai_responses = NULL;
  json_value_t *openai_chat = NULL;
  json_value_t *openai_compatible_chat = NULL;
  json_value_t *anthropic = NULL;
  int rc = -1;

  if (!view || !out_tool_schemas_json) {
    return -1;
  }
  *out_tool_schemas_json = NULL;
  registry = view->registry;

  registry_json = turbo_tool_schema_build_registry_json_value(registry);
  if (!registry_json) {
    goto cleanup;
  }
  openai_responses = turbo_tool_schema_build_openai_tools(registry);
  openai_chat = turbo_tool_schema_build_openai_chat_tools(registry);
  openai_compatible_chat = turbo_tool_schema_build_openai_compatible_chat_tools(registry);
  anthropic = turbo_tool_schema_build_anthropic_tools(registry);
  schemas = json_create_object();
  if (!openai_responses || !openai_chat || !openai_compatible_chat || !anthropic || !schemas) {
    goto cleanup;
  }

  if (diagnostic_set(schemas, "has_tool_registry", json_create_bool(registry ? 1 : 0)) != 0)
    goto cleanup;
  if (diagnostic_set(schemas, "tool_count", json_create_number((double)view->tool_count)) != 0)
    goto cleanup;
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
  json_free(schemas);
  schemas = NULL;
  json_free(registry_json);
  registry_json = NULL;
  json_free(openai_responses);
  openai_responses = NULL;
  json_free(openai_chat);
  openai_chat = NULL;
  json_free(openai_compatible_chat);
  openai_compatible_chat = NULL;
  json_free(anthropic);
  anthropic = NULL;
  return rc;
}

static int
turbo_agent_session_build_startup_diagnostics(const turbo_agent_session_diagnostic_view_t *view,
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

  if (!view || !out_diagnostics_json) {
    return -1;
  }
  *out_diagnostics_json = NULL;

  diagnostics = json_create_object();
  errors = json_create_array();
  if (!diagnostics || !errors) {
    goto cleanup;
  }
  if (diagnostic_set(diagnostics, "schema_version", json_create_number(1.0)) != 0) goto cleanup;

  if (turbo_agent_session_build_capabilities(view, &capabilities) != 0 || !capabilities) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "capabilities_unavailable") != 0) {
      goto cleanup;
    }
    if (diagnostic_set(diagnostics, "capabilities", json_create_null()) != 0) goto cleanup;
  } else if (turbo_agent_session_json_object_take(diagnostics, "capabilities", &capabilities) !=
             0) {
    goto cleanup;
  }

  if (turbo_agent_session_get_workflow_diagnostics(view, &workflow, &workflow_error) != 0 ||
      !workflow) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "workflow_diagnostics_unavailable") !=
        0) {
      goto cleanup;
    }
    if (diagnostic_set(diagnostics, "workflow", json_create_null()) != 0) goto cleanup;
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

  if (turbo_agent_session_get_structured_output_diagnostics(view, &structured_output,
                                                            &structured_output_error) != 0 ||
      !structured_output) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(
            errors, "structured_output_diagnostics_unavailable") != 0) {
      goto cleanup;
    }
    if (diagnostic_set(diagnostics, "structured_output", json_create_null()) != 0) goto cleanup;
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

  if (turbo_agent_session_get_tool_schema_status(view, &tool_schema_status,
                                                 &selected_tool_schema_ok) != 0 ||
      !tool_schema_status) {
    ok = 0;
    if (turbo_agent_session_json_array_add_string(errors, "tool_schema_status_unavailable") != 0) {
      goto cleanup;
    }
    if (diagnostic_set(diagnostics, "tool_schema_status", json_create_null()) != 0) goto cleanup;
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

  if (turbo_agent_session_build_tool_schemas(view, &tool_schemas) != 0 || !tool_schemas) {
    if (!selected_tool_schema_ok) {
      ok = 0;
    }
    if (diagnostic_set(diagnostics, "tool_schemas", json_create_null()) != 0) goto cleanup;
  } else if (turbo_agent_session_json_object_take(diagnostics, "tool_schemas", &tool_schemas) !=
             0) {
    goto cleanup;
  }

  if (diagnostic_set(diagnostics, "ok", json_create_bool(ok ? 1 : 0)) != 0) goto cleanup;
  if (turbo_agent_session_json_object_take(diagnostics, "errors", &errors) != 0) goto cleanup;

  *out_diagnostics_json = diagnostics;
  diagnostics = NULL;
  rc = 0;

cleanup:
  json_free(diagnostics);
  diagnostics = NULL;
  json_free(errors);
  errors = NULL;
  json_free(capabilities);
  capabilities = NULL;
  json_free(tool_schemas);
  tool_schemas = NULL;
  json_free(tool_schema_status);
  tool_schema_status = NULL;
  json_free(workflow);
  workflow = NULL;
  json_free(structured_output);
  structured_output = NULL;
  return rc;
}

CXX_C_API int turbo_agent_session_get_capabilities(const turbo_agent_session_t *session,
                                                   json_value_t **out_capabilities_json) {
  turbo_agent_session_diagnostic_view_t view;
  if (!session || !out_capabilities_json) return -1;
  *out_capabilities_json = NULL;
  if (turbo_agent_session_diagnostic_view_internal(session, &view) != 0) return -1;
  return turbo_agent_session_build_capabilities(&view, out_capabilities_json);
}

CXX_C_API int turbo_agent_session_get_tool_schemas(const turbo_agent_session_t *session,
                                                   json_value_t **out_tool_schemas_json) {
  turbo_agent_session_diagnostic_view_t view;
  if (!session || !out_tool_schemas_json) return -1;
  *out_tool_schemas_json = NULL;
  if (turbo_agent_session_diagnostic_view_internal(session, &view) != 0) return -1;
  return turbo_agent_session_build_tool_schemas(&view, out_tool_schemas_json);
}

CXX_C_API int turbo_agent_session_get_startup_diagnostics(const turbo_agent_session_t *session,
                                                          json_value_t **out_diagnostics_json) {
  turbo_agent_session_diagnostic_view_t view;
  if (!session || !out_diagnostics_json) return -1;
  *out_diagnostics_json = NULL;
  if (turbo_agent_session_diagnostic_view_internal(session, &view) != 0) return -1;
  return turbo_agent_session_build_startup_diagnostics(&view, out_diagnostics_json);
}
