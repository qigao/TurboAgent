#include "../src/turbo_agent_event_internal.h"
#include "tinytest.h"
#include "turbo_agent_graph.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

typedef struct event_probe_s {
  turbo_agent_runtime_store_t inner;
  const char *fail_collection;
  int fail_hook, drop_output;
  int calls, hooks, journal_writes, store_failures;
} event_probe_t;

static int event_put(void *user, const char *collection, const char *id, const char *text) {
  event_probe_t *probe = user;
  if (probe->fail_collection && strcmp(collection, probe->fail_collection) == 0) {
    ++probe->store_failures;
    return -1;
  }
  if (strcmp(collection, "agent_tool_journal") == 0) ++probe->journal_writes;
  return probe->inner.put(probe->inner.user_data, collection, id, text);
}

static int event_get(void *user, const char *collection, const char *id, char **out) {
  event_probe_t *probe = user;
  return probe->inner.get(probe->inner.user_data, collection, id, out);
}

static int event_list(void *user, const char *collection, const char *key, const char *value,
                      char **out) {
  event_probe_t *probe = user;
  return probe->inner.list(probe->inner.user_data, collection, key, value, out);
}

static void event_store_destroy(void *user) {
  event_probe_t *probe = user;
  if (probe->inner.user_data_free) probe->inner.user_data_free(probe->inner.user_data);
  probe->inner = (turbo_agent_runtime_store_t){0};
}

/* The public callback ABI transfers malloc-owned text. */
static char *event_copy(const char *text) {
  size_t size = strlen(text) + 1;
  char *copy = malloc(size);
  if (copy) memcpy(copy, text, size);
  return copy;
}

static int event_invoke(const char *arguments, char **out, void *user) {
  event_probe_t *probe = user;
  ++probe->calls;
  *out = event_copy(arguments);
  return *out ? 0 : -1;
}

static int event_after_tool(turbo_agent_t *agent, json_value_t *state, const char *id,
                            const char *name, const char *arguments, char **output,
                            turbo_tool_status_t status, void *user) {
  event_probe_t *probe = user;
  (void)agent;
  (void)state;
  (void)name;
  ++probe->hooks;
  /* Replays must start from the raw journal value, not the previous hook output. */
  if (status != TURBO_TOOL_OK || !*output || strcmp(*output, arguments) != 0) return -1;
  if (strcmp(id, "two") == 0 && probe->fail_hook) return -1;
  free(*output);
  *output = NULL;
  if (strcmp(id, "two") == 0 && probe->drop_output) return 0;
  *output = event_copy(strcmp(id, "one") == 0 ? "processed one" : "processed two");
  return *output ? 0 : -1;
}

