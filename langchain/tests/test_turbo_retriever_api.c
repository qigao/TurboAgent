#include "tinytest.h"
#include "turbo_agent.h"
#include "turbo_agent_knowledge_store.h"
#include "turbo_agent_state.h"
#include "turbo_retriever.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_SEP "/"
#endif

typedef struct retriever_test_state_s {
  int call_count;
  int freed;
} retriever_test_state_t;

static int retriever_test_dummy_transport(const char *request_json,
                                          char **out_response_json,
                                          void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static turbo_agent_t *retriever_test_agent_create(const char *model) {
  turbo_agent_config_t config = {0};

  config.model = model;
  config.transport_fn = retriever_test_dummy_transport;
  return turbo_agent_create(&config);
}

static char *retriever_test_strdup(const char *text) {
  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);

  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static char *retriever_test_temp_dir(void) {
  char path[512];
#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  snprintf(path, sizeof(path), "%sturbonet_retriever_%llu", temp_dir,
           (unsigned long long)GetTickCount64());
#else
  snprintf(path, sizeof(path), "/tmp/turbonet_retriever_%lu",
           (unsigned long)getpid());
#endif
  check_int_eq(TEST_MKDIR(path), 0);
  return retriever_test_strdup(path);
}

static char *retriever_test_path(const char *dir, const char *name) {
  char path[512];

  snprintf(path, sizeof(path), "%s%s%s", dir, TEST_SEP, name);
  return retriever_test_strdup(path);
}

static int retriever_test_query(
    void *user_data, const char *query,
    const turbo_retriever_query_options_t *options, json_value_t **out_results_json) {
  retriever_test_state_t *state = (retriever_test_state_t *)user_data;
  json_value_t *results;
  json_value_t *item;

  check_not_null(state);
  check_not_null(query);
  check_not_null(options);
  check_not_null(out_results_json);
  state->call_count++;
  check_str_eq(query, "planner context");
  check_str_eq(options->kind, "note");
  check_size_eq(options->limit, 3);

  results = turbo_json_create_array();
  item = turbo_json_create_object();
  check_not_null(results);
  check_not_null(item);
  turbo_json_object_set_string(item, "document_id", "custom-1");
  turbo_json_object_set_string(item, "text", "custom retriever result");
  turbo_json_array_add(results, item);
  *out_results_json = results;
  return 0;
}

static void retriever_test_free(void *user_data) {
  retriever_test_state_t *state = (retriever_test_state_t *)user_data;

  check_not_null(state);
  state->freed = 1;
}

