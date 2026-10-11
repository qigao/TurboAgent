#include "tinytest.h"
#include <json_parser.h>
#include <tstr.h>
#include "turbo_agent_app.h"
#include "turbo_agent_runtime.h"
#include "turbo_agent_evidence_diagram.h"
#include "turbo_agent_graph.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

typedef struct planner_tool_test_state_s {
  size_t transport_calls;
  size_t tool_calls;
  int saw_tool_schema;
  int saw_tool_result;
  int collision_case;
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

static int repeated_step_tool_transport(const char *request_json, char **out, void *user) {
  planner_tool_test_state_t *state = user;
  static const char plan[] =
      "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"first step\\\",\\\"second step\\\"]}\"}]}]}";
  static const char call[] =
      "{\"output\":[{\"type\":\"function_call\",\"call_id\":\"reused_call\","
      "\"name\":\"inspect_plan_context\",\"arguments\":\"{\\\"phase\\\":\\\"planner-executor\\\"}\"}]}";
  static const char single_plan[] =
      "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"{\\\"steps\\\":[\\\"single step\\\"]}\"}]}]}";
  static const char failed[] =
      "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"FAILED: replan required\"}]}]}";
  static const char done[] =
      "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"step done\"}]}]}";
  const char *sequence[][6] = {
      {plan, call, done, call, done, NULL},
      {call, single_plan, call, done, NULL, NULL},
      {single_plan, call, failed, single_plan, call, done},
      {call, single_plan, failed, call, single_plan, done}};
  if (!request_json || !out || !state || state->collision_case < 0 || state->collision_case > 3 ||
      state->transport_calls >= 6) return -1;
  const char *response = sequence[state->collision_case][state->transport_calls++];
  return response ? planner_tool_test_copy(response, out) : -1;
}

spec("planner step tool journal identity") {
  static planner_tool_test_state_t state;
  static turbo_tool_registry_t *registry;
  static turbo_agent_app_t *app;
  static json_value_t *summary, *journals, *result_state, *diagram;
  static char *text;
  static tstr checkpoint;
  before_each() {
    turbo_tool_definition_t definition = {0};
    turbo_agent_session_config_t session = {0};
    turbo_agent_app_config_t config = {0};
    registry = turbo_tool_registry_create(); check_not_null(registry);
    definition.name = "inspect_plan_context";
    definition.description = "Count distinct planned tool invocations";
    definition.parameters_json = "{\"type\":\"object\",\"properties\":{\"phase\":{\"type\":\"string\"}},\"required\":[\"phase\"]}";
    definition.strict = 1; definition.handler = planner_tool_test_handler; definition.user_data = &state;
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    session.agent_config.model = "test-model";
    session.agent_config.transport_fn = repeated_step_tool_transport;
    session.agent_config.transport_user_data = &state;
    session.agent_config.tool_registry = registry;
    session.runtime_store = turbo_agent_runtime_store_memory_create();
    session.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING;
    config.session_config = &session;
    app = turbo_agent_app_create(&config); check_not_null(app);
  }
  after_each() {
    free(text); text = NULL; json_free(summary); summary = NULL;
    json_free(journals); journals = NULL;
    json_free(result_state); result_state = NULL;
    json_free(diagram); diagram = NULL;
    tstr_free(checkpoint); checkpoint = NULL;
    turbo_agent_app_destroy(app); app = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    state = (planner_tool_test_state_t){0};
  }
  const struct { const char *name; size_t models; const char *first, *second; } cases[] = {
      {"distinct executor steps", 5, "executor:0:0", "executor:1:0"},
      {"planner and executor", 4, "planner:root:0", "executor:0:0"},
      {"executor replan generations", 6, "executor:0:0", "executor:1:0"},
      {"planner replan generations", 6, "planner:root:0", "planner:0:0"}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    it("executes an identical call once in each of %s", cases[i].name) {
      const char *first, *second;
      state.collision_case = (int)i;
      check_equal(turbo_agent_app_invoke_text(app, "execute both steps", NULL, &text, &summary), 0);
      check_equal(json_get_string(summary, "status"), "completed");
      check_equal(state.transport_calls, cases[i].models);
      check_equal(state.tool_calls, 2u);
      check_equal(turbo_agent_runtime_store_list_json(turbo_agent_session_runtime(turbo_agent_app_session(app)),
          "agent_tool_journal", "call_id", "reused_call", &journals), 0);
      check_equal(json_array_size(journals), 2u);
      first = json_get_string(json_array_get(journals, 0), "turn_key");
      second = json_get_string(json_array_get(journals, 1), "turn_key");
      check_true((!strcmp(first, cases[i].first) && !strcmp(second, cases[i].second)) ||
          (!strcmp(first, cases[i].second) && !strcmp(second, cases[i].first)));
      {
        const char *ids[] = {json_get_string(json_array_get(journals, 0), "journal_id"),
                            json_get_string(json_array_get(journals, 1), "journal_id")};
        const char *thread = json_get_string(json_array_get(journals, 0), "thread_id");
        size_t matched = 0, steps = 0, prepared = 0;
        check_equal(turbo_agent_thread_tool_diagram_ex(
            turbo_agent_session_runtime(turbo_agent_app_session(app)), thread, ids, 2,
            TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS, &diagram), SALTS_OK);
        const json_value_t *nodes = json_object_get(diagram, "nodes");
        for (size_t n = 0; n < json_array_size(nodes); ++n) {
          const char *status = json_get_string(json_array_get(nodes, n), "origin_status");
          if (status && !strcmp(status, "matched")) ++matched;
          const char *node_id = json_get_string(json_array_get(nodes, n), "id");
          if (!strncmp(node_id, "workflow-step:", 14)) {
            const int64_t plan_version = json_get_int(json_array_get(nodes, n), "plan_version", -1);
            const int64_t step_index = json_get_int(json_array_get(nodes, n), "step_index", -1);
            if (i == 0) { check_equal(plan_version, 0); check_true(step_index == 0 || step_index == 1); }
            else if (i == 1) { check_equal(plan_version, 0); check_equal(step_index, 0); }
            else { check_true(plan_version == 0 || plan_version == 1); check_equal(step_index, 0); }
            ++steps;
          }
          status = json_get_string(json_array_get(nodes, n), "step_origin_status");
          if (status && !strcmp(status, "matched")) ++prepared;
        }
        check_equal(matched, 2u);
        check_equal(steps, i == 0 || i == 2 ? 2u : (i == 1 ? 1u : 0u));
        check_equal(prepared, steps);
        check_equal(state.tool_calls, 2u);
      }
    }
  }
  it("regenerates the same scoped identities when replaying a saved tool checkpoint") {
    const char *interrupts[] = {"tools"};
    turbo_graph_run_options_t options = {0};
    turbo_agent_app_exec_options_t resume = {0};
    options.interrupt_before_nodes = interrupts; options.interrupt_before_count = 1;
    check_equal(turbo_agent_app_start_text(app, "execute both steps", &options, &summary, &result_state), 0);
    check_equal(json_get_string(summary, "status"), "interrupted");
    check_equal(state.transport_calls, 2u); check_equal(state.tool_calls, 0u);
    const json_value_t *saved_origin = json_object_get(json_array_get(
        json_object_get(result_state, "executor_state_versions"), 0), "plan_step_origin");
    check_equal(json_get_int(saved_origin, "schema_version", -1), 1);
    check_equal(json_get_int(saved_origin, "plan_version", -1), 0);
    check_equal(json_get_int(saved_origin, "step_index", -1), 0);
    check_not_null(turbo_agent_app_last_checkpoint_id(app));
    checkpoint = tstr_dup(turbo_agent_app_last_checkpoint_id(app)); check_not_null(checkpoint);
    resume.scope = TURBO_SESSION_SCOPE_CHECKPOINT; resume.input_kind = TURBO_SESSION_INPUT_OVERRIDE;
    resume.checkpoint_id = checkpoint;
    for (int replay = 0; replay < 2; ++replay) {
      state.transport_calls = 2;
      json_free(summary); summary = NULL;
      json_free(result_state); result_state = NULL;
      check_equal(turbo_agent_app_resume_preset(app, TURBO_AGENT_SESSION_WORKFLOW_ENGINEERING,
          NULL, NULL, &resume, NULL, NULL, &summary, &result_state), 0);
      check_equal(json_get_string(summary, "status"), "completed");
      check_equal(state.transport_calls, 5u); check_equal(state.tool_calls, 2u);
      const json_value_t *versions = json_object_get(result_state, "executor_state_versions");
      check_equal(json_array_size(versions), 2u);
      for (size_t n = 0; n < 2; ++n) {
        const json_value_t *origin = json_object_get(json_array_get(versions, n), "plan_step_origin");
        check_equal(json_get_int(origin, "plan_version", -1), 0);
        check_equal(json_get_int(origin, "step_index", -1), (int)n);
      }
    }
  }
  const char *bad_progress[] = {"[]", "[{}]", "[[true],[true]]"};
  for (size_t i = 0; i < sizeof(bad_progress) / sizeof(bad_progress[0]); ++i) {
    it("refuses ambiguous plan progress before creating an executor state case %zu", i) {
      turbo_graph_exec_ctx_t ctx = {0};
      const char plan[] = "{\"input\":[],\"events\":[],\"plan_versions\":[{\"steps\":[{\"text\":\"task\"}]}]}";
      result_state = json_parse(plan, sizeof(plan) - 1); check_not_null(result_state);
      summary = json_parse(bad_progress[i], strlen(bad_progress[i])); check_not_null(summary);
      check_equal(turbo_runtime_json_object_set(result_state, "plan_progress_versions", summary), 0); summary = NULL;
      ctx.state = result_state;
      check_not_equal(turbo_agent_plan_step_prepare_node(&ctx, NULL), 0);
      check_null(json_object_get(result_state, "executor_state_versions"));
      check_equal(state.transport_calls, 0u); check_equal(state.tool_calls, 0u);
    }
  }
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
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
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
    check_equal(state.tool_calls, 1);
    check_true(state.saw_tool_result);
    check_equal(state.transport_calls, 3);
    check_equal(invoke_result, 0);
    text = turbo_agent_app_result_text(result_state);
    check_equal(text, "step done");
    check_equal(json_get_string(summary, "status"), "completed");

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
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
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

    check_equal(turbo_agent_app_invoke_text(app, "recover after failure", NULL, &text, &summary),
                 0);
    check_equal(text, "recovered");
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(state.transport_calls, 5);
    check_equal(state.tool_calls, 1);
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
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
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

    check_equal(
        turbo_agent_app_invoke_text(app, "use a tool while executing", NULL, &text, &summary), 0);
    check_equal(text, "executor tool done");
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(state.transport_calls, 3);
    check_equal(state.tool_calls, 1);
    check_true(state.saw_tool_schema);
    check_true(state.saw_tool_result);

    free(text);
    turbo_runtime_json_destroy(summary);
    turbo_agent_app_destroy(app);
    turbo_tool_registry_destroy(registry);
  }
}