static int event_transport(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static int event_finish(turbo_graph_exec_ctx_t *ctx, void *user) {
  (void)ctx;
  (void)user;
  return 0;
}

spec("tool event publication and recovery") {
  static event_probe_t probe;
  static turbo_agent_runtime_t *runtime;
  static turbo_tool_registry_t *registry;
  static turbo_agent_t *agent;
  static turbo_graph_t *graph;
  static json_value_t *input, *initial_summary, *summary, *state, *records, *checkpoint;
  static char *checkpoint_before, *checkpoint_after;

  before_each() {
    static const char pending[] =
        "{\"kind\":\"model\",\"output_text\":\"\",\"tool_calls\":["
        "{\"call_id\":\"one\",\"name\":\"probe\",\"arguments\":\"{\\\"n\\\":1}\"},"
        "{\"call_id\":\"two\",\"name\":\"probe\",\"arguments\":\"{\\\"n\\\":2}\"}]}";
    const char *pauses[] = {"tools"};
    turbo_graph_run_options_t options = {.interrupt_before_nodes = pauses,
                                         .interrupt_before_count = 1};
    turbo_agent_config_t config = {0};
    turbo_agent_middleware_t middleware = {.after_tool = event_after_tool, .user_data = &probe};
    turbo_tool_definition_t definition = {.name = "probe",
                                          .description = "Event recovery probe",
                                          .parameters_json = "{\"type\":\"object\"}",
                                          .handler = event_invoke,
                                          .user_data = &probe};
    turbo_agent_runtime_store_t callbacks;
    json_value_t *event;
    int append_rc;
    probe = (event_probe_t){0};
    probe.inner = turbo_agent_runtime_store_memory_create();
    check_not_null(probe.inner.put);
    callbacks = (turbo_agent_runtime_store_t){event_put, event_get, event_list, &probe,
                                              event_store_destroy};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    registry = turbo_tool_registry_create();
    check_not_null(registry);
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    config.model = "test";
    config.transport_fn = event_transport;
    config.tool_registry = registry;
    agent = turbo_agent_create(&config);
    check_not_null(agent);
    check_equal(turbo_agent_add_middleware(agent, &middleware), 0);
    graph = turbo_graph_create("tool-event-recovery");
    check_not_null(graph);
    check_equal(turbo_graph_add_node(graph, "tools", turbo_agent_tool_node, agent),
                TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_node(graph, "finish", event_finish, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_add_edge(graph, "tools", "finish", NULL, NULL), TURBO_GRAPH_EXEC_OK);
    check_equal(turbo_graph_set_entry(graph, "tools"), TURBO_GRAPH_EXEC_OK);
    input = turbo_agent_state_create();
    check_not_null(input);
    event = json_parse(pending, sizeof(pending) - 1);
    check_not_null(event);
    append_rc = turbo_agent_append_event(input, event);
    if (append_rc != 0) json_free(event);
    check_equal(append_rc, 0);
    check_equal(turbo_agent_runtime_exec_start(runtime, graph, input, &options, NULL,
                                               &initial_summary, &state),
                0);
    check_equal(json_get_string(initial_summary, "status"), "interrupted");
    check_not_null(json_get_string(initial_summary, "checkpoint_id"));
    check_equal(probe.calls, 0);
    json_free(state);
    state = NULL;
  }

  after_each() {
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_destroy(agent);
    agent = NULL;
    turbo_tool_registry_destroy(registry);
    registry = NULL;
    turbo_agent_runtime_destroy(runtime);
    runtime = NULL;
    event_store_destroy(&probe);
    json_free(input);
    json_free(initial_summary);
    json_free(summary);
    json_free(state);
    json_free(records);
    json_free(checkpoint);
    input = initial_summary = summary = state = records = checkpoint = NULL;
    json_serialize_free(checkpoint_before);
    json_serialize_free(checkpoint_after);
    checkpoint_before = checkpoint_after = NULL;
  }

  const struct {
    const char *name, *collection;
    int fail_hook, drop_output;
  } failures[] = {{"second output middleware", NULL, 1, 0},
                  {"missing second output", NULL, 0, 1},
                  {"checkpoint persistence", "checkpoints", 0, 0},
                  {"run persistence", "runs", 0, 0}};
  for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
    it("recovers complete results after %s fails without invoking tools again", failures[i].name) {
      const char *pauses[] = {"finish"};
      turbo_graph_run_options_t options = {.interrupt_before_nodes = pauses,
                                           .interrupt_before_count = 1,
                                           .skip_initial_interrupt = 1};
      turbo_agent_runtime_exec_options_t resume = {
          .scope = TURBO_RUNTIME_SCOPE_CHECKPOINT,
          .checkpoint_id = json_get_string(initial_summary, "checkpoint_id")};
      const json_value_t *outputs;
      check_equal(turbo_agent_runtime_get_checkpoint(runtime, resume.checkpoint_id, &checkpoint),
                  0);
      checkpoint_before = json_serialize(checkpoint, NULL);
      check_not_null(checkpoint_before);
      json_free(checkpoint);
      checkpoint = NULL;
      probe.fail_collection = failures[i].collection;
      probe.fail_hook = failures[i].fail_hook;
      probe.drop_output = failures[i].drop_output;
      check_not_equal(turbo_agent_runtime_exec_resume(runtime, graph, NULL, &options, &resume,
                                                      &summary, &state),
                      0);
      check_null(summary);
      check_equal(probe.calls, 2);
      check_equal(probe.hooks, 2);
      check_equal(probe.journal_writes, 6);
      check_equal(probe.store_failures, failures[i].collection ? 1 : 0);
      if (!failures[i].collection) {
        check_not_null(state);
        check_equal(turbo_agent_state_event_count(state), 1);
        /* The hook adapter also rejects a successful hook that drops its output. */
        check_equal(turbo_agent_state_model_error_phase(state), "middleware");
      }
      json_free(state);
      state = NULL;
      check_equal(turbo_agent_runtime_get_checkpoint(runtime, resume.checkpoint_id, &checkpoint),
                  0);
      checkpoint_after = json_serialize(checkpoint, NULL);
      check_equal(checkpoint_after, checkpoint_before);
      check_equal(turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "run_id",
                                                      json_get_string(initial_summary, "run_id"),
                                                      &records),
                  0);
      check_equal(json_array_size(records), 2);
      for (size_t j = 0; j < 2; ++j) {
        const json_value_t *record = json_array_get(records, j);
        check_equal(json_get_string(record, "phase"), "committed");
        check_equal(json_get_string(record, "output"),
                    strcmp(json_get_string(record, "call_id"), "one") == 0 ? "{\"n\":1}"
                                                                           : "{\"n\":2}");
      }
      probe.fail_collection = NULL;
      probe.fail_hook = probe.drop_output = 0;
      check_equal(turbo_agent_runtime_exec_resume(runtime, graph, NULL, &options, &resume, &summary,
                                                  &state),
                  0);
      check_equal(json_get_string(summary, "status"), "interrupted");
      check_equal(json_get_string(summary, "run_id"), json_get_string(initial_summary, "run_id"));
      check_equal(probe.calls, 2);
      check_equal(probe.hooks, 4);
      check_equal(probe.journal_writes, 6);
      check_equal(turbo_agent_state_event_count(state), 2);
      outputs = turbo_agent_state_tool_results_outputs(
          turbo_agent_state_latest_tool_results_event(state));
      check_equal(json_array_size(outputs), 2);
      for (size_t j = 0; j < 2; ++j) {
        const json_value_t *output = json_array_get(outputs, j);
        check_equal(json_get_string(output, "type"), "function_call_output");
        check_equal(json_get_string(output, "call_id"), j == 0 ? "one" : "two");
        check_equal(json_get_string(output, "output"), j == 0 ? "processed one" : "processed two");
      }
    }
  }
}

