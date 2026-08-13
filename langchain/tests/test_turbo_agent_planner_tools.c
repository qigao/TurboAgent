#include "tinytest.h"
#include "turbo_agent_app.h"
#include "turbo_agent_runtime.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

typedef struct planner_tool_test_state_s {
  size_t transport_calls;
  size_t tool_calls;
  int saw_tool_schema;
  int saw_tool_result;
} planner_tool_test_state_t;

static int planner_tool_test_copy(const char *source, char **out_copy) {
  size_t size;

  if (!source || !out_copy) {
    return -1;
  }
  size = strlen(source) + 1;
  *out_copy = (char *)malloc(size);
  if (!*out_copy) {
    return -1;
  }
  memcpy(*out_copy, source, size);
  return 0;
}

static int planner_tool_test_handler(const char *arguments_json, char **out_output,
                                     void *user_data) {
  planner_tool_test_state_t *state = (planner_tool_test_state_t *)user_data;
  static const char output[] = "{\"context\":\"ready\"}";

  if (!state || !out_output || !arguments_json || strstr(arguments_json, "planner") == NULL) {
    return -1;
  }
  state->tool_calls++;
  return planner_tool_test_copy(output, out_output);
}

static int planner_tool_test_transport(const char *request_json, char **out_response_json,
                                       void *user_data) {
  planner_tool_test_state_t *state = (planner_tool_test_state_t *)user_data;
  static const char tool_call_response[] =
      "{\"id\":\"resp_plan_tool\",\"output\":[{\"type\":\"function_call\","
      "\"call_id\":\"call_plan_context\",\"name\":\"inspect_plan_context\","
      "\"arguments\":\"{\\\"phase\\\":\\\"planner\\\"}\"}]}";
  static const char plan_response[] =
      "{\"id\":\"resp_plan_done\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"{\\\"steps\\\":[\\\"finish task\\\"]}\"}]}]}";
  static const char executor_response[] =
      "{\"id\":\"resp_exec_done\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"step done\"}]}]}";

  if (!state || !request_json || !out_response_json) {
    return -1;
  }
  state->transport_calls++;
  if (state->transport_calls == 1) {
    state->saw_tool_schema = strstr(request_json, "inspect_plan_context") != NULL;
    return planner_tool_test_copy(tool_call_response, out_response_json);
  }
  if (state->transport_calls == 2) {
    state->saw_tool_result =
        strstr(request_json, "context") != NULL && strstr(request_json, "ready") != NULL;
    return planner_tool_test_copy(plan_response, out_response_json);
  }
  if (state->transport_calls == 3 && strstr(request_json, "Execute plan step") != NULL) {
    return planner_tool_test_copy(executor_response, out_response_json);
  }
  return -1;
}

static int replanner_tool_test_transport(const char *request_json, char **out_response_json,
                                         void *user_data) {
  planner_tool_test_state_t *state = (planner_tool_test_state_t *)user_data;
  static const char initial_plan_response[] =
      "{\"id\":\"resp_initial_plan\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"{\\\"steps\\\":[\\\"failing task\\\"]}\"}]}]}";
  static const char failed_executor_response[] =
      "{\"id\":\"resp_exec_failed\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"FAILED: missing context\"}]}]}";
  static const char tool_call_response[] =
      "{\"id\":\"resp_replan_tool\",\"output\":[{\"type\":\"function_call\","
      "\"call_id\":\"call_replan_context\",\"name\":\"inspect_plan_context\","
      "\"arguments\":\"{\\\"phase\\\":\\\"planner\\\"}\"}]}";
  static const char replanned_response[] =
      "{\"id\":\"resp_replan_done\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"{\\\"steps\\\":[\\\"recovered task\\\"]}\"}]}]}";
  static const char successful_executor_response[] =
      "{\"id\":\"resp_exec_recovered\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"recovered\"}]}]}";

  if (!state || !request_json || !out_response_json) {
    return -1;
  }
  state->transport_calls++;
  if (state->transport_calls == 1) {
    return planner_tool_test_copy(initial_plan_response, out_response_json);
  }
  if (state->transport_calls == 2 && strstr(request_json, "Execute plan step") != NULL) {
    return planner_tool_test_copy(failed_executor_response, out_response_json);
  }
  if (state->transport_calls == 3 &&
      strstr(request_json, "Replan from the current failure") != NULL) {
    state->saw_tool_schema = strstr(request_json, "inspect_plan_context") != NULL;
    return planner_tool_test_copy(tool_call_response, out_response_json);
  }
  if (state->transport_calls == 4) {
    state->saw_tool_result =
        strstr(request_json, "context") != NULL && strstr(request_json, "ready") != NULL;
    return planner_tool_test_copy(replanned_response, out_response_json);
  }
  if (state->transport_calls == 5 && strstr(request_json, "Execute plan step") != NULL) {
    return planner_tool_test_copy(successful_executor_response, out_response_json);
  }
  return -1;
}

static int executor_tool_test_transport(const char *request_json, char **out_response_json,
                                        void *user_data) {
  planner_tool_test_state_t *state = (planner_tool_test_state_t *)user_data;
  static const char plan_response[] =
      "{\"id\":\"resp_exec_tool_plan\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"{\\\"steps\\\":[\\\"inspect then finish\\\"]}\"}]}]}";
  static const char tool_call_response[] =
      "{\"id\":\"resp_exec_tool\",\"output\":[{\"type\":\"function_call\","
      "\"call_id\":\"call_exec_context\",\"name\":\"inspect_plan_context\","
      "\"arguments\":\"{\\\"phase\\\":\\\"planner-executor\\\"}\"}]}";
  static const char done_response[] =
      "{\"id\":\"resp_exec_tool_done\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"executor tool done\"}]}]}";

  if (!state || !request_json || !out_response_json) {
    return -1;
  }
  state->transport_calls++;
  if (state->transport_calls == 1) {
    state->saw_tool_schema = strstr(request_json, "inspect_plan_context") != NULL;
    return planner_tool_test_copy(plan_response, out_response_json);
  }
  if (state->transport_calls == 2 && strstr(request_json, "Execute plan step") != NULL) {
    return planner_tool_test_copy(tool_call_response, out_response_json);
  }
  if (state->transport_calls == 3) {
    state->saw_tool_result =
        strstr(request_json, "context") != NULL && strstr(request_json, "ready") != NULL;
    return planner_tool_test_copy(done_response, out_response_json);
  }
  return -1;
}

spec("turbo agent planner tools") {
  it("executes planner tool calls before committing the plan") {
    planner_tool_test_state_t state = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {
        "inspect_plan_context",
        "Inspect read-only context for planning.",
        "{\"type\":\"object\",\"properties\":{\"phase\":{\"type\":\"string\"}},"
        "\"required\":[\"phase\"]}",
        NULL,
        1,
        planner_tool_test_handler,
        NULL,
        &state,
        NULL};
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *result_state = NULL;
    json_value_t *summary = NULL;
    char *text = NULL;
    int invoke_result;

    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = planner_tool_test_transport;
    agent_config.transport_user_data = &state;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    invoke_result = turbo_agent_app_start_text(app, "plan with inspected context", NULL, &summary,
                                               &result_state);
    check_true(state.saw_tool_schema);
    check_not_null(result_state);
    check_size_eq(state.tool_calls, 1);
    check_true(state.saw_tool_result);
    check_size_eq(state.transport_calls, 3);
    check_int_eq(invoke_result, 0);
    text = turbo_agent_app_result_text(result_state);
    check_str_eq(text, "step done");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");

    free(text);
    turbo_runtime_json_destroy(summary);
    turbo_runtime_json_destroy(result_state);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("executes replanner tool calls in the planner substate") {
    planner_tool_test_state_t state = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    check_not_null(registry);
    definition.name = "inspect_plan_context";
    definition.description = "Inspect read-only context for planning.";
    definition.parameters_json =
        "{\"type\":\"object\",\"properties\":{\"phase\":{\"type\":\"string\"}},"
        "\"required\":[\"phase\"]}";
    definition.strict = 1;
    definition.handler = planner_tool_test_handler;
    definition.user_data = &state;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = replanner_tool_test_transport;
    agent_config.transport_user_data = &state;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(turbo_agent_app_invoke_text(app, "recover after failure", NULL, &text, &summary),
                 0);
    check_str_eq(text, "recovered");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_size_eq(state.transport_calls, 5);
    check_size_eq(state.tool_calls, 1);
    check_true(state.saw_tool_schema);
    check_true(state.saw_tool_result);

    free(text);
    turbo_runtime_json_destroy(summary);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }

  it("executes tool calls in the executor substate") {
    planner_tool_test_state_t state = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {0};
    turbo_agent_config_t agent_config = {0};
    turbo_agent_session_config_t session_config = {0};
    turbo_agent_app_config_t app_config = {0};
    turbo_agent_app_t *app;
    json_value_t *summary = NULL;
    char *text = NULL;

    check_not_null(registry);
    definition.name = "inspect_plan_context";
    definition.description = "Inspect read-only context for planning and execution.";
    definition.parameters_json =
        "{\"type\":\"object\",\"properties\":{\"phase\":{\"type\":\"string\"}},"
        "\"required\":[\"phase\"]}";
    definition.strict = 1;
    definition.handler = planner_tool_test_handler;
    definition.user_data = &state;
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    agent_config.model = "gpt-5.4";
    agent_config.transport_fn = executor_tool_test_transport;
    agent_config.transport_user_data = &state;
    agent_config.tool_registry = registry;
    session_config.agent_config = agent_config;
    session_config.runtime_store = turbo_agent_runtime_store_memory_create();
    session_config.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    app_config.session_config = &session_config;
    app = turbo_agent_app_create(&app_config);
    check_not_null(app);

    check_int_eq(
        turbo_agent_app_invoke_text(app, "use a tool while executing", NULL, &text, &summary), 0);
    check_str_eq(text, "executor tool done");
    check_str_eq(turbo_json_get_string(summary, "status"), "completed");
    check_size_eq(state.transport_calls, 3);
    check_size_eq(state.tool_calls, 1);
    check_true(state.saw_tool_schema);
    check_true(state.saw_tool_result);

    free(text);
    turbo_runtime_json_destroy(summary);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }
}
