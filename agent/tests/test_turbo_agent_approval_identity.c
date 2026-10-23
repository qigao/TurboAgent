#include "tinytest.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_store.h"
#include "../src/turbo_agent_runtime_internal.h"
#include "../src/turbo_agent_workflow_graph_internal.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

static const char identity_args[] = "{\"value\":\"visible\"}";
static const char *identity_fields[] = {"kind", "call_id", "tool_name", "arguments"};
/* JSON-escaped field contents, without the enclosing quotes. */
static const char *identity_values[] = {
    "tool_approval", "same-call", "identity.effect", "{\\\"value\\\":\\\"visible\\\"}"};

static tstr identity_note(int field, int key) {
  return tstr_format("{{\"{}{}\":\"{}{}\",\"{}{}\":\"{}{}\","
      "\"{}{}\":\"{}{}\",\"{}{}\":\"{}{}\"}}",
      identity_fields[0], key && field == 0 ? "\\u0000suffix" : "",
      identity_values[0], !key && field == 0 ? "\\u0000suffix" : "",
      identity_fields[1], key && field == 1 ? "\\u0000suffix" : "",
      identity_values[1], !key && field == 1 ? "\\u0000suffix" : "",
      identity_fields[2], key && field == 2 ? "\\u0000suffix" : "",
      identity_values[2], !key && field == 2 ? "\\u0000suffix" : "",
      identity_fields[3], key && field == 3 ? "\\u0000suffix" : "",
      identity_values[3], !key && field == 3 ? "\\u0000suffix" : "");
}

typedef struct identity_probe_s {
  turbo_agent_runtime_store_t inner;
  int calls, approved_calls, writes, reads;
} identity_probe_t;

