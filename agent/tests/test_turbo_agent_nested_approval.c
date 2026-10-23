#include "tinytest.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_store.h"
#include "../src/turbo_agent_runtime_internal.h"
#include "../src/turbo_agent_workflow_graph_internal.h"
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

typedef struct nested_approval_probe_s {
  turbo_agent_runtime_store_t inner;
  json_value_t *root, *child;
  int calls, approved_calls, writes, live_grant_observed;
  int fail_callback, fail_commit, monitor, rewrite, last_value;
} nested_approval_probe_t;

static void nested_approval_observe(nested_approval_probe_t *probe) {
  if (probe->monitor && (turbo_agent_state_review_approved(probe->root) ||
                        turbo_agent_state_review_approved(probe->child)))
    probe->live_grant_observed = 1;
}

static int nested_approval_put(void *user, const char *collection, const char *id,
                                const char *text) {
  nested_approval_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) {
    ++probe->writes;
    nested_approval_observe(probe);
    if (probe->fail_commit && strstr(text, "\"committed\"")) return SALTS_EIO;
  }
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int nested_approval_get(void *user, const char *collection, const char *id, char **out) {
  nested_approval_probe_t *probe = user;
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int nested_approval_list(void *user, const char *collection, const char *key,
                                 const char *value, char **out) {
  nested_approval_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

/* This action counts invocations only; no command or filesystem action runs. */
static int nested_approval_action(const json_value_t *args, json_value_t **out, void *user) {
  nested_approval_probe_t *probe = user;
  (void)args;
  ++probe->calls;
  probe->last_value = (int)json_get_int(args, "value", -1);
  if (turbo_agent_current_tool_approval_granted()) ++probe->approved_calls;
  nested_approval_observe(probe);
  *out = probe->fail_callback ? NULL : turbo_action_result_create(1, "fixture result");
  return *out ? 0 : -1;
}

static int nested_approval_rewrite(turbo_agent_t *agent, json_value_t *state,
    const char *id, const char *name, char **args, void *user) {
  nested_approval_probe_t *probe = user;
  tstr replacement;
  (void)agent; (void)state; (void)id; (void)name;
  if (!probe->rewrite) return 0;
  replacement = tstr_dup("{\"value\":2}");
  if (!replacement) return -1;
  tstr_free(*args);
  *args = replacement;
  return 0;
}

static int nested_approval_transport(const char *request, char **response, void *user) {
  (void)request; (void)user;
  *response = NULL;
  return -1;
}

static json_value_t *nested_approval_child(json_value_t *root, int planner) {
  const char *key = planner ? "planner_state_versions" : "executor_state_versions";
  json_value_t *versions = json_object_get(root, key);
  json_value_t *child;
  if (!versions) {
    versions = json_create_array();
    if (!versions) return NULL;
    if (turbo_runtime_json_object_set(root, key, versions) != 0) {
      json_free(versions);
      return NULL;
    }
  }
  child = turbo_agent_state_create();
  if (!child) return NULL;
  if (turbo_runtime_json_array_append(versions, child) != 0) {
    json_free(child);
    return NULL;
  }
  return child;
}

static int nested_approval_model(json_value_t *child, int observe) {
  static const char action[] = "{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"same-call\","
      "\"name\":\"approval_probe\",\"arguments\":\"{\\\"value\\\":1}\"}]}";
  static const char observation[] = "{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"same-call\","
      "\"name\":\"approval_observe\",\"arguments\":\"{\\\"value\\\":1}\"}]}";
  const char *text = observe ? observation : action;
  json_value_t *event = json_parse(text, strlen(text));
  if (!event) return -1;
  if (turbo_runtime_json_array_append(json_object_get(child, "events"), event) != 0) {
    json_free(event);
    return -1;
  }
  return 0;
}

static int nested_approval_dispatch(turbo_graph_exec_ctx_t *ctx, turbo_agent_t *agent,
                                     int planner) {
  ctx->stop = 0;
  ctx->next_node = NULL;
  return planner ? turbo_agent_planner_tool_node(ctx, agent)
                 : turbo_agent_executor_tool_node(ctx, agent);
}

spec("nested Tool approval consumption") {
  static nested_approval_probe_t probe;
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
    turbo_action_tool_definition_t definition = {.name = "approval.probe",
        .description = "Count one approved fixture invocation",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},"
            "\"required\":[\"value\"],\"additionalProperties\":false}",
        .kind = TURBO_ACTION_DANGEROUS, .requires_approval = 1,
        .handler = nested_approval_action, .user_data = &probe};
    turbo_agent_runtime_store_t callbacks = {.put = nested_approval_put,
        .get = nested_approval_get, .list = nested_approval_list, .user_data = &probe};
    turbo_agent_execution_context_t context = {0};
    turbo_agent_middleware_t middleware = {.before_tool = nested_approval_rewrite, .user_data = &probe};
    probe = (nested_approval_probe_t){.monitor = 1};
    turbo_agent_execution_context_get(&saved);
    probe.root = turbo_agent_state_create(); check_not_null(probe.root);
    probe.inner = turbo_agent_runtime_store_memory_create(); check_not_null(probe.inner.put);
    runtime = turbo_agent_runtime_create(&callbacks); check_not_null(runtime);
    actions = turbo_action_tool_registry_create(); check_not_null(actions);
    check_equal(turbo_action_tool_registry_add(actions, &definition), TURBO_ACTION_TOOL_OK);
    definition.name = "approval.observe";
    definition.kind = TURBO_ACTION_OBSERVE;
    definition.requires_approval = 0;
    check_equal(turbo_action_tool_registry_add(actions, &definition), TURBO_ACTION_TOOL_OK);
    registry = turbo_action_tool_registry_build_tool_registry_bridge(actions); check_not_null(registry);
    config.model = "offline-fixture"; config.transport_fn = nested_approval_transport;
    config.tool_registry = registry;
    agent = turbo_agent_create(&config); check_not_null(agent);
    policy.allow_custom_tools = 1;
    policy.allow_shell = 1; /* DANGEROUS action admission; callback counts only. */
    check_equal(turbo_agent_set_tool_policy(agent, &policy), 0);
    check_equal(turbo_agent_add_action_policy_guardrail(agent, actions, &policy), 0);
    check_equal(turbo_agent_add_middleware(agent, &middleware), 0);
    graph = turbo_graph_create("nested-approval"); check_not_null(graph);
    check_equal(turbo_graph_add_node(graph, "tools", turbo_agent_executor_tool_node, agent),
        TURBO_GRAPH_EXEC_OK);
    ctx = (turbo_graph_exec_ctx_t){.graph = graph, .current_node = "tools", .state = probe.root};
    context.runtime = runtime; context.thread_id = "approval-thread"; context.run_id = "approval-run";
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
    json_free(probe.root);
    probe = (nested_approval_probe_t){0};
  }

  for (int planner = 0; planner < 2; ++planner) {
    for (int next_version = 0; next_version < 2; ++next_version) {
      const char *outcomes[] = {"success", "callback failure", "commit failure"};
      for (size_t outcome = 0; outcome < sizeof(outcomes) / sizeof(outcomes[0]); ++outcome) {
        it("requires a new parent approval for %s after %s (%s)",
            next_version ? "a new substate version" : "a new turn", outcomes[outcome],
            planner ? "planner" : "executor") {
          probe.fail_callback = outcome == 1;
          probe.fail_commit = outcome == 2;
          probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
          check_equal(nested_approval_model(probe.child, 0), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_true(ctx.stop); check_equal(probe.calls, 0); check_equal(probe.writes, 0);
          check_true(turbo_agent_state_review_required(probe.root));
          check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_equal(probe.calls, 1); check_equal(probe.approved_calls, 1);
          check_equal(probe.writes, 3);
          if (next_version) {
            probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
          }
          check_equal(nested_approval_model(probe.child, 0), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_equal(probe.calls, 1); check_equal(probe.writes, 3);
          check_true(ctx.stop);
          check_true(turbo_agent_state_review_required(probe.root));
          check_false(turbo_agent_state_review_approved(probe.root));
          check_equal(probe.live_grant_observed, 0);
          check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_equal(probe.calls, 2); check_equal(probe.approved_calls, 2);
          check_equal(probe.writes, 6);
          check_equal(probe.live_grant_observed, 0);
          check_false(turbo_agent_state_review_approved(probe.root));
          check_false(turbo_agent_state_review_approved(probe.child));
        }
      }
    }
    it("requires approval for final middleware arguments (%s)", planner ? "planner" : "executor") {
      probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
      check_equal(nested_approval_model(probe.child, 0), 0);
      check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_true(ctx.stop);
      check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
      probe.rewrite = 1;
      check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_true(ctx.stop); check_equal(probe.calls, 0); check_equal(probe.writes, 0);
      check_true(turbo_agent_state_review_required(probe.root));
      check_false(turbo_agent_state_review_approved(probe.root));
      check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
      check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_equal(probe.calls, 1); check_equal(probe.approved_calls, 1);
      check_equal(probe.last_value, 2); check_equal(probe.writes, 3);
      check_equal(probe.live_grant_observed, 0);
      check_false(turbo_agent_state_review_approved(probe.root));
      check_false(turbo_agent_state_review_approved(probe.child));
    }
    it("preserves unrelated workflow review in parent and child (%s)", planner ? "planner" : "executor") {
      probe.monitor = 0;
      probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
      check_equal(nested_approval_model(probe.child, 1), 0);
      check_equal(turbo_agent_state_request_review(probe.root, "workflow-level review"), 0);
      check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
      check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_equal(probe.calls, 1); check_equal(probe.approved_calls, 0);
      check_true(turbo_agent_state_review_approved(probe.root));
      check_true(turbo_agent_state_review_approved(probe.child));
      check_equal(turbo_agent_state_review_note(probe.root), "workflow-level review");
    }
  }
}
