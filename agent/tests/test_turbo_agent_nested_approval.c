#include "tinytest.h"
#include "turbo_agent_extensions.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_policy.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_store.h"
#include "turbo_runtime_control.h"
#include "../src/turbo_agent_runtime_internal.h"
#include "../src/turbo_agent_workflow_graph_internal.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <tstr.h>

typedef struct nested_approval_probe_s {
  turbo_agent_runtime_store_t inner;
  json_value_t *root, *child;
  int calls, approved_calls, writes, reads, live_grant_observed;
  int fail_callback, fail_commit, monitor, rewrite, last_value, fail_before;
  int fail_after, fail_read, stop_before, stop_after;
  const char *fail_phase;
  turbo_cancel_source_t *cancel_source;
  atomic_uint_fast64_t now;
} nested_approval_probe_t;

static uint64_t nested_approval_clock(void *user) {
  nested_approval_probe_t *probe = user;
  return atomic_load_explicit(&probe->now, memory_order_relaxed);
}

static void nested_approval_stop(nested_approval_probe_t *probe, int mode) {
  if (mode == 2) atomic_store_explicit(&probe->now, 200, memory_order_relaxed);
  else if (mode && turbo_cancel_source_reason(probe->cancel_source) == TURBO_CANCEL_NONE)
    check_equal(turbo_cancel_source_cancel(probe->cancel_source, TURBO_CANCEL_USER), SALTS_OK);
}

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
    if (probe->fail_phase) {
      json_value_t *record = json_parse(text, strlen(text));
      const char *phase = record ? json_get_string(record, "phase") : NULL;
      int fail = phase && !strcmp(phase, probe->fail_phase);
      json_free(record);
      if (fail) return SALTS_EIO;
    }
  }
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int nested_approval_get(void *user, const char *collection, const char *id, char **out) {
  nested_approval_probe_t *probe = user;
  if (!strcmp(collection, "agent_tool_journal")) {
    ++probe->reads;
    if (probe->fail_read) { *out = NULL; return SALTS_EIO; }
  }
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int nested_approval_list(void *user, const char *collection, const char *key,
                                 const char *value, char **out) {
  nested_approval_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

static int nested_approval_create(void *user, const char *collection,
                                  const char *id, const char *text) {
  nested_approval_probe_t *probe = user;
  if (!probe->inner.create_if_absent) return SALTS_ENOTSUP;
  if (!strcmp(collection, "agent_tool_journal")) {
    json_value_t *record = json_parse(text, strlen(text));
    const char *phase = record ? json_get_string(record, "phase") : NULL;
    int fail = probe->fail_phase && phase && !strcmp(phase, probe->fail_phase);
    json_free(record);
    ++probe->writes;
    nested_approval_observe(probe);
    if (fail) return SALTS_EIO;
  }
  return probe->inner.create_if_absent(probe->inner.user_data, collection, id, text);
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
  if (probe->fail_before) return -1;
  nested_approval_stop(probe, probe->stop_before);
  if (!probe->rewrite) return 0;
  replacement = tstr_dup("{\"value\":2}");
  if (!replacement) return -1;
  tstr_free(*args);
  *args = replacement;
  return 0;
}

static int nested_approval_after(turbo_agent_t *agent, json_value_t *state,
    const char *id, const char *name, const char *args, char **out,
    turbo_tool_status_t status, void *user) {
  nested_approval_probe_t *probe = user;
  (void)agent; (void)state; (void)id; (void)name; (void)args; (void)out; (void)status;
  nested_approval_stop(probe, probe->stop_after);
  return probe->fail_after ? -1 : 0;
}

static const json_value_t *nested_approval_output(const json_value_t *child) {
  const json_value_t *events = json_object_get(child, "events");
  const json_value_t *event = json_array_get(events, json_array_size(events) - 1);
  return json_array_get(json_object_get(event, "outputs"), 0);
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
  static turbo_cancel_token_t *cancel_token;
  static json_value_t *pending, *records;
  static char *journal_before, *journal_after;

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
        .get = nested_approval_get, .list = nested_approval_list,
        .user_data = &probe, .create_if_absent = nested_approval_create};
    turbo_agent_execution_context_t context = {0};
    turbo_agent_middleware_t middleware = {.before_tool = nested_approval_rewrite,
        .after_tool = nested_approval_after, .user_data = &probe};
    turbo_cancel_source_config_t control = {.struct_size = sizeof(control),
        .abi_version = TURBO_RUNTIME_CONTROL_ABI_VERSION, .deadline_mono_ms = 200,
        .monotonic_ms = nested_approval_clock, .clock_user_data = &probe};
    probe = (nested_approval_probe_t){.monitor = 1};
    atomic_init(&probe.now, 100);
    check_equal(turbo_cancel_source_create(&control, &probe.cancel_source), SALTS_OK);
    check_equal(turbo_cancel_source_token(probe.cancel_source, &cancel_token), SALTS_OK);
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
    context.cancel_token = cancel_token;
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
    turbo_cancel_token_release(cancel_token); cancel_token = NULL;
    turbo_cancel_source_destroy(probe.cancel_source);
    json_free(pending); json_free(records); pending = records = NULL;
    json_serialize_free(journal_before); json_serialize_free(journal_after);
    journal_before = journal_after = NULL;
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
    it("rejects a copied child approval after Host revokes its parent (%s)", planner ? "planner" : "executor") {
      probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
      check_equal(nested_approval_model(probe.child, 0), 0);
      check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_true(ctx.stop);
      check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
      probe.fail_before = 1;
      check_not_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
      check_true(turbo_agent_state_review_approved(probe.child));
      check_equal(probe.calls, 0); check_equal(probe.writes, 0);
      /* No execution is active while the Host changes this review decision. */
      check_equal(turbo_agent_state_set_review_approved(probe.root, 0), 0);
      probe.fail_before = 0;
      for (int retry = 0; retry < 2; ++retry) {
        int rc = nested_approval_dispatch(&ctx, agent, planner);
        check_equal(probe.calls, 0); check_equal(probe.writes, 0);
        check_not_equal(rc, 0);
        check_false(turbo_agent_state_review_approved(probe.root));
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
    const char *failures[] = {"planned", "started", "committed", "result middleware", "Journal read"};
    for (size_t failure = 0; failure < sizeof(failures) / sizeof(failures[0]); ++failure) {
      it("requires fresh approval on pending-state recovery after %s failure (%s)",
          failures[failure], planner ? "planner" : "executor") {
        probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
        check_equal(nested_approval_model(probe.child, 0), 0);
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        check_true(ctx.stop);
        /* Retain the unapproved pending state, not a durable approved grant. */
        pending = json_clone(probe.root); check_not_null(pending);
        check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
        probe.fail_phase = failure < 3 ? failures[failure] : NULL;
        probe.fail_after = failure == 3;
        probe.fail_read = failure == 4;
        int rc = nested_approval_dispatch(&ctx, agent, planner);
        if (failure == 0 || failure >= 3) check_not_equal(rc, 0);
        else check_equal(rc, 0);
        int original_calls = failure < 2 || failure == 4 ? 0 : 1;
        int original_writes = failure == 4 ? 0 : failure < 3 ? (int)failure + 1 : 3;
        check_equal(probe.calls, original_calls); check_equal(probe.writes, original_writes);
        check_false(turbo_agent_state_review_approved(probe.root));
        check_false(turbo_agent_state_review_approved(probe.child));
        check_equal(probe.live_grant_observed, 0);
        /* The same Runtime/Run/turn survives; only the pending state is restored. */
        json_free(probe.root); probe.root = pending; pending = NULL;
        ctx.state = probe.root;
        probe.child = json_array_get(json_object_get(probe.root,
            planner ? "planner_state_versions" : "executor_state_versions"), 0);
        check_not_null(probe.child);
        probe.fail_phase = NULL; probe.fail_after = 0; probe.fail_read = 0;
        int reads_before = probe.reads;
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        check_true(ctx.stop);
        check_equal(probe.calls, original_calls); check_equal(probe.writes, original_writes);
        check_equal(probe.reads, reads_before);
        check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        /* An acknowledged entry claim plus failed STARTED write remains
         * UNKNOWN even though the callback did not enter. */
        check_equal(probe.calls, failure == 1 ? 0 : 1);
        check_equal(probe.approved_calls, failure == 1 ? 0 : 1);
        check_equal(probe.writes, failure == 0 ? 4 : failure == 1 ? 2 : 3);
        check_equal(probe.live_grant_observed, 0);
        check_false(turbo_agent_state_review_approved(probe.root));
        check_false(turbo_agent_state_review_approved(probe.child));
        const json_value_t *output = nested_approval_output(probe.child);
        check_not_null(output);
        check_not_null(json_get_string(output, "output"));
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal",
            "call_id", "same-call", &records), SALTS_OK);
        check_equal(json_array_size(records), 1);
        const json_value_t *record = json_array_get(records, 0);
        check_equal(json_get_string(record, "phase"),
                    failure == 1 ? "planned" : failure == 2 ? "started" : "committed");
        if (failure == 1 || failure == 2) {
          check_null(json_get_string(record, "output"));
          check_not_null(strstr(json_get_string(output, "output"), "tool_side_effect_unknown"));
          check_null(strstr(json_get_string(output, "output"), "fixture result"));
        } else {
          check_equal(json_get_int(record, "status", -99), TURBO_TOOL_OK);
          check_not_null(strstr(json_get_string(output, "output"), "fixture result"));
        }
      }
    }
    for (int after = 0; after < 2; ++after) {
      for (int deadline = 0; deadline < 2; ++deadline) {
        it("consumes approval and withholds output on %s %s (%s)",
            deadline ? "deadline" : "cancellation", after ? "during result delivery" : "before callback",
            planner ? "planner" : "executor") {
          probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
          check_equal(nested_approval_model(probe.child, 0), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_true(ctx.stop);
          pending = json_clone(probe.root); check_not_null(pending);
          check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
          if (after) probe.stop_after = deadline ? 2 : 1;
          else probe.stop_before = deadline ? 2 : 1;
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_equal(probe.calls, after); check_equal(probe.writes, after ? 3 : 1);
          check_equal(probe.live_grant_observed, 0);
          check_false(turbo_agent_state_review_approved(probe.root));
          check_false(turbo_agent_state_review_approved(probe.child));
          const json_value_t *output = nested_approval_output(probe.child);
          check_not_null(output);
          check_not_null(json_get_string(output, "output"));
          check_not_null(strstr(json_get_string(output, "output"),
              deadline ? "tool_deadline_exceeded" : "tool_cancelled"));
          check_null(strstr(json_get_string(output, "output"), "fixture result"));
          check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal",
              "call_id", "same-call", &records), SALTS_OK);
          check_equal(json_array_size(records), 1);
          const json_value_t *record = json_array_get(records, 0);
          check_equal(json_get_string(record, "phase"), after ? "committed" : "planned");
          if (after) check_not_null(strstr(json_get_string(record, "output"), "fixture result"));
          /* No deadline reset: a stopped replay remains stopped even after
           * another explicit Host approval. */
          json_free(probe.root); probe.root = pending; pending = NULL; ctx.state = probe.root;
          probe.child = json_array_get(json_object_get(probe.root,
              planner ? "planner_state_versions" : "executor_state_versions"), 0);
          check_not_null(probe.child);
          int reads_before = probe.reads;
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_true(ctx.stop); check_equal(probe.reads, reads_before);
          check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
          check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
          check_equal(probe.calls, after); check_equal(probe.writes, after ? 3 : 1);
          output = nested_approval_output(probe.child); check_not_null(output);
          check_not_null(json_get_string(output, "output"));
          check_not_null(strstr(json_get_string(output, "output"),
              deadline ? "tool_deadline_exceeded" : "tool_cancelled"));
          check_null(strstr(json_get_string(output, "output"), "fixture result"));
          check_false(turbo_agent_state_review_approved(probe.root));
          check_false(turbo_agent_state_review_approved(probe.child));
        }
      }
    }
    for (int unknown = 0; unknown < 2; ++unknown) {
      it("checks current authority before %s Journal replay despite fresh approval (%s)",
          unknown ? "unknown" : "committed", planner ? "planner" : "executor") {
        probe.child = nested_approval_child(probe.root, planner); check_not_null(probe.child);
        check_equal(nested_approval_model(probe.child, 0), 0);
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        check_true(ctx.stop);
        pending = json_clone(probe.root); check_not_null(pending);
        check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
        probe.fail_commit = unknown;
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        check_equal(probe.calls, 1); check_equal(probe.writes, 3);
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal",
            "call_id", "same-call", &records), SALTS_OK);
        check_equal(json_array_size(records), 1);
        check_equal(json_get_string(json_array_get(records, 0), "phase"),
            unknown ? "started" : "committed");
        journal_before = json_serialize(records, NULL); check_not_null(journal_before);
        json_free(records); records = NULL;
        json_free(probe.root); probe.root = pending; pending = NULL; ctx.state = probe.root;
        probe.child = json_array_get(json_object_get(probe.root,
            planner ? "planner_state_versions" : "executor_state_versions"), 0);
        check_not_null(probe.child);
        /* Quiescent Host policy changes only. Review is not a capability grant. */
        turbo_agent_policy_t denied = turbo_agent_policy_default();
        check_equal(turbo_agent_set_tool_policy(agent, &denied), 0);
        check_equal(turbo_agent_state_set_review_approved(probe.root, 1), 0);
        int reads_before = probe.reads;
        check_equal(nested_approval_dispatch(&ctx, agent, planner), 0);
        check_equal(probe.calls, 1); check_equal(probe.writes, 3);
        check_equal(probe.reads, reads_before);
        const json_value_t *output = nested_approval_output(probe.child);
        check_not_null(output); check_not_null(json_get_string(output, "output"));
        check_null(strstr(json_get_string(output, "output"), "fixture result"));
        check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal",
            "call_id", "same-call", &records), SALTS_OK);
        journal_after = json_serialize(records, NULL); check_not_null(journal_after);
        check_equal(journal_after, journal_before);
        check_equal(probe.live_grant_observed, 0);
      }
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