spec("event value ownership") {
  static json_value_t *state, *event, *item;
  after_each() {
    json_free(state);
    json_free(event);
    json_free(item);
    state = event = item = NULL;
  }

  const char *invalid_states[] = {"null", "[]", "{}", "{\"events\":null}", "{\"events\":{}}"};
  for (size_t i = 0; i < sizeof(invalid_states) / sizeof(invalid_states[0]); ++i) {
    it("retains an event after append to %s fails", invalid_states[i]) {
      state = json_parse(invalid_states[i], strlen(invalid_states[i]));
      event = turbo_agent_event_create("tool_results");
      check_not_null(event);
      check_not_equal(turbo_agent_append_event(state, event), 0);
      check_equal(json_get_string(event, "kind"), "tool_results");
      json_free(state);
      state = turbo_agent_state_create();
      check_not_null(state);
      check_equal(turbo_agent_append_event(state, event), 0);
      event = NULL;
      check_equal(turbo_agent_state_event_count(state), 1);
    }
  }

  const char *texts[] = {"", "plain output", "{invalid json", "{\"child_run_id\":5}",
                         "{\"child_run_id\":\"child\",\"parent_tool_call_id\":\"parent\"}"};
  for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
    it("preserves output text and copies valid optional lineage for case %zu", i) {
      char *text = event_copy(texts[i]);
      check_not_null(text);
      item = turbo_agent_tool_result_output_item_create("call", text);
      free(text);
      check_not_null(item);
      check_equal(json_get_string(item, "type"), "function_call_output");
      check_equal(json_get_string(item, "call_id"), "call");
      check_equal(json_get_string(item, "output"), texts[i]);
      if (i == 4) {
        check_equal(json_get_string(item, "child_run_id"), "child");
        check_equal(json_get_string(item, "parent_tool_call_id"), "parent");
      } else {
        check_null(json_object_get(item, "child_run_id"));
      }
    }
  }
}
