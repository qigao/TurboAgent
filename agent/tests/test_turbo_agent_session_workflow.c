#include "tinytest.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_session.h"
#include "turbo_agent_state.h"
#include "turbo_retriever.h"

#include <string.h>

typedef struct workflow_capture_s {
  int queries;
  int releases;
  int transports;
  json_value_t *request;
} workflow_capture_t;

static int workflow_transport(const char *request, char **response, void *data) {
  workflow_capture_t *capture = data;
  (void)request;
  *response = NULL;
  ++capture->transports;
  return -1;
}

static int workflow_query(void *data, const char *query,
                          const turbo_retriever_query_options_t *options, json_value_t **results) {
  static const char response[] = "[{\"uri\":\"memory://retrieved\",\"text\":\"retrieved text\"}]";
  workflow_capture_t *capture = data;
  ++capture->queries;
  json_free(capture->request);
  capture->request = json_create_object();
  if (turbo_runtime_json_object_set_string(capture->request, "query", query) != 0 ||
      turbo_runtime_json_object_set_string(capture->request, "kind",
                                           options->kind ? options->kind : "") != 0 ||
      turbo_runtime_json_object_set_string(capture->request, "uri_prefix",
                                           options->uri_prefix ? options->uri_prefix : "") != 0 ||
      turbo_runtime_json_object_set_int64(capture->request, "limit", (int64_t)options->limit) != 0)
    return -1;
  *results = json_parse(response, sizeof(response) - 1);
  return *results ? 0 : -1;
}

static void workflow_release(void *data) {
  workflow_capture_t *capture = data;
  ++capture->releases;
}

