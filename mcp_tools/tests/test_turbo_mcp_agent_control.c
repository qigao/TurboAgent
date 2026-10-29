#include "tinytest.h"
#include "turbo_mcp_tool_pack.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_store.h"
#include "../../agent/src/turbo_agent_runtime_internal.h"
#include <salts/clock.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum mcp_control_fault_e {
  MCP_CONTROL_TRANSPORT_FAILURE = 1,
  MCP_CONTROL_HTTP_FAILURE,
  MCP_CONTROL_RESPONSE_LIMIT,
  MCP_CONTROL_INVALID_JSON,
  MCP_CONTROL_WRONG_ID,
  MCP_CONTROL_CONTENT_TYPE,
  MCP_CONTROL_EMPTY_SSE,
  MCP_CONTROL_RPC_ERROR,
  MCP_CONTROL_MISSING_RESULT,
  MCP_CONTROL_VERSION_SUFFIX,
  MCP_CONTROL_FAULT_COUNT
} mcp_control_fault_t;

static const char *const mcp_control_fault_names[] = {
    "none", "transport failure", "HTTP failure", "response limit", "invalid JSON",
    "wrong response id", "unsupported content type", "unmatched SSE", "JSON-RPC error",
    "missing result", "NUL-tailed protocol version"};

typedef enum mcp_control_catalog_fault_e {
  MCP_CONTROL_NUL_RESULT_TYPE = 1,
  MCP_CONTROL_NUL_TOOL_NAME,
  MCP_CONTROL_NUL_HEADER_NAME,
  MCP_CONTROL_NUL_HEADER_TYPE,
  MCP_CONTROL_NUL_PROPERTY_KEY,
  MCP_CONTROL_NUL_CURSOR,
  MCP_CONTROL_NUL_DESCRIPTION,
  MCP_CONTROL_CATALOG_FAULT_COUNT
} mcp_control_catalog_fault_t;

static const char *const mcp_control_catalog_fault_names[] = {
    "none", "resultType", "tool name", "header name", "header type",
    "property key", "next cursor", "description"};

typedef struct mcp_control_probe_s {
  turbo_agent_runtime_store_t inner;
  turbo_cancel_source_t *source;
  atomic_uint_fast64_t now;
  uint64_t deadline;
  int remote_calls, writes, reads, stop_at_started, stop_in_post, fail_commit;
  int fault, discovery_fault, catalog_fault, releases, list_calls;
  char response[2048];
} mcp_control_probe_t;

static void mcp_control_release(void *user) {
  ++((mcp_control_probe_t *)user)->releases;
}

static uint64_t mcp_control_clock(void *user) {
  return atomic_load_explicit(&((mcp_control_probe_t *)user)->now, memory_order_relaxed);
}

static void mcp_control_stop(mcp_control_probe_t *probe, int mode) {
  if (mode == 2) atomic_store_explicit(&probe->now, probe->deadline, memory_order_relaxed);
  else if (mode && turbo_cancel_source_reason(probe->source) == TURBO_CANCEL_NONE)
    check_equal(turbo_cancel_source_cancel(probe->source, TURBO_CANCEL_USER), SALTS_OK);
}

static int mcp_control_put(void *user, const char *collection, const char *id, const char *text) {
  mcp_control_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) {
    ++probe->writes;
    if (strstr(text, "\"started\"")) mcp_control_stop(probe, probe->stop_at_started);
    if (probe->fail_commit && strstr(text, "\"committed\"")) return SALTS_EIO;
  }
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int mcp_control_create(void *user, const char *collection, const char *id,
                              const char *text) {
  mcp_control_probe_t *probe = user;
  if (!probe->inner.create_if_absent) return SALTS_ENOTSUP;
  if (!strcmp(collection, "agent_tool_journal")) ++probe->writes;
  return probe->inner.create_if_absent(probe->inner.user_data, collection, id, text);
}

