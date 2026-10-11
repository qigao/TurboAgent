#include "tinytest.h"
#include "turbo_agent_remote_session.h"
#include "turbo_agent_session.h"
#include <tstr.h>

typedef struct lifecycle_store_probe_s {
  int calls;
  int releases;
} lifecycle_store_probe_t;

static void lifecycle_release(void *data) {
  lifecycle_store_probe_t *probe = data;
  ++probe->releases;
}

static int lifecycle_put(void *data, const char *collection, const char *key, const char *value) {
  lifecycle_store_probe_t *probe = data;
  (void)collection;
  (void)key;
  (void)value;
  ++probe->calls;
  return -1;
}

static int lifecycle_get(void *data, const char *collection, const char *key, char **value) {
  lifecycle_store_probe_t *probe = data;
  (void)collection;
  (void)key;
  *value = NULL;
  ++probe->calls;
  return -1;
}

static int lifecycle_list(void *data, const char *collection, const char *key, const char *value,
                          char **records) {
  (void)key;
  (void)value;
  return lifecycle_get(data, collection, NULL, records);
}

static int lifecycle_transport(const char *request, char **response, void *data) {
  (void)request;
  (void)data;
  *response = NULL;
  return -1;
}

spec("Session construction ownership") {
  static turbo_agent_session_t *session;
  static turbo_agent_session_config_t config;
  static lifecycle_store_probe_t runtime_probe, memory_probe;
  static json_value_t *report;
  static tstr source;

  before_each() {
    runtime_probe = (lifecycle_store_probe_t){0};
    memory_probe = (lifecycle_store_probe_t){0};
    config = (turbo_agent_session_config_t){0};
    config.runtime_store = (turbo_agent_runtime_store_t){.put = lifecycle_put,
                                                         .get = lifecycle_get,
                                                         .list = lifecycle_list,
                                                         .user_data = &runtime_probe,
                                                         .user_data_free = lifecycle_release};
    config.memory_store = (turbo_agent_memory_store_t){.put = lifecycle_put,
                                                       .get = lifecycle_get,
                                                       .user_data = &memory_probe,
                                                       .user_data_free = lifecycle_release};
  }

  after_each() {
    turbo_agent_session_destroy(session);
    session = NULL;
    json_free(report);
    report = NULL;
    tstr_free(source);
    source = NULL;
  }

  it("owns the selected stores once and keeps them idle during construction") {
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_null(turbo_agent_session_agent(session));
    check_equal(runtime_probe.calls, 0);
    check_equal(memory_probe.calls, 0);
    check_equal(runtime_probe.releases, 0);
    check_equal(memory_probe.releases, 0);
    check_equal(turbo_agent_runtime_store_get_json(turbo_agent_session_runtime(session), "items",
                                                   "missing", &report),
                -1);
    check_equal(runtime_probe.calls, 1);
    check_equal(
        turbo_agent_memory_put(turbo_agent_session_memory_store(session), "project", "key", "{}"),
        -1);
    check_equal(memory_probe.calls, 1);
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(runtime_probe.releases, 1);
    check_equal(memory_probe.releases, 1);
  }

  it("consumes the selected RuntimeStore but retains caller MemoryStore on Agent rejection") {
    config.agent_config.model = "model-without-transport-or-credentials";
    session = turbo_agent_session_create(&config);
    check_null(session);
    check_equal(runtime_probe.releases, 1);
    check_equal(memory_probe.releases, 0);
    check_equal(runtime_probe.calls, 0);
    check_equal(turbo_agent_memory_put(&config.memory_store, "project", "key", "{}"), -1);
    check_equal(memory_probe.calls, 1);
    turbo_agent_memory_store_destroy(&config.memory_store);
    check_equal(memory_probe.releases, 1);
  }

  it("preserves the default Runtime fallback without consuming an incomplete caller store") {
    config.runtime_store.list = NULL;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    report = json_create_object();
    check_not_null(report);
    check_equal(turbo_runtime_json_object_set_string(report, "value", "retained"), 0);
    check_equal(turbo_agent_runtime_store_put_json(turbo_agent_session_runtime(session), "items",
                                                   "id", report),
                0);
    json_free(report);
    report = NULL;
    check_equal(turbo_agent_runtime_store_get_json(turbo_agent_session_runtime(session), "items",
                                                   "id", &report),
                0);
    check_equal(json_get_string(report, "value"), "retained");
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(runtime_probe.calls, 0);
    check_equal(runtime_probe.releases, 0);
    check_equal(memory_probe.releases, 1);
    config.runtime_store.user_data_free(config.runtime_store.user_data);
    check_equal(runtime_probe.releases, 1);
  }

  it("copies aliased caller strings and preserves empty optional configuration") {
    source = tstr_dup("caller-owned-setting");
    check_not_null(source);
    config.thread_id = source;
    config.memory_namespace = source;
    config.agent_config.model = source;
    config.agent_config.base_url = source;
    config.agent_config.transport_fn = lifecycle_transport;
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    source[0] = 'X';
    tstr_free(source);
    source = NULL;
    check_equal(turbo_agent_session_thread_id(session), "caller-owned-setting");
    check_equal(turbo_agent_session_memory_namespace(session), "caller-owned-setting");
    check_equal(turbo_agent_session_get_capabilities(session, &report), 0);
    check_equal(json_get_string(report, "model"), "caller-owned-setting");
    check_equal(json_get_string(report, "base_url"), "caller-owned-setting");
    turbo_agent_session_destroy(session);
    session = NULL;
    check_equal(runtime_probe.releases, 1);
    check_equal(memory_probe.releases, 1);

    config = (turbo_agent_session_config_t){.thread_id = "", .memory_namespace = ""};
    session = turbo_agent_session_create(&config);
    check_not_null(session);
    check_not_null(turbo_agent_session_thread_id(session));
    check_equal(turbo_agent_session_thread_id(session), "");
    check_not_null(turbo_agent_session_memory_namespace(session));
    check_equal(turbo_agent_session_memory_namespace(session), "");
    check_null(turbo_agent_session_last_run_id(session));
    check_null(turbo_agent_session_last_checkpoint_id(session));
  }
}

