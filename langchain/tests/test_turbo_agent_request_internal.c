#include "tinytest.h"
#include "turbo_agent.h"
#include "../src/turbo_agent_runtime_internal.h"
#include "turbo_model_provider.h"

#include <stdlib.h>
#include <string.h>

static int dummy_transport(const char *request_json, char **out_response_json, void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static json_value_t *parse_request_json_or_fail(const char *request_json) {
  json_value_t *parsed = NULL;

  check_not_null(request_json);
  check_int_eq(turbo_parse_json((const uint8_t *)request_json, strlen(request_json), &parsed), 0);
  check_not_null(parsed);
  return parsed;
}

spec("turbo agent request internals") {

  it("should preserve parallel tool call config when building requests") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.parallel_tool_calls = 1;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_not_null(request_json);
    check_true(strstr(request_json, "\"parallel_tool_calls\":true") != NULL);

    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should build responses requests with input messages") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.instructions = "Be terse.";
    config.provider = turbo_model_provider_openai_responses();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    check_not_null(turbo_json_object_get(request, "input"));
    check_ptr_eq(turbo_json_object_get(request, "messages"), NULL);
    check_str_eq(turbo_json_get_string(request, "instructions"), "Be terse.");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(turbo_json_object_get(request, "input"), 0),
                                       "role"),
                 "user");

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should build chat requests with messages instead of responses input") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *messages;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.instructions = "Be terse.";
    config.provider = turbo_model_provider_openai_chat_completions();
    config.parallel_tool_calls = 1;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    messages = turbo_json_object_get(request, "messages");
    check_not_null(messages);
    check_ptr_eq(turbo_json_object_get(request, "input"), NULL);
    check_true(turbo_json_get_bool(request, "parallel_tool_calls", false));
    check_size_eq(turbo_json_array_size(messages), 2);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 0), "role"), "system");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 1), "role"), "user");

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should build anthropic requests with top-level system text") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *messages;

    config.model = "claude-sonnet";
    config.transport_fn = dummy_transport;
    config.instructions = "Be terse.";
    config.provider = turbo_model_provider_anthropic_messages();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    messages = turbo_json_object_get(request, "messages");
    check_not_null(messages);
    check_ptr_eq(turbo_json_object_get(request, "input"), NULL);
    check_str_eq(turbo_json_get_string(request, "system"), "Be terse.");
    check_size_eq(turbo_json_array_size(messages), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 0), "role"), "user");

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should encode structured output in chat requests as response_format") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *response_format;
    const json_value_t *json_schema;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    config.structured_output_name = "answer";
    config.structured_output_schema_json =
        "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"]}";
    config.structured_output_strict = 1;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    response_format = turbo_json_object_get(request, "response_format");
    check_not_null(response_format);
    check_str_eq(turbo_json_get_string(response_format, "type"), "json_schema");
    json_schema = turbo_json_object_get(response_format, "json_schema");
    check_not_null(json_schema);
    check_str_eq(turbo_json_get_string(json_schema, "name"), "answer");
    check_true(turbo_json_get_bool(json_schema, "strict", false));

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should encode structured output in responses requests as text format") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *text;
    const json_value_t *format;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_responses();
    config.structured_output_name = "answer";
    config.structured_output_schema_json =
        "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"]}";
    config.structured_output_strict = 1;
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    text = turbo_json_object_get(request, "text");
    check_not_null(text);
    format = turbo_json_object_get(text, "format");
    check_not_null(format);
    check_str_eq(turbo_json_get_string(format, "type"), "json_schema");
    check_str_eq(turbo_json_get_string(format, "name"), "answer");
    check_true(turbo_json_get_bool(format, "strict", false));

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should keep anthropic system text ordered as instructions then memory context") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const char *system_text;
    const char *memory_marker = "Persistent memory:\n[project] /tmp/memory.md\nRemember prior constraints.";
    const char *memory_pos;
    const char *second_instructions;

    config.model = "claude-sonnet";
    config.transport_fn = dummy_transport;
    config.instructions = "Be terse.";
    config.provider = turbo_model_provider_anthropic_messages();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(
        turbo_agent_state_add_memory_context_layer(state, "project", "/tmp/memory.md",
                                                   "Remember prior constraints."),
        0);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);

    request = parse_request_json_or_fail(request_json);
    system_text = turbo_json_get_string(request, "system");
    check_not_null(system_text);
    check_true(strncmp(system_text, "Be terse.", strlen("Be terse.")) == 0);
    memory_pos = strstr(system_text, memory_marker);
    check_not_null(memory_pos);
    check_true(memory_pos > system_text);
    second_instructions = strstr(system_text + strlen("Be terse."), "Be terse.");
    check_null(second_instructions);

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fail request build when structured output schema json is invalid") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    config.structured_output_name = "answer";
    config.structured_output_schema_json = "{\"type\":\"object\"";
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_ne(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_ptr_eq(request_json, NULL);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fail responses request build when structured output schema json is invalid") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_responses();
    config.structured_output_name = "answer";
    config.structured_output_schema_json = "{\"type\":\"object\"";
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);
    check_int_ne(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_ptr_eq(request_json, NULL);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should reuse tool outputs when previous response id is available") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *model_event;
    json_value_t *tool_results_event;
    json_value_t *outputs;
    json_value_t *output_item;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *input;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_responses();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    model_event = turbo_json_create_object();
    check_not_null(model_event);
    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "response_id", "resp_123");
    turbo_json_array_add(events, model_event);

    tool_results_event = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(tool_results_event);
    check_not_null(outputs);
    check_not_null(output_item);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "type", "function_call_output");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "{\"ok\":true,\"stdout\":\"done\"}");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results_event, "outputs", outputs);
    turbo_json_array_add(events, tool_results_event);

    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    request = parse_request_json_or_fail(request_json);
    check_str_eq(turbo_json_get_string(request, "previous_response_id"), "resp_123");
    input = turbo_json_object_get(request, "input");
    check_not_null(input);
    check_size_eq(turbo_json_array_size(input), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(input, 0), "type"),
                 "function_call_output");
    check_ptr_eq(turbo_json_object_get(request, "messages"), NULL);

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fall back to canonical input when cached outputs are malformed") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *model_event;
    json_value_t *tool_results_event;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *input;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_responses();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    model_event = turbo_json_create_object();
    check_not_null(model_event);
    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "response_id", "resp_123");
    turbo_json_array_add(events, model_event);

    tool_results_event = turbo_json_create_object();
    check_not_null(tool_results_event);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(tool_results_event, "outputs", "bad");
    turbo_json_array_add(events, tool_results_event);

    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    request = parse_request_json_or_fail(request_json);
    check_ptr_eq(turbo_json_object_get(request, "previous_response_id"), NULL);
    input = turbo_json_object_get(request, "input");
    check_not_null(input);
    check_size_eq(turbo_json_array_size(input), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(input, 0), "role"), "user");

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fall back to canonical input when cached outputs have no response id") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *tool_results_event;
    json_value_t *outputs;
    json_value_t *output_item;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *input;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_responses();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    tool_results_event = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(tool_results_event);
    check_not_null(outputs);
    check_not_null(output_item);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "type", "function_call_output");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(output_item, "output", "{\"ok\":true,\"stdout\":\"done\"}");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results_event, "outputs", outputs);
    turbo_json_array_add(events, tool_results_event);

    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    request = parse_request_json_or_fail(request_json);
    check_ptr_eq(turbo_json_object_get(request, "previous_response_id"), NULL);
    input = turbo_json_object_get(request, "input");
    check_not_null(input);
    check_size_eq(turbo_json_array_size(input), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(input, 0), "role"), "user");

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fail chat request build when replayed tool result payload is malformed") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *tool_results_event;
    json_value_t *outputs;
    json_value_t *output_item;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    tool_results_event = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(tool_results_event);
    check_not_null(outputs);
    check_not_null(output_item);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results_event, "outputs", outputs);
    turbo_json_array_add(events, tool_results_event);

    check_int_ne(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_ptr_eq(request_json, NULL);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should replay tool result messages even when output items carry parent lineage fields") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *tool_results_event;
    json_value_t *outputs;
    json_value_t *output_item;
    char *request_json = NULL;
    json_value_t *request = NULL;
    const json_value_t *messages;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    tool_results_event = turbo_json_create_object();
    outputs = turbo_json_create_array();
    output_item = turbo_json_create_object();
    check_not_null(tool_results_event);
    check_not_null(outputs);
    check_not_null(output_item);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(output_item, "call_id", "call_1");
    turbo_json_object_set_string(
        output_item, "output",
        "{\"ok\":true,\"stdout\":\"done\",\"parent_agent_run_id\":\"run_parent\","
        "\"parent_tool_call_id\":\"call_parent\",\"parent_tool_name\":\"delegate\"}");
    turbo_json_object_set_string(output_item, "parent_agent_run_id", "run_parent");
    turbo_json_object_set_string(output_item, "parent_tool_call_id", "call_parent");
    turbo_json_object_set_string(output_item, "parent_tool_name", "delegate");
    turbo_json_array_add(outputs, output_item);
    turbo_json_object_add(tool_results_event, "outputs", outputs);
    turbo_json_array_add(events, tool_results_event);

    check_int_eq(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    request = parse_request_json_or_fail(request_json);
    messages = turbo_json_object_get(request, "messages");
    check_not_null(messages);
    check_size_eq(turbo_json_array_size(messages), 2);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(messages, 1), "role"), "tool");
    check_true(strstr(turbo_json_get_string(turbo_json_array_get(messages, 1), "content"),
                      "\"parent_agent_run_id\":\"run_parent\"") != NULL);

    turbo_free_json(&request);
    free(request_json);
    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fail chat request build when replayed tool results outputs are not an array") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *tool_results_event;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    tool_results_event = turbo_json_create_object();
    check_not_null(tool_results_event);
    turbo_json_object_set_string(tool_results_event, "kind", "tool_results");
    turbo_json_object_set_string(tool_results_event, "outputs", "bad");
    turbo_json_array_add(events, tool_results_event);

    check_int_ne(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_ptr_eq(request_json, NULL);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }

  it("should fail chat request build when replayed model tool call record is malformed") {
    turbo_agent_config_t config = {0};
    turbo_agent_t *agent;
    json_value_t *state;
    json_value_t *events;
    json_value_t *model_event;
    json_value_t *tool_calls;
    json_value_t *tool_call;
    char *request_json = NULL;

    config.model = "gpt-5.4";
    config.transport_fn = dummy_transport;
    config.provider = turbo_model_provider_openai_chat_completions();
    agent = turbo_agent_create(&config);
    check_not_null(agent);

    state = turbo_agent_state_create();
    check_not_null(state);
    check_int_eq(turbo_agent_state_add_user_message(state, "Ping"), 0);

    events = turbo_json_object_get(state, "events");
    check_not_null(events);

    model_event = turbo_json_create_object();
    tool_calls = turbo_json_create_array();
    tool_call = turbo_json_create_object();
    check_not_null(model_event);
    check_not_null(tool_calls);
    check_not_null(tool_call);
    turbo_json_object_set_string(model_event, "kind", "model");
    turbo_json_object_set_string(model_event, "output_text", "");
    turbo_json_object_set_string(tool_call, "call_id", "call_1");
    turbo_json_array_add(tool_calls, tool_call);
    turbo_json_object_add(model_event, "tool_calls", tool_calls);
    turbo_json_array_add(events, model_event);

    check_int_ne(turbo_agent_build_turn_request(agent, state, &request_json), 0);
    check_ptr_eq(request_json, NULL);

    turbo_free_json(&state);
    turbo_agent_destroy(agent);
  }
}
