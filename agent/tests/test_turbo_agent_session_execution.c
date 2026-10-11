#include "tinytest.h"
#include "turbo_agent_session_execution.h"
#include "turbo_agent_inbox.h"
#include "turbo_agent_state.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct session_submit_gate_s {
  atomic_int entered;
  atomic_int open;
} session_submit_gate_t;

typedef struct session_submit_observer_s {
  session_submit_gate_t gate;
  atomic_int calls;
  int query_rc;
  int result_rc;
  turbo_agent_execution_status_t status;
} session_submit_observer_t;

static void session_submit_gate_init(session_submit_gate_t *gate, int open) {
  atomic_init(&gate->entered, 0);
  atomic_init(&gate->open, open);
}

static void session_submit_block(void *user_data) {
  session_submit_gate_t *gate = user_data;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire)) cmeta_thread_yield();
}

static int session_submit_node(turbo_graph_exec_ctx_t *ctx, void *user_data) {
  (void)ctx;
  if (user_data) session_submit_block(user_data);
  return 0;
}

static void session_submit_terminal(void *user_data, turbo_agent_execution_t *execution) {
  session_submit_observer_t *observer = user_data;
  observer->query_rc = turbo_agent_execution_get_status(execution, &observer->status);
  if (observer->query_rc == SALTS_OK)
    observer->query_rc = turbo_agent_execution_result_code(execution, &observer->result_rc);
  atomic_fetch_add_explicit(&observer->calls, 1, memory_order_relaxed);
  session_submit_block(&observer->gate);
}

static void session_submit_observer_init(session_submit_observer_t *observer, int open) {
  memset(observer, 0, sizeof(*observer));
  session_submit_gate_init(&observer->gate, open);
  atomic_init(&observer->calls, 0);
}

static int session_submit_transport(const char *request, char **out_response, void *user_data) {
  static const char response[] =
      "{\"id\":\"response-submit\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"ok\"}]}]}";
  (void)request;
  (void)user_data;
  *out_response = malloc(sizeof(response));
  if (!*out_response) return -1;
  memcpy(*out_response, response, sizeof(response));
  return 0;
}

static turbo_agent_session_t *session_submit_create(turbo_agent_runtime_store_t store) {
  turbo_agent_session_config_t config = {0};
  config.thread_id = "session-submit";
  config.runtime_store = store;
  config.agent_config.model = "test-model";
  config.agent_config.transport_fn = session_submit_transport;
  return turbo_agent_session_create(&config);
}

static turbo_graph_t *session_submit_graph(session_submit_gate_t *gate) {
  turbo_graph_t *graph = turbo_graph_create("session-submit");
  check_not_null(graph);
  check_equal(turbo_graph_add_json_value_node(graph, "start", session_submit_node, gate), 0);
  check_equal(turbo_graph_set_entry(graph, "start"), 0);
  return graph;
}

typedef struct session_submit_store_s {
  turbo_agent_runtime_store_t base;
  int rejected_commits;
} session_submit_store_t;

static int session_submit_store_put(void *user_data, const char *collection, const char *id,
                                      const char *record) {
  session_submit_store_t *store = user_data;
  if (strcmp(collection, "agent_inbox_transitions") == 0 &&
      strstr(record, "\"status\":\"applied\"")) {
    ++store->rejected_commits;
    return -1;
  }
  return store->base.put(store->base.user_data, collection, id, record);
}

static int session_submit_store_get(void *user_data, const char *collection, const char *id,
                                      char **record) {
  session_submit_store_t *store = user_data;
  return store->base.get(store->base.user_data, collection, id, record);
}

static int session_submit_store_list(void *user_data, const char *collection, const char *key,
                                       const char *value, char **records) {
  session_submit_store_t *store = user_data;
  return store->base.list(store->base.user_data, collection, key, value, records);
}