spec("Remote Session construction ownership") {
  static turbo_agent_remote_session_t *session;
  static turbo_agent_runtime_remote_client_t *client;
  static tstr source;

  after_each() {
    turbo_agent_remote_session_destroy(session);
    session = NULL;
    turbo_agent_runtime_remote_client_destroy(client);
    client = NULL;
    tstr_free(source);
    source = NULL;
  }

  it("copies the thread id and leaves a supplied client available to its owner") {
    turbo_agent_runtime_remote_client_config_t client_config = {.url = "http://127.0.0.1:1/rpc"};
    turbo_agent_remote_session_config_t config = {0};
    client = turbo_agent_runtime_remote_client_create(&client_config);
    check_not_null(client);
    source = tstr_dup("caller-thread");
    check_not_null(source);
    config.client = client;
    config.thread_id = source;
    session = turbo_agent_remote_session_create(&config);
    check_not_null(session);
    check_true(turbo_agent_remote_session_client(session) == client);
    source[0] = 'X';
    tstr_free(source);
    source = NULL;
    check_equal(turbo_agent_remote_session_thread_id(session), "caller-thread");
    turbo_agent_remote_session_destroy(session);
    session = NULL;

    config.thread_id = "";
    session = turbo_agent_remote_session_create(&config);
    check_not_null(session);
    check_true(turbo_agent_remote_session_client(session) == client);
    check_not_null(turbo_agent_remote_session_thread_id(session));
    check_equal(turbo_agent_remote_session_thread_id(session), "");
  }

  it("owns a configured client and preserves an omitted thread as an empty string") {
    turbo_agent_remote_session_config_t config = {
        .client_config = {.url = "http://127.0.0.1:1/rpc"}};
    session = turbo_agent_remote_session_create(&config);
    check_not_null(session);
    check_not_null(turbo_agent_remote_session_client(session));
    check_not_null(turbo_agent_remote_session_thread_id(session));
    check_equal(turbo_agent_remote_session_thread_id(session), "");
    check_null(turbo_agent_remote_session_last_run_id(session));
    check_null(turbo_agent_remote_session_last_checkpoint_id(session));
  }
}