spec("Session workflow factories") {
  static turbo_agent_session_t *session;
  static turbo_retriever_t *retriever;
  static turbo_agent_knowledge_store_t *store;
  static turbo_graph_t *graph, *other_graph;
  static workflow_capture_t capture;
  static json_value_t *settings, *input, *state, *summary, *diagnostics;
  static const char *interrupt_nodes[] = {"planner"};
  static turbo_graph_run_options_t options;

  before_each() {
    static const char source[] =
        "{\"retriever_query\":\"fixed retrieval\",\"kind\":\"note\","
        "\"uri_prefix\":\"memory://wanted/\",\"scope\":\"project\",\"knowledge_query\":\"needle\"}";
    turbo_agent_session_config_t config = {0};
    turbo_retriever_config_t retriever_config = {workflow_query, &capture, workflow_release};
    retriever = turbo_retriever_create(&retriever_config);
    store = turbo_agent_knowledge_store_sqlite_open(":memory:");
    settings = json_parse(source, sizeof(source) - 1);
    check_not_null(retriever);
    check_not_null(store);
    check_not_null(settings);
    config.agent_config.model = "test-model";
    config.agent_config.transport_fn = workflow_transport;
    config.agent_config.transport_user_data = &capture;
    config.knowledge_store = store;
    config.knowledge_query = json_get_string(settings, "knowledge_query");
    config.knowledge_kind = json_get_string(settings, "kind");
    config.knowledge_uri_prefix = json_get_string(settings, "uri_prefix");
    config.knowledge_limit = 1;
    config.retriever = retriever;
    config.retriever_query = json_get_string(settings, "retriever_query");
    config.retriever_kind = json_get_string(settings, "kind");
    config.retriever_uri_prefix = json_get_string(settings, "uri_prefix");
    config.retriever_scope = json_get_string(settings, "scope");
    config.retriever_limit = 2;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    json_free(settings);
    settings = NULL;
    input = turbo_agent_session_create_input_state_json_value("user query");
    check_not_null(input);
    options.interrupt_before_nodes = interrupt_nodes;
    options.interrupt_before_count = 1;
  }

  after_each() {
    turbo_graph_destroy(graph);
    turbo_graph_destroy(other_graph);
    graph = other_graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    turbo_retriever_destroy(retriever);
    retriever = NULL;
    turbo_agent_knowledge_store_close(store);
    store = NULL;
    json_free(capture.request);
    capture = (workflow_capture_t){0};
    json_free(settings);
    json_free(input);
    json_free(state);
    json_free(summary);
    json_free(diagnostics);
    settings = input = state = summary = diagnostics = NULL;
  }

  it("executes a retained retriever config after caller config storage is released") {
    const json_value_t *layer;
    graph = turbo_agent_session_create_retriever_engineering_graph(session);
    other_graph = turbo_agent_session_create_knowledge_engineering_graph(session);
    check_not_null(graph);
    check_not_null(other_graph);
    check_equal(turbo_graph_get_entry(graph), "retriever");
    check_equal(turbo_graph_get_entry(other_graph), "knowledge");
    check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                &summary, &state),
                0);
    check_equal(json_get_string(summary, "status"), "interrupted");
    check_equal(capture.transports, 0);
    check_equal(capture.queries, 1);
    check_equal(json_get_string(capture.request, "query"), "fixed retrieval");
    check_equal(json_get_string(capture.request, "kind"), "note");
    check_equal(json_get_string(capture.request, "uri_prefix"), "memory://wanted/");
    check_equal(json_get_int(capture.request, "limit", 0), 2);
    check_equal(turbo_agent_state_memory_layer_count(state), 1);
    layer = turbo_agent_state_memory_layer_at(state, 0);
    check_equal(json_get_string(layer, "scope"), "project");
    check_equal(json_get_string(layer, "text"), "retrieved text");
    turbo_graph_destroy(graph);
    turbo_graph_destroy(other_graph);
    graph = other_graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(capture.releases, 0);
    check_equal(json_get_string(layer, "path"), "memory://retrieved");
    turbo_retriever_destroy(retriever);
    retriever = NULL;
    check_equal(capture.releases, 1);
  }

  it("runs the knowledge context node with copied query filters and limit") {
    turbo_agent_knowledge_document_t document = {0};
    document.id = "wanted";
    document.uri = "memory://wanted/doc";
    document.kind = "note";
    check_equal(
        turbo_agent_knowledge_store_upsert_text(store, &document, "needle retained context", 256),
        0);
    document.id = "wanted-two";
    document.uri = "memory://wanted/other";
    check_equal(
        turbo_agent_knowledge_store_upsert_text(store, &document, "needle retained context", 256),
        0);
    document.id = "wrong-kind";
    document.uri = "memory://wanted/wrong-kind";
    document.kind = "other";
    check_equal(turbo_agent_knowledge_store_upsert_text(store, &document, "needle wrong kind", 256),
                0);
    document.id = "wrong-uri";
    document.uri = "memory://other/doc";
    document.kind = "note";
    check_equal(turbo_agent_knowledge_store_upsert_text(store, &document, "needle wrong uri", 256),
                0);
    graph = turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_KNOWLEDGE_ENGINEERING);
    check_not_null(graph);
    check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                &summary, &state),
                0);
    check_equal(capture.transports, 0);
    check_equal(turbo_agent_state_memory_layer_count(state), 1);
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "text"),
                "needle retained context");
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "scope"), "knowledge");
    turbo_graph_destroy(graph);
    graph = NULL;
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(turbo_agent_knowledge_store_stats(store, &diagnostics), 0);
    check_not_null(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "path"));
    check_true(strstr(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "path"),
                      "memory://wanted/") ==
               json_get_string(turbo_agent_state_memory_layer_at(state, 0), "path"));
  }

  it("uses the latest user query when the configured retriever query is empty") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_destroy(session);
    config.agent_config.model = "test-model";
    config.agent_config.transport_fn = workflow_transport;
    config.agent_config.transport_user_data = &capture;
    config.retriever = retriever;
    config.retriever_query = "";
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_equal(turbo_agent_state_add_user_message(input, "latest query"), 0);
    graph = turbo_agent_session_create_preset_graph(
        session, TURBO_AGENT_SESSION_WORKFLOW_RETRIEVER_ENGINEERING);
    check_not_null(graph);
    check_equal(turbo_agent_session_start_graph(session, graph, input, &options, NULL, NULL,
                                                &summary, &state),
                0);
    check_equal(json_get_string(capture.request, "query"), "latest query");
    check_equal(json_get_int(capture.request, "limit", -1), 0);
    check_equal(json_get_string(turbo_agent_state_memory_layer_at(state, 0), "scope"), "retriever");
    check_equal(capture.transports, 0);
  }

  it("rejects missing prerequisites and unknown presets without invoking a backend") {
    turbo_agent_session_config_t config = {0};
    turbo_agent_session_destroy(session);
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_null(turbo_agent_session_create_loop_graph(session));
    check_null(turbo_agent_session_create_review_graph(session));
    check_null(turbo_agent_session_create_engineering_graph(session));
    check_null(turbo_agent_session_create_knowledge_engineering_graph(session));
    check_null(turbo_agent_session_create_retriever_engineering_graph(session));
    check_null(turbo_agent_session_create_preset_graph(NULL, TURBO_AGENT_SESSION_WORKFLOW_LOOP));
    turbo_agent_session_destroy(session);
    config.agent_config.model = "test-model";
    config.agent_config.transport_fn = workflow_transport;
    config.agent_config.transport_user_data = &capture;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_null(turbo_agent_session_create_knowledge_engineering_graph(session));
    check_null(turbo_agent_session_create_retriever_engineering_graph(session));
    check_null(
        turbo_agent_session_create_preset_graph(session, (turbo_agent_session_workflow_kind_t)99));
    check_equal(capture.queries, 0);
    check_equal(capture.transports, 0);
    check_equal(capture.releases, 0);
  }
}
