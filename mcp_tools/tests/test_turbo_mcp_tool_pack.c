#include "tinytest.h"
#include "turbo_mcp_tool_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbo_runtime_json.h>

typedef enum test_mcp_mode_e {
  TEST_MCP_JSON = 0,
  TEST_MCP_SSE = 1,
  TEST_MCP_FAIL = 2
} test_mcp_mode_t;

typedef struct test_mcp_transport_s {
  test_mcp_mode_t mode;
  int page;
  int saw_protocol_version;
  int saw_method;
  int saw_name;
  int saw_parameter;
  int saw_nested_parameters;
  char response[8192];
} test_mcp_transport_t;

static int test_has_header(const char *const *headers, size_t header_count, const char *expected) {
  size_t index;
  for (index = 0; index < header_count; ++index)
    if (!strcmp(headers[index], expected)) return 1;
  return 0;
}

static int test_mcp_post(void *user_data, const char *endpoint, const char *const *headers,
                         size_t header_count, const uint8_t *body, size_t body_len,
                         turbo_mcp_transport_response_t *response) {
  test_mcp_transport_t *transport = (test_mcp_transport_t *)user_data;
  json_value_t *request = NULL;
  json_value_t *id;
  json_value_t *params;
  const char *method;
  const char *number_text;
  size_t number_length = 0;
  char id_text[32];
  int length;
  int is_list;
  int is_sse_response = 0;

  if (!transport || !endpoint || strcmp(endpoint, "https://mcp.example.test/api") || !body ||
      !body_len || !response || turbo_runtime_json_parse(body, body_len, &request) != 0 || !request) {
    turbo_runtime_json_reset(&request);
    return -1;
  }
  id = json_object_get(request, "id");
  params = json_object_get(request, "params");
  method = json_get_string(request, "method");
  number_text = json_number_text(id, &number_length);
  if (!id || !params || !method || !number_text || !number_length ||
      number_length >= sizeof(id_text) || !json_object_get(params, "_meta")) {
    turbo_runtime_json_reset(&request);
    return -1;
  }
  memcpy(id_text, number_text, number_length);
  id_text[number_length] = '\0';
  is_list = !strcmp(method, "tools/list");
  transport->saw_protocol_version |=
      test_has_header(headers, header_count, "MCP-Protocol-Version: " TURBO_MCP_PROTOCOL_VERSION);
  transport->saw_method |= !strcmp(method, "tools/list")
                               ? test_has_header(headers, header_count, "Mcp-Method: tools/list")
                               : test_has_header(headers, header_count, "Mcp-Method: tools/call");

  if (transport->mode == TEST_MCP_FAIL) {
    turbo_runtime_json_reset(&request);
    return -1;
  }
  if (is_list) {
    if (transport->mode == TEST_MCP_SSE && transport->page++ == 0) {
      is_sse_response = 1;
      length = snprintf(transport->response, sizeof(transport->response),
                        "event: message\r\ndata: {\"jsonrpc\":\"2.0\",\"id\":%s,"
                        "\"result\":{\"resultType\":\"complete\",\"tools\":[],"
                        "\"nextCursor\":\"page-2\"}}\r\n\r\n",
                        id_text);
      response->content_type = "text/event-stream; charset=utf-8";
    } else {
      length = snprintf(transport->response, sizeof(transport->response),
                        "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                        "\"resultType\":\"complete\",\"tools\":["
                        "{\"name\":\"echo.remote\",\"description\":\"Remote echo\","
                        "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                        "\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"},"
                        "\"routing\":{\"type\":\"object\",\"properties\":{"
                        "\"shard\":{\"type\":\"integer\",\"x-mcp-header\":\"Shard\"},"
                        "\"enabled\":{\"type\":\"boolean\",\"x-mcp-header\":\"Enabled\"}}},"
                        "\"value\":{\"type\":\"integer\"}}}},"
                        "{\"name\":\"bad_header\",\"inputSchema\":{\"type\":\"object\","
                        "\"properties\":{\"ratio\":{\"type\":\"number\","
                        "\"x-mcp-header\":\"Ratio\"}}}}]}}",
                        id_text);
      response->content_type = "application/json; charset=utf-8";
    }
  } else if (!strcmp(method, "tools/call")) {
    transport->saw_name |= test_has_header(headers, header_count, "Mcp-Name: echo.remote");
    transport->saw_parameter |=
        test_has_header(headers, header_count, "Mcp-Param-Region: =?base64?SGVsbG8sIOS4lueVjA==?=");
    transport->saw_nested_parameters |=
        test_has_header(headers, header_count, "Mcp-Param-Shard: 42") &&
        test_has_header(headers, header_count, "Mcp-Param-Enabled: true");
    length = snprintf(transport->response, sizeof(transport->response),
                      "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                      "\"resultType\":\"complete\",\"structuredContent\":{\"ok\":true},"
                      "\"content\":[{\"type\":\"text\",\"text\":\"done\"}],"
                      "\"isError\":false}}",
                      id_text);
    response->content_type = "application/json";
  } else {
    turbo_runtime_json_reset(&request);
    return -1;
  }
  turbo_runtime_json_reset(&request);
  if (length < 0 || (size_t)length >= sizeof(transport->response)) return -1;
  memset(response, 0, sizeof(*response));
  response->status_code = 200;
  response->content_type =
      is_sse_response ? "text/event-stream; charset=utf-8" : "application/json; charset=utf-8";
  response->body = (const uint8_t *)transport->response;
  response->body_len = (size_t)length;
  return 0;
}

