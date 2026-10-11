#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_event.h"

static int child_inspect_node(turbo_graph_exec_ctx_t *ctx, void *data) {
  (void)ctx;
  (void)data;
  return 0;
}

spec("Session child inspection") {
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static const char *interrupt_nodes[] = {"start"};
  static turbo_graph_run_options_t options;
  static json_value_t *input, *state, *item, *inspect, *fresh, *multi, *events, *trace, *summary;

  before_each() {
    turbo_agent_session_config_t config = {0};
    config.thread_id = "child-inspect-thread";
    options.interrupt_before_nodes = interrupt_nodes;
    options.interrupt_before_count = 1;
    session = turbo_agent_session_create(&config);
    graph = turbo_graph_create("child-inspect-graph");
    input = turbo_agent_state_create_json_value();
    item = json_create_object();
    check_not_null(session);
    check_not_null(graph);
    check_not_null(input);
    check_not_null(item);
    check_equal(turbo_graph_add_json_value_node(graph, "start", child_inspect_node, NULL), 0);
    check_equal(turbo_graph_set_entry(graph, "start"), 0);
    trace = turbo_event_trace_create_json_value("agent.trace", "finish", "done", 0);
    check_equal(turbo_runtime_json_object_set_string(trace, "marker", "original"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_state_add_trace_event_json_value(input, trace), 0);
    json_free(trace);
    trace = NULL;
    check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                &summary, &state),
                0);
    check_equal(turbo_runtime_json_object_set_string(item, "child_run_id",
                                                     turbo_agent_session_last_run_id(session)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(
                    item, "child_checkpoint_id", turbo_agent_session_last_checkpoint_id(session)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(item, "child_thread_id",
                                                     turbo_agent_session_thread_id(session)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(item, "parent_tool_name", "delegate"),
                TURBO_RUNTIME_JSON_OK);
  }

  after_each() {
    json_free(input);
    json_free(state);
    json_free(item);
    json_free(inspect);
    json_free(fresh);
    json_free(multi);
    json_free(events);
    json_free(trace);
    json_free(summary);
    input = state = item = inspect = fresh = multi = events = trace = summary = NULL;
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
  }

  it("owns reports and nested records independently of other queries and Session") {
    json_value_t *child;
    check_equal(turbo_agent_session_get_child_inspect(session, item, &inspect), 0);
    check_equal(turbo_agent_session_get_child_multi_agent_inspect(session, item, &multi), 0);
    check_equal(turbo_runtime_json_object_set_string(json_object_get(inspect, "run"), "status",
                                                     "caller-change"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(
        turbo_runtime_json_object_set_string(
            json_array_get(json_object_get(inspect, "trace_events"), 0), "marker", "caller-change"),
        TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_get_child_inspect(session, item, &fresh), 0);
    check_equal(json_get_string(json_object_get(fresh, "run"), "status"), "interrupted");
    check_equal(
        json_get_string(json_array_get(json_object_get(fresh, "trace_events"), 0), "marker"),
        "original");
    check_equal(turbo_runtime_json_object_set_string(item, "parent_tool_name", "changed"),
                TURBO_RUNTIME_JSON_OK);
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(item);
    item = NULL;
    json_free(input);
    input = NULL;
    json_free(state);
    state = NULL;
    json_free(inspect);
    inspect = NULL;
    child = json_object_get(multi, "child_orchestration_inspect");
    check_equal(json_get_string(child, "parent_tool_name"), "delegate");
    check_equal(json_type(json_object_get(child, "parent_agent_run_id")), JSON_NULL);
    child = json_object_get(child, "child_inspect");
    check_equal(json_get_string(json_object_get(child, "run"), "status"), "interrupted");
    check_equal(
        json_get_string(json_array_get(json_object_get(child, "trace_events"), 0), "marker"),
        "original");
    check_equal(json_type(json_object_get(child, "thread_timeline")), JSON_OBJECT);
    check_equal(json_type(json_object_get(child, "branch_tree")), JSON_OBJECT);
    check_equal(json_type(json_object_get(fresh, "history_events")), JSON_ARRAY);
  }

  it("keeps checkpoint and thread projections optional when their ids are absent") {
    check_equal(turbo_runtime_json_object_set_string(item, "child_checkpoint_id", ""),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(item, "child_thread_id", ""),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_get_child_orchestration_inspect(session, item, &inspect), 0);
    fresh = json_clone(json_object_get(inspect, "child_inspect"));
    check_not_null(fresh);
    check_equal(json_type(json_object_get(fresh, "latest_checkpoint")), JSON_NULL);
    check_equal(json_type(json_object_get(fresh, "checkpoint_context")), JSON_NULL);
    check_equal(json_type(json_object_get(fresh, "thread_timeline")), JSON_NULL);
    check_equal(json_type(json_object_get(fresh, "branch_tree")), JSON_NULL);
    check_equal(json_type(json_object_get(fresh, "history_events")), JSON_ARRAY);
    check_equal(
        json_get_string(json_array_get(json_object_get(fresh, "trace_events"), 0), "marker"),
        "original");
  }

  it("keeps the branch result when the optional timeline query fails") {
    check_equal(turbo_runtime_json_object_set_string(item, "child_thread_id", "missing-thread"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_get_child_thread_timeline_json_value(session, item, &events),
                -1);
    check_null(events);
    check_equal(turbo_agent_session_get_child_branch_tree(session, item, &fresh), 0);
    check_equal(json_array_size(json_object_get(fresh, "branches")), 0);
    check_equal(turbo_agent_session_get_child_inspect(session, item, &inspect), 0);
    check_equal(json_type(json_object_get(inspect, "thread_timeline")), JSON_NULL);
    check_equal(json_type(json_object_get(inspect, "branch_tree")), JSON_OBJECT);
    check_equal(json_get_string(json_object_get(inspect, "branch_tree"), "thread_id"),
                "missing-thread");
    check_equal(
        json_array_size(json_object_get(json_object_get(inspect, "branch_tree"), "branches")), 0);
    check_equal(json_type(json_object_get(inspect, "latest_checkpoint")), JSON_OBJECT);
    check_equal(json_type(json_object_get(inspect, "checkpoint_context")), JSON_OBJECT);
    check_equal(json_get_string(json_object_get(inspect, "run"), "status"), "interrupted");
  }

  it("uses the explicit checkpoint before the run and propagates checkpoint failures") {
    json_free(state);
    state = NULL;
    trace = turbo_event_trace_create_json_value("agent.trace", "finish", "done", 0);
    check_equal(turbo_runtime_json_object_set_string(trace, "marker", "second"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_state_add_trace_event_json_value(input, trace), 0);
    json_free(trace);
    trace = NULL;
    check_equal(
        turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL, NULL, &state),
        0);
    check_equal(turbo_runtime_json_object_set_string(item, "child_run_id",
                                                     turbo_agent_session_last_run_id(session)),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_get_child_trace_events_json_value(session, item, &trace), 0);
    check_equal(json_array_size(trace), 1);
    check_equal(json_get_string(json_array_get(trace, 0), "marker"), "original");
    json_free(trace);
    trace = NULL;
    check_equal(turbo_agent_session_load_child_history_events_json_value(session, item, &events),
                0);
    json_free(events);
    events = NULL;

    check_equal(turbo_runtime_json_object_set_string(item, "child_checkpoint_id", "missing"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_load_child_history_events_json_value(session, item, &events),
                -1);
    check_null(events);
    check_equal(turbo_agent_session_get_child_trace_events_json_value(session, item, &trace), -1);
    check_null(trace);
    check_equal(turbo_agent_session_get_child_inspect(session, item, &inspect), -1);
    check_null(inspect);
    check_equal(turbo_agent_session_get_child_orchestration_inspect(session, item, &inspect), -1);
    check_null(inspect);
    check_equal(turbo_agent_session_get_child_multi_agent_inspect(session, item, &multi), -1);
    check_null(multi);

    check_equal(turbo_runtime_json_object_set_string(item, "child_checkpoint_id", ""),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_load_child_history_events_json_value(session, item, &events),
                0);
    check_equal(turbo_agent_session_get_child_trace_events_json_value(session, item, &trace), 0);
    check_equal(json_array_size(trace), 2);
    check_equal(json_get_string(json_array_get(trace, 1), "marker"), "second");
  }

  it("does not publish partial reports when the referenced run is missing") {
    check_equal(turbo_runtime_json_object_set_string(item, "child_run_id", "missing"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_get_child_inspect(session, item, &inspect), -1);
    check_null(inspect);
    check_equal(turbo_agent_session_get_child_orchestration_inspect(session, item, &inspect), -1);
    check_null(inspect);
    check_equal(turbo_agent_session_get_child_multi_agent_inspect(session, item, &multi), -1);
    check_null(multi);
  }
}
