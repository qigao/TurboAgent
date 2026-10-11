#include "tinytest.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"

typedef struct query_capture_s {
  size_t calls;
  int releases;
  json_value_t *last;
} query_capture_t;

static void query_capture_event(const json_value_t *event, void *data) {
  query_capture_t *capture = data;
  ++capture->calls;
  json_free(capture->last);
  capture->last = json_clone(event);
}

static void query_release_capture(void *data) {
  query_capture_t *capture = data;
  ++capture->releases;
}

static void query_clear_capture(query_capture_t *capture) {
  json_free(capture->last);
  capture->last = NULL;
  capture->calls = 0;
}

static int query_node(turbo_graph_exec_ctx_t *ctx, void *data) {
  (void)ctx;
  (void)data;
  return 0;
}

spec("Session Runtime queries") {
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static json_value_t *input, *state, *first_summary, *latest_summary, *history, *result, *fresh,
      *patch;
  static query_capture_t replay, observer;
  static turbo_agent_observer_json_value_sink_t sink;

  before_each() {
    turbo_agent_session_config_t config = {0};
    const char *interrupt_nodes[] = {"end"};
    turbo_graph_run_options_t options = {0};
    config.thread_id = "query-thread";
    session = turbo_agent_session_create(&config);
    graph = turbo_graph_create("query-graph");
    input = turbo_agent_state_create_json_value();
    check_not_null(session);
    check_not_null(graph);
    check_not_null(input);
    check_equal(turbo_runtime_json_object_set_string(input, "query_marker", "original"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_graph_add_json_value_node(graph, "start", query_node, NULL), 0);
    check_equal(turbo_graph_add_json_value_node(graph, "end", query_node, NULL), 0);
    check_equal(turbo_graph_add_json_value_edge(graph, "start", "end", NULL, NULL), 0);
    check_equal(turbo_graph_set_entry(graph, "start"), 0);
    options.interrupt_before_nodes = interrupt_nodes;
    options.interrupt_before_count = 1;
    check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                &first_summary, &state),
                0);
    check_equal(json_get_string(first_summary, "status"), "interrupted");
    check_not_null(json_get_string(first_summary, "checkpoint_id"));
    json_free(state);
    state = NULL;
    /* A completed run without a new checkpoint leaves the older checkpoint cached.
     * This distinguishes history source selection from checkpoint defaults. */
    check_equal(turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL,
                                                &latest_summary, &state),
                0);
    check_equal(json_get_string(latest_summary, "status"), "completed");
    check_equal(turbo_agent_session_last_checkpoint_id(session),
                json_get_string(first_summary, "checkpoint_id"));
    check_equal(turbo_agent_session_last_run_id(session),
                json_get_string(latest_summary, "run_id"));
    sink.callback = query_capture_event;
    sink.user_data = &observer;
    sink.user_data_free = query_release_capture;
  }

  after_each() {
    turbo_agent_session_destroy(session);
    session = NULL;
    turbo_graph_destroy(graph);
    graph = NULL;
    json_free(input);
    json_free(state);
    json_free(first_summary);
    json_free(latest_summary);
    json_free(history);
    json_free(result);
    json_free(fresh);
    json_free(patch);
    input = state = first_summary = latest_summary = history = result = fresh = patch = NULL;
    query_clear_capture(&replay);
    query_clear_capture(&observer);
    replay.releases = observer.releases = 0;
  }

  it("uses the same explicit and cached source order for loading replay and observation") {
    const char *first_run = json_get_string(first_summary, "run_id");
    const char *checkpoint = json_get_string(first_summary, "checkpoint_id");
    const char *latest_run = json_get_string(latest_summary, "run_id");
    struct source_case {
      const char *run;
      const char *checkpoint;
      int interrupted;
    } cases[] = {{first_run, "missing", 1}, {latest_run, checkpoint, 0},
                 {NULL, checkpoint, 1},     {"", checkpoint, 1},
                 {NULL, NULL, 0},           {"", "", 0}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      check_equal(turbo_agent_session_load_history_events_json_value(session, cases[i].run,
                                                                     cases[i].checkpoint, &history),
                  0);
      if (cases[i].interrupted) check_true(json_array_size(history) >= 1);
      else check_equal(json_array_size(history), 0);
      check_equal(turbo_agent_session_replay_history_json_value(
                      session, cases[i].run, cases[i].checkpoint, query_capture_event, &replay),
                  0);
      check_equal(replay.calls, json_array_size(history));
      check_equal(turbo_agent_session_observe_history_json_value(session, cases[i].run,
                                                                 cases[i].checkpoint, &sink),
                  0);
      check_not_null(observer.last);
      check_equal(json_get_string(observer.last, "type"),
                  cases[i].interrupted ? "interrupted" : "completed");
      check_equal(observer.releases, 0);
      json_free(history);
      history = NULL;
      query_clear_capture(&replay);
      query_clear_capture(&observer);
    }
    check_equal(turbo_agent_session_last_run_id(session), latest_run);
    check_equal(turbo_agent_session_last_checkpoint_id(session), checkpoint);
  }

  it("does not retry a failed explicit checkpoint against the cached run") {
    check_equal(
        turbo_agent_session_load_history_events_json_value(session, NULL, "missing", &history), -1);
    check_null(history);
    check_equal(turbo_agent_session_replay_history_json_value(session, NULL, "missing",
                                                              query_capture_event, &replay),
                -1);
    check_equal(replay.calls, 0);
    check_equal(turbo_agent_session_observe_history_json_value(session, NULL, "missing", &sink),
                -1);
    check_equal(observer.calls, 0);
    check_equal(observer.releases, 0);
  }

  it("rejects history operations without a source before delivering callbacks") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_destroy(session);
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_session_load_history_events_json_value(session, NULL, NULL, &history),
                -1);
    check_null(history);
    check_equal(turbo_agent_session_replay_history_json_value(session, "", "", query_capture_event,
                                                              &replay),
                -1);
    check_equal(replay.calls, 0);
    check_equal(turbo_agent_session_observe_history_json_value(session, NULL, NULL, &sink), -1);
    check_equal(observer.calls, 0);
    check_equal(observer.releases, 0);
  }

  it("borrows historical sinks and allows copied callback events to outlive Session") {
    const char *checkpoint = json_get_string(first_summary, "checkpoint_id");
    check_equal(turbo_agent_session_replay_history_json_value(session, NULL, checkpoint,
                                                              query_capture_event, &replay),
                0);
    check_true(replay.calls >= 1);
    check_not_null(replay.last);
    check_equal(turbo_agent_session_observe_history_json_value(session, NULL, checkpoint, &sink),
                0);
    check_true(observer.calls >= 1);
    check_not_null(observer.last);
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(observer.releases, 0);
    check_equal(json_type(replay.last), JSON_OBJECT);
    check_equal(json_get_string(observer.last, "type"), "interrupted");
    check_not_null(json_object_get(observer.last, "event"));
  }

  it("keeps query defaults and explicit-only checkpoint APIs distinct without persisting "
     "overrides") {
    const char *checkpoint = json_get_string(first_summary, "checkpoint_id");
    check_equal(turbo_agent_session_get_run(session, "", &result), 0);
    check_equal(json_get_string(result, "id"), json_get_string(latest_summary, "run_id"));
    check_equal(turbo_agent_session_get_checkpoint(session, "", &fresh), 0);
    check_equal(json_get_string(fresh, "id"), checkpoint);
    json_free(result);
    json_free(fresh);
    result = fresh = NULL;
    check_equal(turbo_agent_session_get_checkpoint_context(session, "", &result), -1);
    check_null(result);
    patch = json_create_object();
    check_equal(turbo_runtime_json_object_set_string(patch, "query_marker", "changed"),
                TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_prepare_checkpoint_state_override_json_value(session, "", patch,
                                                                                 &result),
                0);
    check_equal(json_get_string(result, "query_marker"), "changed");
    check_equal(turbo_agent_session_get_checkpoint_state_json_value(session, "", &fresh), 0);
    check_equal(json_get_string(fresh, "query_marker"), "original");
    json_free(patch);
    patch = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(json_get_string(result, "query_marker"), "changed");
    check_equal(json_get_string(fresh, "query_marker"), "original");
  }
}