static turbo_mcp_tool_pack_t *test_pack_create(test_mcp_transport_t *transport) {
  turbo_mcp_tool_pack_config_t config;
  turbo_mcp_tool_pack_config_init(&config);
  config.endpoint = "https://mcp.example.test/api";
  config.server_id = "demo";
  config.transport_post = test_mcp_post;
  config.transport_user_data = transport;
  return turbo_mcp_tool_pack_create(&config);
}

spec("MCP tool pack") {
  it("validates its bounded versioned configuration") {
    turbo_mcp_tool_pack_config_t config;
    test_mcp_transport_t transport = {0};
    turbo_mcp_tool_pack_config_init(&config);
    config.endpoint = "https://mcp.example.test/api";
    config.server_id = "bad.id";
    config.transport_post = test_mcp_post;
    config.transport_user_data = &transport;
    check_null(turbo_mcp_tool_pack_create(&config));
    config.server_id = "demo";
    config.execution_policy.mode = TURBO_TOOL_EXECUTION_PARALLEL_SAFE;
    check_null(turbo_mcp_tool_pack_create(&config));
    config.execution_policy.mode = TURBO_TOOL_EXECUTION_EXCLUSIVE;
    config.abi_version++;
    check_null(turbo_mcp_tool_pack_create(&config));
  }

  it("discovers tools, rejects invalid header schemas, and mirrors safe headers") {
    test_mcp_transport_t transport = {0};
    turbo_mcp_tool_pack_t *pack = test_pack_create(&transport);
    json_value_t *arguments = NULL;
    json_value_t *result = NULL;
    json_value_t *structured;
    const char *const *required_capabilities = NULL;
    size_t required_capability_count = 0;

    check_not_null(pack);
    check_int_eq(turbo_mcp_tool_pack_refresh(pack), TURBO_TOOL_OK);
    check_size_eq(turbo_mcp_tool_pack_tool_count(pack), 1);
    check_size_eq(turbo_mcp_tool_pack_rejected_tool_count(pack), 1);
    check_int_eq(turbo_tool_registry_get_required_capabilities(
                     turbo_mcp_tool_pack_registry(pack), "mcp_demo_echo_remote",
                     &required_capabilities, &required_capability_count),
                 TURBO_TOOL_OK);
    check_size_eq(required_capability_count, 2);
    check_str_eq(required_capabilities[0], "runtime_tools");
    check_str_eq(required_capabilities[1], "network");
    check_true(transport.saw_protocol_version);
    check_true(transport.saw_method);
    check_int_eq(turbo_runtime_json_parse((const uint8_t *)"{\"region\":\"Hello, 世界\",\"routing\":{"
                                                   "\"shard\":42,\"enabled\":true},\"value\":7}",
                                  strlen("{\"region\":\"Hello, 世界\",\"routing\":{"
                                         "\"shard\":42,\"enabled\":true},\"value\":7}"),
                                  &arguments),
                 0);
    check_int_eq(turbo_tool_registry_execute_json_value(turbo_mcp_tool_pack_registry(pack),
                                                        "mcp_demo_echo_remote", arguments, &result),
                 TURBO_TOOL_OK);
    structured = json_object_get(result, "structuredContent");
    check_not_null(structured);
    check_true(json_get_bool(structured, "ok", false));
    check_true(transport.saw_name);
    check_true(transport.saw_parameter);
    check_true(transport.saw_nested_parameters);

    turbo_runtime_json_reset(&result);
    turbo_runtime_json_reset(&arguments);
    turbo_mcp_tool_pack_destroy(pack);
  }

  it("accepts request-scoped SSE and follows bounded tools/list pagination") {
    test_mcp_transport_t transport = {.mode = TEST_MCP_SSE};
    turbo_mcp_tool_pack_t *pack = test_pack_create(&transport);
    check_not_null(pack);
    check_int_eq(turbo_mcp_tool_pack_refresh(pack), TURBO_TOOL_OK);
    check_size_eq(turbo_mcp_tool_pack_tool_count(pack), 1);
    check_int_eq(transport.page, 2);
    turbo_mcp_tool_pack_destroy(pack);
  }

  it("keeps the previous registry when refresh fails") {
    test_mcp_transport_t transport = {0};
    turbo_mcp_tool_pack_t *pack = test_pack_create(&transport);
    turbo_tool_registry_t *registry;
    check_not_null(pack);
    check_int_eq(turbo_mcp_tool_pack_refresh(pack), TURBO_TOOL_OK);
    registry = turbo_mcp_tool_pack_registry(pack);
    transport.mode = TEST_MCP_FAIL;
    check_int_eq(turbo_mcp_tool_pack_refresh(pack), TURBO_TOOL_ERROR);
    check_ptr_eq(turbo_mcp_tool_pack_registry(pack), registry);
    check_size_eq(turbo_mcp_tool_pack_tool_count(pack), 1);
    check_true(strlen(turbo_mcp_tool_pack_last_error(pack)) > 0);
    turbo_mcp_tool_pack_destroy(pack);
  }
}