static int identity_put(void *user, const char *collection, const char *id, const char *text) {
  identity_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) ++probe->writes;
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int identity_get(void *user, const char *collection, const char *id, char **out) {
  identity_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) ++probe->reads;
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int identity_list(void *user, const char *collection, const char *key,
    const char *value, char **out) {
  identity_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

/* Count only: this fixture never starts a process or modifies a file. */
static int identity_action(const json_value_t *args, json_value_t **out, void *user) {
  identity_probe_t *probe = user;
  (void)args;
  ++probe->calls;
  if (turbo_agent_current_tool_approval_granted()) ++probe->approved_calls;
  *out = turbo_action_result_create(1, "approved identity fixture");
  return *out ? 0 : -1;
}

static int identity_transport(const char *request, char **response, void *user) {
  (void)request; (void)user; *response = NULL; return -1;
}

static int identity_dispatch(turbo_graph_exec_ctx_t *ctx, turbo_agent_t *agent, int mode) {
  int outcome = -1;
  ctx->stop = 0; ctx->next_node = NULL;
  if (mode == 1) return turbo_agent_tool_node_checked(ctx, agent, &outcome);
  if (mode == 2) return turbo_agent_planner_tool_node(ctx, agent);
  if (mode == 3) return turbo_agent_executor_tool_node(ctx, agent);
  return turbo_agent_tool_node(ctx, agent);
}

/* Model an imported JSON state: the outer note itself contains a NUL after
 * an otherwise valid approval object. Do not pass it through a C-string setter. */
static int identity_append_outer_suffix(json_value_t *state) {
  json_value_t *versions = json_object_get(state, "review_versions");
  json_value_t *review = json_array_get(versions, json_array_size(versions) - 1);
  char *serialized = json_serialize(json_object_get(review, "note"), NULL);
  json_value_t *value;
  tstr payload;
  int rc;
  if (!serialized) return SALTS_ENOMEM;
  serialized[strlen(serialized) - 1] = '\0';
  payload = tstr_format("{}\\u0000suffix\"", serialized);
  json_serialize_free(serialized);
  if (!payload) return SALTS_ENOMEM;
  value = json_parse(payload, strlen(payload));
  tstr_free(payload);
  if (!value) return SALTS_EPROTO;
  if (json_type(value) != JSON_STRING || json_string_len(value) == strlen(json_string(value))) {
    json_free(value); return SALTS_EPROTO;
  }
  rc = turbo_runtime_json_object_set(review, "note", value);
  if (rc != SALTS_OK) json_free(value);
  return rc;
}

spec("exact Tool approval identity") {
  static identity_probe_t probe;
  static json_value_t *root, *child, *parsed;
  static tstr note;
  static turbo_action_tool_registry_t *actions;
  static turbo_tool_registry_t *registry;
  static turbo_agent_t *agent;
  static turbo_agent_runtime_t *runtime;
  static turbo_graph_t *graph;
  static turbo_graph_exec_ctx_t ctx;
  static turbo_agent_execution_context_t saved;

  before_each() {
    turbo_agent_config_t config = {0};
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_execution_context_t context = {0};
    turbo_agent_runtime_store_t callbacks = {.put = identity_put, .get = identity_get,
        .list = identity_list, .user_data = &probe};
    turbo_action_tool_definition_t definition = {.name = "identity.effect",
        .description = "Count a precisely approved invocation",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"}},"
            "\"required\":[\"value\"],\"additionalProperties\":false}",
        .kind = TURBO_ACTION_DANGEROUS, .requires_approval = 1,
        .handler = identity_action, .user_data = &probe};
    probe = (identity_probe_t){0};
    turbo_agent_execution_context_get(&saved);
    root = turbo_agent_state_create(); check_not_null(root);
    probe.inner = turbo_agent_runtime_store_memory_create(); check_not_null(probe.inner.put);
    runtime = turbo_agent_runtime_create(&callbacks); check_not_null(runtime);
    actions = turbo_action_tool_registry_create(); check_not_null(actions);
    check_equal(turbo_action_tool_registry_add(actions, &definition), TURBO_ACTION_TOOL_OK);
    registry = turbo_action_tool_registry_build_tool_registry_bridge(actions); check_not_null(registry);
    config.model = "offline"; config.transport_fn = identity_transport; config.tool_registry = registry;
    agent = turbo_agent_create(&config); check_not_null(agent);
    policy.allow_custom_tools = 1; policy.allow_shell = 1; /* Counter-only DANGEROUS fixture. */
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_equal(turbo_agent_add_action_policy_guardrail(agent, actions, &policy), 0);
    graph = turbo_graph_create("approval-identity"); check_not_null(graph);
    check_equal(turbo_graph_add_node(graph, "tools", turbo_agent_tool_node, agent), TURBO_GRAPH_EXEC_OK);
    ctx = (turbo_graph_exec_ctx_t){.graph = graph, .current_node = "tools", .state = root};
    context.runtime = runtime; context.thread_id = "identity-thread"; context.run_id = "identity-run";
    turbo_agent_execution_context_set(&context);
  }
  after_each() {
    turbo_agent_execution_context_set(&saved);
    turbo_graph_destroy(graph); graph = NULL;
    turbo_agent_destroy(agent); agent = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    turbo_action_tool_registry_destroy(actions); actions = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    if (probe.inner.user_data_free) probe.inner.user_data_free(probe.inner.user_data);
    json_free(root); json_free(parsed); root = child = parsed = NULL;
    tstr_free(note); note = NULL;
    probe = (identity_probe_t){0};
  }

  for (int key = 0; key < 2; ++key) {
    for (int field = 0; field < 4; ++field) {
      it("rejects NUL-suffixed %s %s in the public matcher", identity_fields[field], key ? "key" : "value") {
        note = identity_note(field, key); check_not_null(note);
        parsed = json_parse(note, strlen(note)); check_not_null(parsed);
        if (key) check_true(json_object_key_len(parsed, (size_t)field) > strlen(json_object_key(parsed, (size_t)field)));
        else {
          const json_value_t *value = json_object_get(parsed, identity_fields[field]);
          check_not_null(value); check_true(json_string_len(value) > strlen(json_string(value)));
        }
        check_equal(turbo_agent_state_request_review(root, note), 0);
        check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
        check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", identity_args), 0);
      }
    }
  }
  it("rejects an imported outer review note with a NUL suffix") {
    note = identity_note(-1, 0); check_not_null(note);
    check_equal(turbo_agent_state_request_review(root, note), 0);
    check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
    check_equal(identity_append_outer_suffix(root), SALTS_OK);
    check_null(turbo_agent_state_review_note(root));
    check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", identity_args), 0);
  }
  const char *valid_args[] = {"{\"value\":\"\xe9\x9b\xaa\"}", "{\"value\":\"\\u0000\"}",
      "{\"value\":\"\\\\path\"}"};
  for (size_t i = 0; i < sizeof(valid_args) / sizeof(valid_args[0]); ++i) {
    it("matches full arguments text without rejecting valid Unicode/JSON escapes (%lu)", (unsigned long)i) {
      parsed = json_parse(valid_args[i], strlen(valid_args[i])); check_not_null(parsed);
      json_free(parsed);
      parsed = json_create_object(); check_not_null(parsed);
      check_equal(turbo_runtime_json_object_set_string(parsed, "kind", "tool_approval"), SALTS_OK);
      check_equal(turbo_runtime_json_object_set_string(parsed, "call_id", "same-call"), SALTS_OK);
      check_equal(turbo_runtime_json_object_set_string(parsed, "tool_name", "identity.effect"), SALTS_OK);
      check_equal(turbo_runtime_json_object_set_string(parsed, "arguments", valid_args[i]), SALTS_OK);
      char *serialized = json_serialize(parsed, NULL); check_not_null(serialized);
      note = tstr_dup(serialized); json_serialize_free(serialized); check_not_null(note);
      check_equal(turbo_agent_state_request_review(root, note), 0);
      check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
      check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", valid_args[i]), 1);
      check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", identity_args), 0);
    }
  }
  it("accepts reordered escaped fields and unrelated Host metadata") {
    note = tstr_dup("{\"metadata\":\"Host context\",\"arguments\":\"{\\\"value\\\":\\\"visible\\\"}\","
        "\"tool_name\":\"identity.effect\",\"call_id\":\"same-call\",\"\\u006bind\":\"tool_\\u0061pproval\"}");
    check_not_null(note);
    check_equal(turbo_agent_state_request_review(root, note), 0);
    check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
    check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", identity_args), 1);
  }
  for (int invalid = 0; invalid < 2; ++invalid) {
    it("rejects an %s review", invalid ? "invalid JSON" : "unapproved exact") {
      note = invalid ? tstr_dup("not JSON") : identity_note(-1, 0); check_not_null(note);
      check_equal(turbo_agent_state_request_review(root, note), 0);
      check_equal(turbo_agent_state_set_review_approved(root, invalid), 0);
      check_equal(turbo_agent_tool_approval_review_matches(root, "same-call", "identity.effect", identity_args), 0);
    }
  }
  const char *modes[] = {"ordinary", "checked", "planner", "executor"};
  for (int mode = 0; mode < 4; ++mode) {
    for (int field = -1; field < 5; ++field) {
      it("%s dispatch %s %s approval before Journal/callback entry", modes[mode],
          field < 0 ? "accepts exact" : "rejects NUL-suffixed",
          field < 0 ? "control" : field == 4 ? "outer note" : identity_fields[field]) {
        child = root;
        if (mode >= 2) {
          json_value_t *versions = json_create_array(); check_not_null(versions);
          check_equal(turbo_runtime_json_object_set(root,
              mode == 2 ? "planner_state_versions" : "executor_state_versions", versions), SALTS_OK);
          child = turbo_agent_state_create(); check_not_null(child);
          check_equal(turbo_runtime_json_array_append(versions, child), SALTS_OK);
        }
        static const char model[] = "{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"same-call\","
            "\"name\":\"identity_effect\",\"arguments\":\"{\\\"value\\\":\\\"visible\\\"}\"}]}";
        json_value_t *event = json_parse(model, sizeof(model) - 1); check_not_null(event);
        check_equal(turbo_runtime_json_array_append(json_object_get(child, "events"), event), SALTS_OK);
        note = identity_note(field, 0); check_not_null(note);
        check_equal(turbo_agent_state_request_review(root, note), 0);
        check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
        if (field == 4) check_equal(identity_append_outer_suffix(root), SALTS_OK);
        check_equal(identity_dispatch(&ctx, agent, mode), 0);
        if (field >= 0) {
          check_equal(probe.calls, 0); check_equal(probe.writes, 0); check_equal(probe.reads, 0);
          check_true(ctx.stop); check_true(turbo_agent_state_review_required(root));
          check_false(turbo_agent_state_review_approved(root));
          check_equal(turbo_agent_state_set_review_approved(root, 1), 0);
          check_equal(identity_dispatch(&ctx, agent, mode), 0);
        }
        check_false(ctx.stop); check_equal(probe.calls, 1); check_equal(probe.approved_calls, 1);
        check_equal(probe.writes, 3);
      }
    }
  }
}
