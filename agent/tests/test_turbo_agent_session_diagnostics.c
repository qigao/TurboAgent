#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_policy.h"
#include "turbo_model_provider.h"
#include "turbo_tool_registry.h"

static int transport_calls;

static int diagnostic_transport(const char *request, char **out, void *data) {
  (void)request;
  (void)out;
  (void)data;
  ++transport_calls;
  return -1;
}

static int diagnostic_tool(const char *arguments, char **out, void *data) {
  (void)arguments;
  (void)out;
  (void)data;
  return -1;
}

static int grant_custom_tools(turbo_agent_session_t *session) {
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  policy.allow_custom_tools = 1;
  return turbo_agent_set_tool_policy(turbo_agent_session_agent(session), &policy);
}

static int add_tool(turbo_tool_registry_t *registry, const char *name) {
  turbo_tool_definition_t definition = {0};
  definition.name = name;
  definition.description = "Diagnostic fixture tool; never executed.";
  definition.parameters_json = "{\"type\":\"object\",\"properties\":{}}";
  definition.handler = diagnostic_tool;
  return turbo_tool_registry_add(registry, &definition);
}

spec("session diagnostic snapshots") {
  static turbo_agent_session_t *session;
  static turbo_tool_registry_t *registry;
  static json_value_t *capabilities, *schemas, *report, *next;

  before_each() { transport_calls = 0; }
  after_each() {
    json_free(capabilities);
    json_free(schemas);
    json_free(report);
    json_free(next);
    capabilities = schemas = report = next = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
  }

  it("should return independent reports and refresh facts without executing the agent") {
    turbo_agent_session_config_t config = {0};
    json_value_t *entry;
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(add_tool(registry, "tools.first"), TURBO_TOOL_OK);
    config.agent_config.model = "diagnostic-model";
    config.agent_config.api_key = "test-secret-not-for-report";
    config.agent_config.transport_fn = diagnostic_transport;
    config.agent_config.tool_registry = registry;
    config.agent_config.structured_output_name = "answer";
    config.agent_config.structured_output_schema_json = "{\"type\":\"object\"}";
    config.agent_config.structured_output_strict = 1;
    config.memory_namespace = "project";
    config.thread_id = "diagnostic-thread";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(grant_custom_tools(session), 0);

    check_equal(turbo_agent_session_get_capabilities(session, &capabilities), 0);
    check_equal(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    check_equal(turbo_agent_session_get_startup_diagnostics(session, &report), 0);
    check_true(json_get_bool(report, "ok", false));
    check_null(json_object_get(capabilities, "api_key"));
    check_true(json_get_bool(capabilities, "has_api_key", false));

    /* Mutating one caller-owned report must not change the Session or another report. */
    entry = json_array_get(json_object_get(schemas, "registry"), 0);
    check_equal(turbo_runtime_json_object_set_string(entry, "name", "caller-change"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(capabilities, "model", "caller-model"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(add_tool(registry, "tools.second"), TURBO_TOOL_OK);
    check_equal(turbo_agent_session_get_startup_diagnostics(session, &next), 0);
    check_equal(json_get_int(json_object_get(next, "capabilities"), "tool_count", -1), 2);
    check_equal(json_array_size(json_object_get(json_object_get(next, "tool_schemas"), "registry")),
                2);
    check_equal(json_get_string(json_object_get(next, "capabilities"), "model"),
                "diagnostic-model");
    check_equal(transport_calls, 0);
    check_equal(turbo_agent_session_thread_id(session), "diagnostic-thread");
    check_null(turbo_agent_session_last_run_id(session));

    turbo_agent_session_destroy(session);
    session = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    check_equal(json_get_string(capabilities, "memory_namespace"), "project");
    check_equal(json_get_string(json_array_get(json_object_get(schemas, "registry"), 0), "name"),
                "caller-change");
    check_equal(json_get_int(json_object_get(report, "capabilities"), "tool_count", -1), 1);
    entry = json_array_get(json_object_get(json_object_get(report, "tool_schemas"), "registry"), 0);
    check_equal(json_get_string(entry, "name"), "tools.first");
    entry = json_object_get(report, "structured_output");
    check_true(json_get_bool(entry, "configured", false));
    check_true(json_get_bool(entry, "strict", false));
    check_equal(json_get_string(entry, "name"), "answer");
  }


  it("should hide legacy tools from diagnostics until the Host grants custom_tools") {
    turbo_agent_session_config_t config = {0};
    json_value_t *registry_json;
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(add_tool(registry, "tools.private"), TURBO_TOOL_OK);
    config.agent_config.model = "diagnostic-model";
    config.agent_config.transport_fn = diagnostic_transport;
    config.agent_config.tool_registry = registry;
    session = turbo_agent_session_create(&config);
    check_not_null(session);

    check_equal(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    registry_json = json_object_get(schemas, "registry");
    check_equal(json_array_size(registry_json), 0);
    check_equal(json_array_size(json_object_get(schemas, "openai_responses")), 0);
    check_equal(json_array_size(json_object_get(schemas, "openai_chat")), 0);
    check_equal(json_array_size(json_object_get(schemas, "openai_compatible_chat")), 0);
    check_equal(json_array_size(json_object_get(schemas, "anthropic")), 0);
    json_free(schemas);
    schemas = NULL;

    check_equal(grant_custom_tools(session), 0);
    check_equal(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    check_equal(json_array_size(json_object_get(schemas, "registry")), 1);
    check_equal(json_get_string(json_array_get(json_object_get(schemas, "registry"), 0), "name"),
                "tools.private");
    check_equal(transport_calls, 0);
  }

  it("should preserve provider aliases and the existing custom-provider default") {
    static const struct {
      const char *provider;
      const char *format;
    } cases[] = {
        {"openai_responses", "openai_responses"},
        {"openai_chat_completions", "openai_chat"},
        {"openai_chat", "openai_chat"},
        {"openai_compatible_chat_completions", "openai_compatible_chat"},
        {"openai_compatible_chat", "openai_compatible_chat"},
        {"anthropic_messages", "anthropic"},
        {"anthropic", "anthropic"},
        {"custom-provider", "openai_responses"},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      turbo_agent_session_config_t config = {0};
      turbo_model_provider_t provider = *turbo_model_provider_openai_responses();
      json_value_t *status, *selected;
      provider.name = cases[i].provider;
      config.agent_config.model = "diagnostic-model";
      config.agent_config.provider = &provider;
      config.agent_config.transport_fn = diagnostic_transport;
      session = turbo_agent_session_create(&config);
      check_not_null(session);
      check_equal(turbo_agent_session_get_startup_diagnostics(session, &report), 0);
      check_true(json_get_bool(report, "ok", false));
      status = json_object_get(report, "tool_schema_status");
      check_equal(json_get_string(status, "selected_format"), cases[i].format);
      selected = json_object_get(status, cases[i].format);
      check_true(json_get_bool(selected, "selected", false));
      check_true(json_get_bool(selected, "ok", false));
      check_true(json_is_null(json_object_get(selected, "reason")));
      json_free(report);
      report = NULL;
      turbo_agent_session_destroy(session);
      session = NULL;
    }
    check_equal(transport_calls, 0);
  }

  it("should preserve error order while hiding ungranted tool schemas") {
    turbo_agent_session_config_t config = {0};
    json_value_t *errors;
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(add_tool(registry, "tools.first"), TURBO_TOOL_OK);
    check_equal(add_tool(registry, "tools/first"), TURBO_TOOL_OK);
    config.agent_config.model = "diagnostic-model";
    config.agent_config.transport_fn = diagnostic_transport;
    config.agent_config.provider = turbo_model_provider_openai_compatible_chat_completions();
    config.agent_config.tool_registry = registry;
    config.agent_config.structured_output_schema_json = "[]";
    config.workflow_kind = (turbo_agent_session_workflow_kind_t)999;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_session_get_tool_schemas(session, &schemas), 0);
    check_not_null(schemas);
    check_equal(json_array_size(json_object_get(schemas, "registry")), 0);
    json_free(schemas);
    schemas = NULL;
    check_equal(turbo_agent_session_get_startup_diagnostics(session, &report), 0);
    check_false(json_get_bool(report, "ok", true));
    errors = json_object_get(report, "errors");
    check_equal(json_array_size(errors), 2);
    check_equal(json_string(json_array_get(errors, 0)), "invalid_workflow_kind");
    check_equal(json_string(json_array_get(errors, 1)), "invalid_structured_output_schema");
    check_equal(json_get_string(json_object_get(report, "capabilities"), "workflow_kind"),
                "unknown");
    check_not_null(json_object_get(report, "tool_schemas"));
    check_equal(json_array_size(json_object_get(
                      json_object_get(report, "tool_schemas"), "registry")), 0);
    check_equal(transport_calls, 0);
  }
}