static int mcp_control_get(void *user, const char *collection, const char *id, char **out) {
  mcp_control_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) ++probe->reads;
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int mcp_control_list(void *user, const char *collection, const char *key,
    const char *value, char **out) {
  mcp_control_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

/* Offline MCP server: no network/process/filesystem effect is performed. */
static int mcp_control_post(void *user, const char *endpoint, const char *const *headers,
    size_t header_count, const uint8_t *body, size_t length,
    turbo_mcp_transport_response_t *response) {
  mcp_control_probe_t *probe = user;
  json_value_t *request = json_parse((const char *)body, length);
  const char *method = request ? json_get_string(request, "method") : NULL;
  int id = request ? json_get_int(request, "id", -1) : -1;
  int written, is_call = method && !strcmp(method, "tools/call");
  int fault = is_call || probe->discovery_fault ? probe->fault : 0;
  (void)endpoint; (void)headers; (void)header_count;
  if (!method || id < 0) { json_free(request); return -1; }
  if (fault == MCP_CONTROL_TRANSPORT_FAILURE) {
    if (is_call) { ++probe->remote_calls; mcp_control_stop(probe, probe->stop_in_post); }
    json_free(request); return -1;
  }
  if (!strcmp(method, "tools/list")) {
    ++probe->list_calls;
    written = snprintf(probe->response, sizeof(probe->response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
        "\"name\":\"effect.probe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"}}}}]}}", id);
    if (probe->catalog_fault == MCP_CONTROL_NUL_RESULT_TYPE)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\\u0000suffix\",\"tools\":[]}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_TOOL_NAME)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
          "\"name\":\"effect.probe\\u0000suffix\",\"inputSchema\":{\"type\":\"object\"}}]}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_HEADER_NAME)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
          "\"name\":\"effect.probe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
          "\"region\":{\"type\":\"string\",\"x-mcp-header\":\"Region\\u0000suffix\"}}}}]}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_HEADER_TYPE)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
          "\"name\":\"effect.probe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
          "\"region\":{\"type\":\"string\\u0000suffix\",\"x-mcp-header\":\"Region\"}}}}]}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_PROPERTY_KEY)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
          "\"name\":\"effect.probe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{"
          "\"region\\u0000suffix\":{\"type\":\"string\",\"x-mcp-header\":\"Region\"}}}}]}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_CURSOR)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[],"
          "\"nextCursor\":\"page\\u0000suffix\"}}", id);
    else if (probe->catalog_fault == MCP_CONTROL_NUL_DESCRIPTION)
      written = snprintf(probe->response, sizeof(probe->response),
          "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\",\"tools\":[{"
          "\"name\":\"effect.probe\",\"description\":\"visible\\u0000hidden\","
          "\"inputSchema\":{\"type\":\"object\"}}]}}", id);
  } else if (!strcmp(method, "tools/call")) {
    ++probe->remote_calls;
    mcp_control_stop(probe, probe->stop_in_post);
    written = snprintf(probe->response, sizeof(probe->response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{\"resultType\":\"complete\","
        "\"structuredContent\":{\"private\":\"mcp-success\"}}}", id);
  } else { json_free(request); return -1; }
  if (fault == MCP_CONTROL_RPC_ERROR)
    written = snprintf(probe->response, sizeof(probe->response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32000,\"message\":\"remote failure\"}}", id);
  else if (fault == MCP_CONTROL_MISSING_RESULT)
    written = snprintf(probe->response, sizeof(probe->response), "{\"jsonrpc\":\"2.0\",\"id\":%d}", id);
  else if (fault == MCP_CONTROL_WRONG_ID)
    written = snprintf(probe->response, sizeof(probe->response),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":{}}", id + 1000);
  else if (fault == MCP_CONTROL_VERSION_SUFFIX)
    written = snprintf(probe->response, sizeof(probe->response),
        "{\"jsonrpc\":\"2.0\\u0000suffix\",\"id\":%d,\"result\":{\"resultType\":\"complete\","
        "\"tools\":[{\"name\":\"effect.probe\",\"inputSchema\":{\"type\":\"object\"}}],"
        "\"structuredContent\":{\"private\":\"mcp-success\"}}}", id);
  json_free(request);
  if (written < 0 || (size_t)written >= sizeof(probe->response)) return -1;
  *response = (turbo_mcp_transport_response_t){.status_code = 200,
      .content_type = "application/json", .body = (const uint8_t *)probe->response,
      .body_len = (size_t)written};
  if (is_call || probe->discovery_fault) {
    response->release = mcp_control_release; response->release_context = probe;
    if (fault == MCP_CONTROL_HTTP_FAILURE) response->status_code = 503;
    else if (fault == MCP_CONTROL_RESPONSE_LIMIT) response->body_len = sizeof(probe->response) + 1;
    else if (fault == MCP_CONTROL_INVALID_JSON) response->body_len = 1;
    else if (fault == MCP_CONTROL_CONTENT_TYPE) response->content_type = "text/plain";
    else if (fault == MCP_CONTROL_EMPTY_SSE) {
      response->content_type = "text/event-stream";
      response->body = (const uint8_t *)"event: ping\n\n"; response->body_len = strlen((const char *)response->body);
    }
  }
  return 0;
}

static int mcp_control_transport(const char *request, char **out, void *user) {
  (void)request; (void)user; *out = NULL; return -1;
}

static int mcp_control_model(json_value_t *state) {
  static const char text[] = "{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"mcp-call\","
      "\"name\":\"mcp_control_effect_probe\",\"arguments\":\"{}\"}]}";
  json_value_t *event = json_parse(text, sizeof(text) - 1);
  if (!event) return -1;
  if (turbo_runtime_json_array_append(json_object_get(state, "events"), event)) {
    json_free(event); return -1;
  }
  return 0;
}

static const char *mcp_control_delivered(const json_value_t *state) {
  const json_value_t *events = json_object_get(state, "events");
  const json_value_t *event = json_array_get(events, json_array_size(events) - 1);
  return json_get_string(json_array_get(json_object_get(event, "outputs"), 0), "output");
}

spec("MCP Agent and context admission") {
  static mcp_control_probe_t probe;
  static turbo_cancel_token_t *token;
  static turbo_mcp_tool_pack_t *pack;
  static turbo_agent_t *agent;
  static turbo_agent_runtime_t *runtime;
  static json_value_t *state, *pending, *args, *result, *records;
  static char *output;
  static char *journal_before, *journal_after;
  static turbo_graph_exec_ctx_t ctx;
  static turbo_agent_execution_context_t saved;
  static turbo_tool_execution_context_t control;

  before_each() {
    turbo_agent_config_t config = {0};
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_mcp_tool_pack_config_t mcp;
    turbo_agent_execution_context_t context = {0};
    turbo_agent_runtime_store_t store = {.put = mcp_control_put, .get = mcp_control_get,
        .list = mcp_control_list, .user_data = &probe,
        .create_if_absent = mcp_control_create};
    turbo_cancel_source_config_t cancellation = {.struct_size = sizeof(cancellation),
        .abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION, .monotonic_ms = mcp_control_clock,
        .clock_user_data = &probe};
    probe = (mcp_control_probe_t){0};
    atomic_init(&probe.now, cmeta_monotonic_ms());
    probe.deadline = atomic_load(&probe.now) + 60000;
    cancellation.deadline_mono_ms = probe.deadline;
    check_equal(turbo_cancel_source_create(&cancellation, &probe.source), SALTS_OK);
    check_equal(turbo_cancel_source_token(probe.source, &token), SALTS_OK);
    turbo_agent_execution_context_get(&saved);
    turbo_mcp_tool_pack_config_init(&mcp);
    mcp.endpoint = "https://mcp.example.test/offline"; mcp.server_id = "control";
    mcp.max_request_bytes = 1024; mcp.max_response_bytes = sizeof(probe.response);
    mcp.transport_post = mcp_control_post; mcp.transport_user_data = &probe;
    pack = turbo_mcp_tool_pack_create(&mcp); check_not_null(pack);
    check_equal(turbo_mcp_tool_pack_refresh(pack), TURBO_TOOL_OK);
    config.model = "offline"; config.transport_fn = mcp_control_transport;
    config.tool_registry = turbo_mcp_tool_pack_registry(pack);
    agent = turbo_agent_create(&config); check_not_null(agent);
    policy.allow_runtime_tools = 1; policy.allow_network = 1;
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    probe.inner = turbo_agent_runtime_store_memory_create(); check_not_null(probe.inner.put);
    runtime = turbo_agent_runtime_create(&store); check_not_null(runtime);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(mcp_control_model(state), 0);
    pending = json_clone(state); check_not_null(pending);
    args = json_create_object(); check_not_null(args);
    ctx = (turbo_graph_exec_ctx_t){.state = state};
    context.runtime = runtime; context.thread_id = "mcp-thread"; context.run_id = "mcp-run";
    context.cancel_token = token; turbo_agent_execution_context_set(&context);
    control = (turbo_tool_execution_context_t){.struct_size = sizeof(control),
        .abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION, .cancel_token = token,
        .deadline_mono_ms = probe.deadline};
  }
  after_each() {
    turbo_agent_execution_context_set(&saved);
    turbo_agent_destroy(agent); agent = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    if (probe.inner.user_data_free) probe.inner.user_data_free(probe.inner.user_data);
    turbo_mcp_tool_pack_destroy(pack); pack = NULL;
    turbo_cancel_token_release(token); token = NULL;
    turbo_cancel_source_destroy(probe.source);
    json_free(state); json_free(pending); json_free(args); json_free(result); json_free(records);
    state = pending = args = result = records = NULL;
    free(output); output = NULL;
    json_serialize_free(journal_before); json_serialize_free(journal_after);
    journal_before = journal_after = NULL;
    probe = (mcp_control_probe_t){0};
  }

  it("dispatches a real MCP binding through ToolNode text execution and replays without another request") {
    for (int replay = 0; replay < 2; ++replay) {
      ctx.state = replay ? pending : state;
      check_equal(turbo_agent_tool_node(&ctx, agent), 0);
      check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
      check_not_null(mcp_control_delivered(ctx.state));
      check_not_null(strstr(mcp_control_delivered(ctx.state), "mcp-success"));
    }
  }
  for (int fault = MCP_CONTROL_TRANSPORT_FAILURE; fault < MCP_CONTROL_FAULT_COUNT; ++fault) {
    it("retains discovery status and registry after %s", mcp_control_fault_names[fault]) {
      turbo_tool_registry_t *registry = turbo_mcp_tool_pack_registry(pack);
      /* Release the Agent's borrowed view before testing a refresh rejection. */
      turbo_agent_destroy(agent); agent = NULL;
      probe.fault = fault; probe.discovery_fault = 1;
      check_equal(turbo_mcp_tool_pack_refresh(pack), fault == MCP_CONTROL_RESPONSE_LIMIT ? TURBO_TOOL_OUTPUT_LIMIT : TURBO_TOOL_ERROR);
      check_true(turbo_mcp_tool_pack_registry(pack) == registry);
      check_equal(turbo_mcp_tool_pack_tool_count(pack), 1); check_equal(probe.remote_calls, 0);
      check_equal(probe.releases, fault == MCP_CONTROL_TRANSPORT_FAILURE ? 0 : 1);
      check_true(strlen(turbo_mcp_tool_pack_last_error(pack)) > 0);
    }
    for (int text = 0; text < 2; ++text) {
      it("preserves uncertainty after %s on the %s context entry", mcp_control_fault_names[fault], text ? "text" : "JSON") {
        probe.fault = fault;
        turbo_tool_status_t status = text
            ? turbo_tool_registry_execute_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", "{}", &control, &output)
            : turbo_tool_registry_execute_json_value_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", args, &control, &result);
        check_equal(status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
        check_equal(probe.remote_calls, 1);
        check_equal(probe.releases, fault == MCP_CONTROL_TRANSPORT_FAILURE ? 0 : 1);
        check_null(output); check_null(result);
        check_true(strlen(turbo_mcp_tool_pack_last_error(pack)) > 0);
      }
    }
    it("commits and replays uncertain %s without another request", mcp_control_fault_names[fault]) {
      probe.fault = fault;
      for (int replay = 0; replay < 2; ++replay) {
        ctx.state = replay ? pending : state;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
        check_equal(probe.releases, fault == MCP_CONTROL_TRANSPORT_FAILURE ? 0 : 1);
        check_not_null(mcp_control_delivered(ctx.state));
        check_not_null(strstr(mcp_control_delivered(ctx.state), "tool_side_effect_unknown"));
        check_null(strstr(mcp_control_delivered(ctx.state), "mcp-success"));
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
        check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
        check_equal(json_get_int(json_array_get(records, 0), "status", -99), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
        if (!replay) journal_before = json_serialize(records, NULL);
        else journal_after = json_serialize(records, NULL);
        json_free(records); records = NULL;
      }
      check_not_null(journal_before); check_not_null(journal_after);
      check_equal(journal_after, journal_before);
    }
  }
  for (int fault = MCP_CONTROL_NUL_RESULT_TYPE; fault < MCP_CONTROL_CATALOG_FAULT_COUNT; ++fault) {
    it("handles a NUL-tailed MCP catalog %s without a truncated identity", mcp_control_catalog_fault_names[fault]) {
      turbo_tool_registry_t *registry = turbo_mcp_tool_pack_registry(pack);
      turbo_tool_definition_t definition = {0};
      turbo_agent_destroy(agent); agent = NULL;
      probe.catalog_fault = fault;
      turbo_tool_status_t status = turbo_mcp_tool_pack_refresh(pack);
      check_equal(probe.list_calls, 2);
      if (fault == MCP_CONTROL_NUL_HEADER_NAME || fault == MCP_CONTROL_NUL_HEADER_TYPE ||
          fault == MCP_CONTROL_NUL_PROPERTY_KEY) {
        check_equal(status, TURBO_TOOL_OK);
        check_equal(turbo_mcp_tool_pack_tool_count(pack), 0);
        check_equal(turbo_mcp_tool_pack_rejected_tool_count(pack), 1);
      } else if (fault == MCP_CONTROL_NUL_DESCRIPTION) {
        check_equal(status, TURBO_TOOL_OK);
        check_equal(turbo_mcp_tool_pack_tool_count(pack), 1);
        check_equal(turbo_tool_registry_get_definition(turbo_mcp_tool_pack_registry(pack), 0, &definition), TURBO_TOOL_OK);
        check_equal(definition.description, "Tool discovered from an MCP server");
      } else {
        check_equal(status, TURBO_TOOL_ERROR);
        check_true(turbo_mcp_tool_pack_registry(pack) == registry);
        check_equal(turbo_mcp_tool_pack_tool_count(pack), 1);
        check_true(strlen(turbo_mcp_tool_pack_last_error(pack)) > 0);
      }
    }
  }
  for (int text = 0; text < 2; ++text) {
    it("retains the legacy %s failure convention after transport entry", text ? "text" : "JSON") {
      probe.fault = MCP_CONTROL_TRANSPORT_FAILURE;
      turbo_tool_status_t status = text
          ? turbo_tool_registry_execute(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", "{}", &output)
          : turbo_tool_registry_execute_json_value(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", args, &result);
      check_equal(status, TURBO_TOOL_ERROR); check_equal(probe.remote_calls, 1);
      check_null(output); check_null(result);
    }
    it("retains the known pre-transport request limit on the %s context entry", text ? "text" : "JSON") {
      char value[1501]; memset(value, 'x', sizeof(value) - 1); value[sizeof(value) - 1] = '\0';
      check_equal(turbo_runtime_json_object_set_string(args, "payload", value), SALTS_OK);
      char *arguments_json = json_serialize(args, NULL); check_not_null(arguments_json);
      turbo_tool_status_t status = text
          ? turbo_tool_registry_execute_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", arguments_json, &control, &output)
          : turbo_tool_registry_execute_json_value_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", args, &control, &result);
      json_serialize_free(arguments_json);
      check_equal(status, TURBO_TOOL_OUTPUT_LIMIT); check_equal(probe.remote_calls, 0);
      check_null(output); check_null(result); check_equal(probe.releases, 0);
    }
    it("does not replace uncertainty with a late %s", text ? "deadline" : "cancellation") {
      probe.fault = MCP_CONTROL_TRANSPORT_FAILURE; probe.stop_in_post = text ? 2 : 1;
      for (int replay = 0; replay < 2; ++replay) {
        ctx.state = replay ? pending : state;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
        check_not_null(mcp_control_delivered(ctx.state));
        check_not_null(strstr(mcp_control_delivered(ctx.state), "tool_side_effect_unknown"));
      }
    }
  }
  for (int limited = 0; limited < 2; ++limited) {
    it("retains started UNKNOWN after uncertain %s cannot commit", limited ? "oversized response" : "transport") {
      probe.fault = limited ? MCP_CONTROL_RESPONSE_LIMIT : MCP_CONTROL_TRANSPORT_FAILURE;
      probe.fail_commit = 1;
      for (int replay = 0; replay < 2; ++replay) {
        ctx.state = replay ? pending : state;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
        check_not_null(strstr(mcp_control_delivered(ctx.state), "tool_side_effect_unknown"));
      }
      check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
      check_equal(json_get_string(json_array_get(records, 0), "phase"), "started");
    }
    for (int network = 0; network < 2; ++network) {
      it("denies %s replay after %s without touching the uncertain record", network ? "network" : "runtime_tools", limited ? "oversized response" : "transport failure") {
        probe.fault = limited ? MCP_CONTROL_RESPONSE_LIMIT : MCP_CONTROL_TRANSPORT_FAILURE;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
        journal_before = json_serialize(records, NULL); check_not_null(journal_before);
        json_free(records); records = NULL;
        turbo_agent_policy_t policy = turbo_agent_policy_default();
        policy.allow_runtime_tools = network; policy.allow_network = !network;
        check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
        int reads_before = probe.reads;
        ctx.state = pending;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3); check_equal(probe.reads, reads_before);
        check_null(strstr(mcp_control_delivered(pending), "mcp-success"));
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
        journal_after = json_serialize(records, NULL); check_not_null(journal_after);
        check_equal(journal_after, journal_before);
      }
    }
  }
  it("commits and replays a known request limit without remote entry") {
    char value[1501]; memset(value, 'x', sizeof(value) - 1); value[sizeof(value) - 1] = '\0';
    check_equal(turbo_runtime_json_object_set_string(args, "payload", value), SALTS_OK);
    char *arguments_json = json_serialize(args, NULL); check_not_null(arguments_json);
    json_value_t *call = json_array_get(json_object_get(json_array_get(json_object_get(state, "events"), 0), "tool_calls"), 0);
    int rc = turbo_runtime_json_object_set_string(call, "arguments", arguments_json);
    json_serialize_free(arguments_json); check_equal(rc, SALTS_OK);
    json_free(pending); pending = json_clone(state); check_not_null(pending);
    for (int replay = 0; replay < 2; ++replay) {
      ctx.state = replay ? pending : state;
      check_equal(turbo_agent_tool_node(&ctx, agent), 0);
      check_equal(probe.remote_calls, 0); check_equal(probe.writes, 3);
      check_null(strstr(mcp_control_delivered(ctx.state), "tool_side_effect_unknown"));
    }
    check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
    check_equal(json_get_int(json_array_get(records, 0), "status", -99), TURBO_TOOL_OUTPUT_LIMIT);
  }
  for (int contextual = 0; contextual < 2; ++contextual) {
    for (int text = 0; text < 2; ++text) {
      it("preserves active %s %s execution", contextual ? "contextual" : "legacy", text ? "text" : "JSON") {
        turbo_tool_registry_t *registry = turbo_mcp_tool_pack_registry(pack);
        turbo_tool_status_t status = text
            ? contextual ? turbo_tool_registry_execute_with_context(registry, "mcp_control_effect_probe", "{}", &control, &output)
                         : turbo_tool_registry_execute(registry, "mcp_control_effect_probe", "{}", &output)
            : contextual ? turbo_tool_registry_execute_json_value_with_context(registry, "mcp_control_effect_probe", args, &control, &result)
                         : turbo_tool_registry_execute_json_value(registry, "mcp_control_effect_probe", args, &result);
        check_equal(status, TURBO_TOOL_OK); check_equal(probe.remote_calls, 1);
        if (text) { check_not_null(output); check_not_null(strstr(output, "mcp-success")); }
        else check_not_null(json_object_get(result, "structuredContent"));
      }
    }
  }
  for (int text = 0; text < 2; ++text) {
    it("checks an expired token-free deadline on the %s entry", text ? "text" : "JSON") {
      control.cancel_token = NULL; control.deadline_mono_ms = 1;
      turbo_tool_status_t status = text
          ? turbo_tool_registry_execute_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", "{}", &control, &output)
          : turbo_tool_registry_execute_json_value_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", args, &control, &result);
      check_equal(status, TURBO_TOOL_DEADLINE_EXCEEDED);
      check_equal(probe.remote_calls, 0); check_null(output); check_null(result);
    }
    it("preserves typed invalid-header status on the %s context entry", text ? "text" : "JSON") {
      check_equal(turbo_runtime_json_object_set_int64(args, "region", 1), SALTS_OK);
      turbo_tool_status_t status = text
          ? turbo_tool_registry_execute_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", "{\"region\":1}", &control, &output)
          : turbo_tool_registry_execute_json_value_with_context(turbo_mcp_tool_pack_registry(pack), "mcp_control_effect_probe", args, &control, &result);
      check_equal(status, TURBO_TOOL_INVALID_ARGUMENT);
      check_equal(probe.remote_calls, 0); check_null(output); check_null(result);
    }
  }
  it("retains the existing legacy JSON error convention") {
    check_equal(turbo_runtime_json_object_set_int64(args, "region", 1), SALTS_OK);
    check_equal(turbo_tool_registry_execute_json_value(turbo_mcp_tool_pack_registry(pack),
        "mcp_control_effect_probe", args, &result), TURBO_TOOL_ERROR);
    check_equal(probe.remote_calls, 0); check_null(result);
  }
  for (int network = 0; network < 2; ++network) {
    it("denies missing %s authority before a fresh request or Journal write", network ? "network" : "runtime_tools") {
      turbo_agent_policy_t policy = turbo_agent_policy_default();
      policy.allow_runtime_tools = network; policy.allow_network = !network;
      check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
      check_equal(turbo_agent_tool_node(&ctx, agent), 0);
      check_equal(probe.remote_calls, 0); check_equal(probe.writes, 0); check_equal(probe.reads, 0);
      check_not_null(mcp_control_delivered(state)); check_null(strstr(mcp_control_delivered(state), "mcp-success"));
    }
    for (int unknown = 0; unknown < 2; ++unknown) {
      it("denies %s replay without %s authority and retains the record", unknown ? "UNKNOWN" : "committed",
          network ? "network" : "runtime_tools") {
        probe.fail_commit = unknown;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
        journal_before = json_serialize(records, NULL); check_not_null(journal_before);
        json_free(records); records = NULL;
        turbo_agent_policy_t policy = turbo_agent_policy_default();
        policy.allow_runtime_tools = network; policy.allow_network = !network;
        check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
        int reads_before = probe.reads;
        ctx.state = pending;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3); check_equal(probe.reads, reads_before);
        check_null(strstr(mcp_control_delivered(pending), "mcp-success"));
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
        journal_after = json_serialize(records, NULL); check_not_null(journal_after);
        check_equal(journal_after, journal_before);
      }
    }
  }
  it("retains UNKNOWN after failed commit without repeating the MCP request") {
    probe.fail_commit = 1;
    for (int replay = 0; replay < 2; ++replay) {
      ctx.state = replay ? pending : state;
      check_equal(turbo_agent_tool_node(&ctx, agent), 0);
      check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
      check_not_null(strstr(mcp_control_delivered(ctx.state), "tool_side_effect_unknown"));
      check_null(strstr(mcp_control_delivered(ctx.state), "mcp-success"));
    }
    check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
    check_equal(json_get_string(json_array_get(records, 0), "phase"), "started");
  }
  for (int deadline = 0; deadline < 2; ++deadline) {
    for (int text = 0; text < 2; ++text) {
      it("checks %s before %s transport entry", deadline ? "deadline" : "cancellation", text ? "text" : "JSON") {
        mcp_control_stop(&probe, deadline ? 2 : 1);
        turbo_tool_status_t status = text
            ? turbo_tool_registry_execute_with_context(turbo_mcp_tool_pack_registry(pack),
                "mcp_control_effect_probe", "{}", &control, &output)
            : turbo_tool_registry_execute_json_value_with_context(turbo_mcp_tool_pack_registry(pack),
                "mcp_control_effect_probe", args, &control, &result);
        check_equal(status, deadline ? TURBO_TOOL_DEADLINE_EXCEEDED : TURBO_TOOL_CANCELLED);
        check_equal(probe.remote_calls, 0); check_null(output); check_null(result);
      }
    }
    it("checks %s after started persistence and retains the stopped Journal result on replay", deadline ? "deadline" : "cancellation") {
      probe.stop_at_started = deadline ? 2 : 1;
      for (int replay = 0; replay < 2; ++replay) {
        ctx.state = replay ? pending : state;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 0); check_equal(probe.writes, 3);
        check_not_null(mcp_control_delivered(ctx.state));
        check_not_null(strstr(mcp_control_delivered(ctx.state), deadline ? "tool_deadline_exceeded" : "tool_cancelled"));
      }
    }
    it("withholds %s late success while preserving the committed callback fact", deadline ? "expired" : "cancelled") {
      probe.stop_in_post = deadline ? 2 : 1;
      for (int replay = 0; replay < 2; ++replay) {
        ctx.state = replay ? pending : state;
        check_equal(turbo_agent_tool_node(&ctx, agent), 0);
        check_equal(probe.remote_calls, 1); check_equal(probe.writes, 3);
        check_not_null(mcp_control_delivered(ctx.state));
        check_not_null(strstr(mcp_control_delivered(ctx.state), deadline ? "tool_deadline_exceeded" : "tool_cancelled"));
        check_null(strstr(mcp_control_delivered(ctx.state), "mcp-success"));
      }
      check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "call_id", "mcp-call", &records), SALTS_OK);
      check_equal(json_get_int(json_array_get(records, 0), "status", -99), TURBO_TOOL_OK);
      check_equal(json_get_string(json_array_get(records, 0), "phase"), "committed");
      check_not_null(strstr(json_get_string(json_array_get(records, 0), "output"), "mcp-success"));
    }
  }
}