spec("Session asynchronous submission boundary") {
  it("copies input and publishes Session results before terminal notification finishes") {
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_session_t *session = session_submit_create(turbo_agent_runtime_store_memory_create());
    session_submit_gate_t node_gate;
    session_submit_observer_t observer;
    turbo_agent_session_submit_options_t options;
    turbo_agent_execution_t *execution = NULL;
    turbo_graph_t *graph;
    json_value_t *input = turbo_agent_state_create_json_value();
    json_value_t *summary = NULL;
    json_value_t *state = NULL;
    check_not_null(pool);
    check_not_null(session);
    session_submit_gate_init(&node_gate, 0);
    session_submit_observer_init(&observer, 0);
    graph = session_submit_graph(&node_gate);
    json_object_set_string(input, "marker", "original");
    turbo_agent_session_submit_options_init(&options);
    options.terminal = session_submit_terminal;
    options.terminal_user_data = &observer;
    check_equal(turbo_agent_session_submit(pool, session, graph, input, &options, &execution), SALTS_OK);
    while (!atomic_load_explicit(&node_gate.entered, memory_order_acquire)) cmeta_thread_yield();
    json_object_set_string(input, "marker", "changed");
    json_free(input);
    atomic_store_explicit(&node_gate.open, 1, memory_order_release);
    while (!atomic_load_explicit(&observer.gate.entered, memory_order_acquire)) cmeta_thread_yield();
    check_equal(turbo_agent_execution_wait(execution, 0), SALTS_OK);
    check_equal(observer.query_rc, SALTS_OK);
    check_equal(observer.result_rc, SALTS_OK);
    check_equal(observer.status, TURBO_AGENT_EXECUTION_COMPLETED);
    check_equal(turbo_agent_execution_take_result(execution, &summary, &state), SALTS_OK);
    check_equal(json_get_string(state, "marker"), "original");
    check_equal(turbo_agent_session_last_run_id(session), json_get_string(summary, "run_id"));
    turbo_agent_execution_release(execution);
    atomic_store_explicit(&observer.gate.open, 1, memory_order_release);
    cmeta_threadpool_wait(pool);
    check_equal(atomic_load_explicit(&observer.calls, memory_order_relaxed), 1);
    json_free(summary);
    json_free(state);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
    cmeta_threadpool_destroy(pool);
  }

  it("rejects invalid options, full queues and stopped pools without notifying") {
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    turbo_agent_session_t *session = session_submit_create(turbo_agent_runtime_store_memory_create());
    turbo_graph_t *graph = session_submit_graph(NULL);
    session_submit_gate_t blocker;
    session_submit_observer_t observer;
    turbo_agent_session_submit_options_t options;
    turbo_agent_execution_t *execution = NULL;
    json_value_t *input = turbo_agent_state_create_json_value();
    check_not_null(pool);
    check_not_null(session);
    session_submit_gate_init(&blocker, 0);
    session_submit_observer_init(&observer, 1);
    turbo_agent_session_submit_options_init(&options);
    options.terminal = session_submit_terminal;
    options.terminal_user_data = &observer;
    options.abi_version = 0;
    check_equal(turbo_agent_session_submit(pool, session, graph, input, &options, &execution), SALTS_EINVAL);
    check_null(execution);
    options.abi_version = TURBO_AGENT_SESSION_SUBMIT_ABI_VERSION;
    check_equal(cmeta_threadpool_try_submit(pool, session_submit_block, &blocker), SALTS_OK);
    while (!atomic_load_explicit(&blocker.entered, memory_order_acquire)) cmeta_thread_yield();
    check_equal(cmeta_threadpool_try_submit(pool, session_submit_block, &blocker), SALTS_OK);
    check_equal(turbo_agent_session_submit(pool, session, graph, input, &options, &execution), SALTS_ENOBUFS);
    check_null(execution);
    atomic_store_explicit(&blocker.open, 1, memory_order_release);
    cmeta_threadpool_wait(pool);
    cmeta_threadpool_shutdown(pool);
    check_equal(turbo_agent_session_submit(pool, session, graph, input, &options, &execution), SALTS_ESHUTDOWN);
    check_null(execution);
    check_equal(atomic_load_explicit(&observer.calls, memory_order_relaxed), 0);
    json_free(input);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
    cmeta_threadpool_destroy(pool);
  }

  it("publishes inbox completion failure instead of reporting a successful run") {
    cmeta_threadpool_config_t pool_config = {.num_threads = 1, .queue_capacity = 1};
    cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_config);
    session_submit_store_t failing = {0};
    turbo_agent_runtime_store_t store = {0};
    turbo_agent_inbox_config_t inbox_config = {
        sizeof(inbox_config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    turbo_agent_session_t *session;
    turbo_graph_t *graph;
    session_submit_observer_t observer;
    turbo_agent_session_submit_options_t options;
    turbo_agent_execution_t *execution = NULL;
    json_value_t *input = turbo_agent_session_create_input_state_json_value("initial");
    json_value_t *message = json_create_object();
    json_value_t *status = NULL;
    char *inbox_id = NULL;
    failing.base = turbo_agent_runtime_store_memory_create();
    store.put = session_submit_store_put;
    store.get = session_submit_store_get;
    store.list = session_submit_store_list;
    store.user_data = &failing;
    session = session_submit_create(store);
    check_not_null(session);
    check_equal(turbo_agent_session_inbox_configure(session, &inbox_config), SALTS_OK);
    json_object_set_string(message, "role", "user");
    json_object_set_string(message, "content", "durable steering");
    check_equal(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id), SALTS_OK);
    graph = turbo_agent_session_create_preset_graph(session, TURBO_AGENT_SESSION_WORKFLOW_LOOP);
    check_not_null(graph);
    session_submit_observer_init(&observer, 1);
    turbo_agent_session_submit_options_init(&options);
    options.terminal = session_submit_terminal;
    options.terminal_user_data = &observer;
    check_equal(turbo_agent_session_submit(pool, session, graph, input, &options, &execution), SALTS_OK);
    check_equal(turbo_agent_execution_wait(execution, UINT64_MAX), SALTS_OK);
    cmeta_threadpool_wait(pool);
    check_equal(atomic_load_explicit(&observer.calls, memory_order_relaxed), 1);
    check_equal(observer.query_rc, SALTS_OK);
    check_equal(observer.status, TURBO_AGENT_EXECUTION_FAILED);
    check_equal(observer.result_rc, SALTS_EIO);
    check_equal(failing.rejected_commits, 1);
    check_equal(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_equal(json_get_string(status, "status"), "claimed");
    json_free(status);
    json_free(message);
    json_free(input);
    free(inbox_id);
    turbo_agent_execution_release(execution);
    turbo_graph_destroy(graph);
    turbo_agent_session_destroy(session);
    cmeta_threadpool_destroy(pool);
    failing.base.user_data_free(failing.base.user_data);
  }
}

spec("Session synchronous completion boundary") {
  static turbo_agent_session_t *session;
  static turbo_graph_t *graph;
  static json_value_t *input, *state, *summary, *record, *checkpoint, *message, *status;
  static char *inbox_id, *deferred_id;
  static session_submit_store_t failing;

  after_each() {
    json_free(input); json_free(state); json_free(summary); json_free(record);
    json_free(checkpoint); json_free(message); json_free(status);
    input = state = summary = record = checkpoint = message = status = NULL;
    free(inbox_id); free(deferred_id);
    inbox_id = deferred_id = NULL;
    turbo_graph_destroy(graph); graph = NULL;
    turbo_agent_session_destroy(session); session = NULL;
    if (failing.base.user_data_free) failing.base.user_data_free(failing.base.user_data);
    memset(&failing, 0, sizeof(failing));
  }

  it("updates run and checkpoint facts with omitted summaries across start resume and fork") {
    static const char *interrupt_before[] = {"start"};
    turbo_graph_run_options_t graph_options = {0};
    turbo_agent_session_exec_options_t exec_options = {0};
    session = session_submit_create(turbo_agent_runtime_store_memory_create());
    graph = session_submit_graph(NULL);
    input = turbo_agent_state_create_json_value();
    check_not_null(session);
    check_not_null(input);
    check_equal(turbo_runtime_json_object_set_string(input, "marker", "retained"), TURBO_RUNTIME_JSON_OK);
    graph_options.interrupt_before_nodes = interrupt_before;
    graph_options.interrupt_before_count = 1;
    check_equal(turbo_agent_session_start_graph(session, graph, input, &graph_options,
                                                  NULL, NULL, NULL, &state), 0);
    check_equal(turbo_agent_session_get_latest_run(session, &record), 0);
    check_equal(json_get_string(record, "status"), "interrupted");
    check_equal(json_get_string(record, "id"), turbo_agent_session_last_run_id(session));
    check_equal(turbo_agent_session_get_checkpoint(session, NULL, &checkpoint), 0);
    check_equal(json_get_string(checkpoint, "id"), turbo_agent_session_last_checkpoint_id(session));
    json_free(state); state = NULL;

    /* An explicit empty ID is not the synchronous API's NULL default. */
    exec_options.checkpoint_id = "";
    check_equal(turbo_agent_session_resume_graph(session, graph, NULL, NULL, &exec_options,
                                                   NULL, NULL, NULL, &state), -1);
    check_null(state);
    check_equal(turbo_agent_session_last_run_id(session), json_get_string(record, "id"));
    check_equal(turbo_agent_session_last_checkpoint_id(session), json_get_string(checkpoint, "id"));

    exec_options.checkpoint_id = NULL;
    check_equal(turbo_agent_session_resume_graph(session, graph, NULL, NULL, &exec_options,
                                                   NULL, NULL, NULL, &state), 0);
    check_equal(json_get_string(state, "marker"), "retained");
    check_equal(turbo_agent_session_last_run_id(session), json_get_string(record, "id"));
    check_equal(turbo_agent_session_last_checkpoint_id(session), json_get_string(checkpoint, "id"));
    json_free(state); state = NULL;

    exec_options.scope = TURBO_SESSION_SCOPE_THREAD;
    check_equal(turbo_agent_session_fork_graph(session, graph, NULL, NULL, &exec_options,
                                                 NULL, NULL, NULL, &state), 0);
    check_equal(json_get_string(state, "marker"), "retained");
    check_true(strcmp(turbo_agent_session_last_run_id(session), json_get_string(record, "id")) != 0);
    check_equal(turbo_agent_session_get_latest_run(session, &summary), 0);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(json_get_string(summary, "id"), turbo_agent_session_last_run_id(session));
  }

  it("propagates an inbox commit failure and leaves completed outputs with the caller") {
    turbo_agent_runtime_store_t store = {0};
    turbo_agent_inbox_config_t inbox_config = {
        sizeof(inbox_config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    failing.base = turbo_agent_runtime_store_memory_create();
    store.put = session_submit_store_put;
    store.get = session_submit_store_get;
    store.list = session_submit_store_list;
    store.user_data = &failing;
    session = session_submit_create(store);
    input = turbo_agent_session_create_input_state_json_value("initial");
    message = json_create_object();
    check_not_null(session);
    check_not_null(input);
    check_equal(turbo_agent_session_inbox_configure(session, &inbox_config), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(message, "role", "user"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(message, "content", "durable steering"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_STEER, message, 0, &inbox_id), SALTS_OK);
    check_equal(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_FOLLOW_UP, message, 0, &deferred_id), SALTS_OK);
    graph = turbo_agent_session_create_loop_graph(session);
    check_not_null(graph);
    check_equal(turbo_agent_session_start_graph(session, graph, input, NULL, NULL, NULL,
                                                  &summary, &state), SALTS_EIO);
    check_not_null(state);
    check_equal(json_get_string(summary, "status"), "completed");
    check_equal(turbo_agent_session_last_run_id(session), json_get_string(summary, "run_id"));
    check_equal(failing.rejected_commits, 1);
    check_equal(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_equal(json_get_string(status, "status"), "claimed");
    json_free(status); status = NULL;
    check_equal(turbo_agent_session_inbox_status(session, deferred_id, &status), SALTS_OK);
    check_equal(json_get_string(status, "status"), "queued");
    check_equal(turbo_agent_session_list_runs(session, &record), 0);
    check_equal(json_array_size(record), 1);
  }

  it("commits a bounded follow-up and publishes its state without a requested summary") {
    turbo_agent_inbox_config_t inbox_config = {
        sizeof(inbox_config), TURBO_AGENT_INBOX_ABI_VERSION, 4, 4096, 1024, 1};
    session = session_submit_create(turbo_agent_runtime_store_memory_create());
    message = json_create_object();
    check_not_null(session);
    check_equal(turbo_agent_session_inbox_configure(session, &inbox_config), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(message, "role", "user"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_runtime_json_object_set_string(message, "content", "next turn"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_FOLLOW_UP, message, 0, &inbox_id), SALTS_OK);
    check_equal(turbo_agent_session_enqueue(session, TURBO_AGENT_INBOX_FOLLOW_UP, message, 0, &deferred_id), SALTS_OK);
    check_equal(turbo_agent_session_start_text(session, "initial", NULL, NULL, &state), SALTS_OK);
    check_equal(json_array_size(json_object_get(state, "input")), 2);
    check_equal(json_get_string(json_array_get(json_object_get(state, "input"), 1), "content"), "next turn");
    check_equal(turbo_agent_session_inbox_status(session, inbox_id, &status), SALTS_OK);
    check_equal(json_get_string(status, "status"), "applied");
    json_free(status); status = NULL;
    check_equal(turbo_agent_session_inbox_status(session, deferred_id, &status), SALTS_OK);
    check_equal(json_get_string(status, "status"), "queued");
    check_equal(turbo_agent_session_list_runs(session, &record), 0);
    check_equal(json_array_size(record), 2);
    check_equal(turbo_agent_session_get_latest_run(session, &summary), 0);
    check_equal(json_get_string(summary, "id"), turbo_agent_session_last_run_id(session));
  }
}
