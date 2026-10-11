#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"

static int inspect_node(turbo_graph_exec_ctx_t *ctx, void *data) {
  (void)ctx;
  (void)data;
  return 0;
}

spec("Session inspection ownership") {
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static json_value_t *input, *state, *inspect, *orchestration, *inbox, *history, *override, *fresh;

  before_each() {
    turbo_agent_session_config_t config = {0};
    config.thread_id = "inspect-thread";
    session = turbo_agent_session_create(&config);
    graph = turbo_graph_create("inspect-graph");
    input = turbo_agent_state_create_json_value();
    check_not_null(session);
    check_not_null(graph);
    check_not_null(input);
    check_equal(turbo_graph_add_json_value_node(graph, "start", inspect_node, NULL), 0);
    check_equal(turbo_graph_set_entry(graph, "start"), 0);
    check_equal(turbo_agent_state_set_active_agent(input, "planner"), 0);
    check_equal(turbo_agent_state_request_handoff(input, "executor", "delegate"), 0);
    check_equal(turbo_agent_state_append_supervisor_inbox_message(input, "supervisor", "original"),
                0);
  }

  after_each() {
    json_free(input);
    json_free(state);
    json_free(inspect);
    json_free(orchestration);
    json_free(inbox);
    json_free(history);
    json_free(override);
    json_free(fresh);
    input = state = inspect = orchestration = inbox = history = override = fresh = NULL;
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
  }

  it("returns independently owned projections that survive Session destruction") {
    json_value_t *control_supervisor;
    check_equal(
        turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL, NULL, &state), 0);
    check_equal(turbo_agent_session_get_supervisor_inspect(session, &inspect), 0);
    check_equal(turbo_agent_session_get_orchestration_inspect(session, &orchestration), 0);
    check_equal(turbo_agent_session_get_supervisor_inbox(session, &inbox), 0);
    check_equal(turbo_agent_session_get_supervisor_handoff_history(session, &history), 0);
    check_equal(
        turbo_runtime_json_object_set_string(json_array_get(inbox, 0), "text", "caller-change"),
        TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(json_object_get(inspect, "supervisor"),
                                                     "active_agent", "caller-agent"),
                TURBO_RUNTIME_JSON_OK);
    control_supervisor =
        json_object_get(json_object_get(inspect, "control_snapshot"), "supervisor");
    check_equal(json_get_string(control_supervisor, "active_agent"), "planner");
    check_equal(json_get_string(json_array_get(json_object_get(inspect, "inbox"), 0), "text"),
                "original");
    check_equal(turbo_agent_session_get_supervisor_inspect(session, &fresh), 0);
    check_equal(json_get_string(json_object_get(fresh, "supervisor"), "active_agent"), "planner");

    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(input);
    input = NULL;
    json_free(state);
    state = NULL;
    json_free(inspect);
    inspect = NULL;
    check_equal(json_get_string(json_array_get(inbox, 0), "text"), "caller-change");
    check_equal(json_get_string(json_array_get(history, 0), "target_agent"), "executor");
    inspect = json_clone(json_object_get(orchestration, "supervisor_inspect"));
    check_not_null(inspect);
    check_equal(json_get_string(json_array_get(json_object_get(inspect, "inbox"), 0), "text"),
                "original");
    check_not_null(json_object_get(orchestration, "thread_timeline"));
  }

  it("prepares an independent inbox override without changing persisted state") {
    check_equal(
        turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL, NULL, &state), 0);
    check_equal(turbo_agent_session_append_supervisor_inbox_message_json_value(
                    session, "reviewer", "proposed", &override),
                0);
    check_equal(turbo_agent_state_supervisor_inbox_count(override), 2);
    check_equal(turbo_agent_session_get_thread_state_json_value(session, &fresh), 0);
    check_equal(turbo_agent_state_supervisor_inbox_count(fresh), 1);
    check_equal(turbo_agent_state_supervisor_inbox_count(state), 1);
    check_equal(turbo_agent_session_get_supervisor_inbox(session, &inbox), 0);
    check_equal(json_array_size(inbox), 1);
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(json_get_string(turbo_agent_state_supervisor_inbox_at(override, 1), "text"),
                "proposed");
    check_equal(json_get_string(turbo_agent_state_supervisor_inbox_at(override, 0), "text"),
                "original");
  }

  it("fails without returning partial projections when the thread has no stored run") {
    check_equal(turbo_agent_session_get_supervisor_inbox(session, &inbox), -1);
    check_equal(turbo_agent_session_get_supervisor_handoff_history(session, &history), -1);
    check_equal(turbo_agent_session_get_supervisor_inspect(session, &inspect), -1);
    check_equal(turbo_agent_session_get_orchestration_inspect(session, &orchestration), -1);
    check_equal(turbo_agent_session_append_supervisor_inbox_message_json_value(
                    session, "reviewer", "proposed", &override),
                -1);
    check_null(inbox);
    check_null(history);
    check_null(inspect);
    check_null(orchestration);
    check_null(override);
    check_equal(turbo_agent_state_supervisor_inbox_count(input), 1);
  }
}