spec("turbo retriever api") {
  it("should wrap a custom retriever query callback") {
    retriever_test_state_t state = {0};
    turbo_retriever_config_t config = {0};
    turbo_retriever_query_options_t options = {0};
    turbo_retriever_t *retriever;
    json_value_t *results = NULL;
    const json_value_t *first;

    config.query = retriever_test_query;
    config.user_data = &state;
    config.user_data_free = retriever_test_free;
    options.kind = "note";
    options.limit = 3;

    retriever = turbo_retriever_create(&config);
    check_not_null(retriever);
    check_int_eq(turbo_retriever_query(retriever, "planner context", &options,
                                       &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"), "custom-1");
    check_int_eq(state.call_count, 1);

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    check_int_eq(state.freed, 1);
  }

  it("should build generic context from retriever results") {
    retriever_test_state_t state = {0};
    turbo_retriever_config_t config = {0};
    turbo_retriever_query_options_t options = {0};
    turbo_retriever_t *retriever;
    json_value_t *context = NULL;
    const json_value_t *layers;
    const json_value_t *evidence;

    config.query = retriever_test_query;
    config.user_data = &state;
    options.kind = "note";
    options.limit = 3;

    retriever = turbo_retriever_create(&config);
    check_not_null(retriever);
    check_int_eq(turbo_retriever_build_context(
                     retriever, "planner context", &options, &context),
                 0);
    check_str_eq(turbo_json_get_string(context, "query"), "planner context");
    check_str_eq(turbo_json_get_string(context, "kind"), "note");
    check_int_eq(turbo_json_get_int(context, "layer_count", 0), 1);
    check_not_null(strstr(turbo_json_get_string(context, "context_text"),
                          "custom retriever result"));
    layers = turbo_json_object_get(context, "layers");
    evidence = turbo_json_object_get(context, "evidence");
    check_size_eq(turbo_json_array_size(layers), 1);
    check_size_eq(turbo_json_array_size(evidence), 1);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(evidence, 0),
                                       "document_id"),
                 "custom-1");

    turbo_free_json(&context);
    turbo_retriever_destroy(retriever);
  }

  it("should load retriever results into agent memory context") {
    retriever_test_state_t state = {0};
    turbo_retriever_config_t config = {0};
    turbo_retriever_query_options_t options = {0};
    turbo_retriever_t *retriever;
    json_value_t *agent_state = turbo_agent_state_create();
    char *memory_text;

    config.query = retriever_test_query;
    config.user_data = &state;
    options.kind = "note";
    options.limit = 3;

    retriever = turbo_retriever_create(&config);
    check_not_null(retriever);
    check_not_null(agent_state);
    check_int_eq(turbo_retriever_load_context(
                     retriever, agent_state, "planner context", &options,
                     "vector"),
                 0);
    check_size_eq(turbo_agent_state_memory_layer_count(agent_state), 1);
    check_str_eq(turbo_json_get_string(
                     turbo_agent_state_memory_layer_at(agent_state, 0), "scope"),
                 "vector");
    check_not_null(strstr(turbo_json_get_string(
                              turbo_agent_state_memory_layer_at(agent_state, 0),
                              "text"),
                          "custom retriever result"));
    memory_text = turbo_agent_state_memory_context_text(agent_state);
    check_not_null(memory_text);
    check_not_null(strstr(memory_text, "custom retriever result"));

    free(memory_text);
    turbo_free_json(&agent_state);
    turbo_retriever_destroy(retriever);
  }

  it("should expose a graph node that loads retriever context") {
    retriever_test_state_t state = {0};
    turbo_retriever_config_t retriever_config = {0};
    turbo_retriever_context_config_t context_config = {0};
    turbo_retriever_t *retriever;
    turbo_graph_exec_ctx_t ctx = {0};
    json_value_t *agent_state = turbo_agent_state_create();

    retriever_config.query = retriever_test_query;
    retriever_config.user_data = &state;
    retriever = turbo_retriever_create(&retriever_config);
    check_not_null(retriever);
    check_not_null(agent_state);
    check_int_eq(turbo_agent_state_add_user_message(agent_state,
                                                    "planner context"),
                 0);
    context_config.retriever = retriever;
    context_config.kind = "note";
    context_config.limit = 3;
    context_config.scope = "retriever";
    ctx.state = agent_state;

    check_int_eq(turbo_retriever_context_node(&ctx, &context_config), 0);
    check_size_eq(turbo_agent_state_memory_layer_count(agent_state), 1);
    check_str_eq(turbo_json_get_string(
                     turbo_agent_state_memory_layer_at(agent_state, 0), "scope"),
                 "retriever");
    check_not_null(strstr(turbo_json_get_string(
                              turbo_agent_state_memory_layer_at(agent_state, 0),
                              "text"),
                          "custom retriever result"));

    turbo_free_json(&agent_state);
    turbo_retriever_destroy(retriever);
  }

  it("should install a retriever-enhanced engineering loop") {
    retriever_test_state_t state = {0};
    turbo_retriever_config_t retriever_config = {0};
    turbo_retriever_context_config_t context_config = {0};
    turbo_retriever_t *retriever;
    turbo_graph_t *graph = turbo_graph_create("retriever-engineering-loop");
    turbo_agent_t *planner = retriever_test_agent_create("planner-test");
    turbo_agent_t *executor = retriever_test_agent_create("executor-test");

    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    retriever_config.query = retriever_test_query;
    retriever_config.user_data = &state;
    retriever = turbo_retriever_create(&retriever_config);
    check_not_null(retriever);
    context_config.retriever = retriever;
    context_config.kind = "note";
    context_config.limit = 3;

    check_int_eq(turbo_agent_install_retriever_engineering_loop(
                     graph, &context_config, planner, executor, "retriever",
                     "planner", "plan_commit", "plan_step", "review",
                     "executor", "tools", "detect_failure", "replan_route",
                     "replan_prepare", "plan_advance", "end", 1),
                 TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "retriever");
    check_size_eq(turbo_graph_node_count(graph), 12);
    check_size_eq(turbo_graph_edge_count(graph), 15);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_retriever_destroy(retriever);
    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
  }

  it("should adapt the SQLite knowledge store as a retriever") {
    char *dir = retriever_test_temp_dir();
    char *db_path = retriever_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t document = {0};
    turbo_retriever_query_options_t options = {0};
    turbo_retriever_t *retriever;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    document.id = "retriever-doc";
    document.uri = "memory://retriever-doc";
    document.kind = "note";
    document.title = "Retriever note";
    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document,
                     "retriever adapter returns local planner context", 128),
                 0);

    retriever = turbo_retriever_from_knowledge_store(store);
    check_not_null(retriever);
    options.kind = "note";
    options.uri_prefix = "memory://";
    options.limit = 2;
    check_int_eq(turbo_retriever_query(
                     retriever, "please retrieve local planner context", &options,
                     &results),
                 0);
    first = turbo_json_array_get(results, 0);
    check_not_null(first);
    check_str_eq(turbo_json_get_string(first, "document_id"), "retriever-doc");
    check_not_null(strstr(turbo_json_get_string(first, "text"), "planner context"));

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }
}
