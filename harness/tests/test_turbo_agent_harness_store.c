#include "tinytest.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_job_verifier.h"

static int verify_source(const json_value_t *snapshot, void *user) {
  (void)snapshot; (void)user; return SALTS_OK;
}
static int verify_evidence(const json_value_t *snapshot, const json_value_t *reference,
    const json_value_t *result, turbo_agent_job_acceptance_decision_t decision,
    const char *evidence, void *user) {
  (void)snapshot; (void)reference; (void)result; (void)decision; (void)evidence; (void)user;
  return SALTS_OK;
}

/* Real memory storage behind a release probe, not an emulated Session. */
typedef struct harness_store_probe_s {
  turbo_agent_runtime_store_t inner;
  int releases;
} harness_store_probe_t;

static int store_put(void *data, const char *collection, const char *id, const char *record) {
  harness_store_probe_t *probe = data;
  return probe->inner.put(probe->inner.user_data, collection, id, record);
}

static int store_get(void *data, const char *collection, const char *id, char **record) {
  harness_store_probe_t *probe = data;
  return probe->inner.get(probe->inner.user_data, collection, id, record);
}

static int store_list(void *data, const char *collection, const char *key,
                      const char *value, char **records) {
  harness_store_probe_t *probe = data;
  return probe->inner.list(probe->inner.user_data, collection, key, value, records);
}

static void store_release(void *data) {
  harness_store_probe_t *probe = data;
  ++probe->releases;
  probe->inner.user_data_free(probe->inner.user_data);
}

spec("Harness explicit RuntimeStore ownership") {
  static cmeta_threadpool_t *pool;
  static turbo_agent_harness_t *harness;
  static harness_store_probe_t runtime, memory;
  static turbo_agent_session_config_t session;
  static turbo_agent_app_config_t app;
  static turbo_agent_harness_config_t config;

  before_each() {
    cmeta_threadpool_config_t options = {.num_threads = 1, .queue_capacity = 1};
    pool = cmeta_threadpool_create_with_config(&options);
    check_not_null(pool);
    runtime = (harness_store_probe_t){.inner = turbo_agent_runtime_store_memory_create()};
    memory = (harness_store_probe_t){.inner = turbo_agent_runtime_store_memory_create()};
    check_not_null(runtime.inner.user_data);
    check_not_null(memory.inner.user_data);
    session = (turbo_agent_session_config_t){0};
    session.runtime_store = (turbo_agent_runtime_store_t){
        .put = store_put, .get = store_get, .list = store_list,
        .user_data = &runtime, .user_data_free = store_release};
    session.memory_store = (turbo_agent_memory_store_t){
        .put = store_put, .get = store_get, .user_data = &memory,
        .user_data_free = store_release};
    session.thread_id = "explicit-harness-scope";
    app = (turbo_agent_app_config_t){.session_config = &session};
    turbo_agent_harness_config_init(&config);
    config.app_config = &app;
    config.executor = pool;
  }

  after_each() {
    turbo_agent_harness_release(harness);
    harness = NULL;
    /* Invalid admission deliberately leaves resources with this caller. */
    if (!runtime.releases) store_release(&runtime);
    if (!memory.releases) store_release(&memory);
    check_equal(runtime.releases, 1);
    check_equal(memory.releases, 1);
    cmeta_threadpool_destroy(pool);
  }

  it("rejects every incomplete callback set before transfer") {
    turbo_agent_runtime_store_t complete = session.runtime_store;
    int i;
    for (i = 0; i < 3; ++i) {
      session.runtime_store = complete;
      if (i == 0) session.runtime_store.put = NULL;
      if (i == 1) session.runtime_store.get = NULL;
      if (i == 2) session.runtime_store.list = NULL;
      harness = (turbo_agent_harness_t *)&config;
      check_equal(turbo_agent_harness_create_with_store(&config, &harness), SALTS_EINVAL);
      check_null(harness);
      check_equal(runtime.releases, 0);
      check_equal(memory.releases, 0);
    }
  }

  it("rejects invalid ABI and missing nested configuration without taking stores") {
    config.abi_version = 0;
    check_equal(turbo_agent_harness_create_with_store(&config, &harness), SALTS_EINVAL);
    config.abi_version = TURBO_AGENT_HARNESS_ABI_VERSION;
    app.session_config = NULL;
    check_equal(turbo_agent_harness_create_with_store(&config, &harness), SALTS_EINVAL);
    check_null(turbo_agent_harness_create(&config));
    check_equal(turbo_agent_harness_create_with_store(NULL, &harness), SALTS_EINVAL);
    check_equal(turbo_agent_harness_create_with_store(&config, NULL), SALTS_EINVAL);
    check_null(harness);
    check_equal(runtime.releases, 0);
    check_equal(memory.releases, 0);
  }

  it("owns both stores on success through final retained release") {
    turbo_agent_harness_t *retained;
    check_equal(turbo_agent_harness_create_with_store(&config, &harness), SALTS_OK);
    check_not_null(harness);
    check_equal(turbo_agent_app_thread_id(turbo_agent_harness_app(harness)),
                "explicit-harness-scope");
    retained = turbo_agent_harness_retain(harness);
    check_not_null(retained);
    turbo_agent_harness_release(retained);
    check_equal(runtime.releases, 0);
    check_equal(memory.releases, 0);
    turbo_agent_harness_release(harness);
    harness = NULL;
    check_equal(runtime.releases, 1);
    check_equal(memory.releases, 1);
  }

  it("retains caller MemoryStore when Agent construction rejects configuration") {
    session.agent_config.model = "model-without-transport-or-credentials";
    check_equal(turbo_agent_harness_create_with_store(&config, &harness), SALTS_EIO);
    check_null(harness);
    check_equal(runtime.releases, 1);
    check_equal(memory.releases, 0);
  }

  it("keeps the legacy constructor's memory fallback") {
    session.runtime_store.list = NULL;
    harness = turbo_agent_harness_create(&config);
    check_not_null(harness);
    turbo_agent_harness_release(harness);
    harness = NULL;
    check_equal(runtime.releases, 0);
    check_equal(memory.releases, 1);
  }

  it("rejects every incomplete verifier before transferring stores") {
    turbo_agent_job_verifier_t verifier;
    turbo_agent_job_verifier_init(&verifier);
    check_equal(turbo_agent_harness_create_verified_with_store(&config, NULL, &harness), SALTS_EINVAL);
    for (int i = 0; i < 4; ++i) {
      turbo_agent_job_verifier_init(&verifier);
      verifier.source = verify_source; verifier.evidence = verify_evidence;
      if (i == 0) verifier.source = NULL;
      if (i == 1) verifier.evidence = NULL;
      if (i == 2) verifier.struct_size = sizeof(verifier) - 1;
      if (i == 3) verifier.abi_version = 0;
      harness = (turbo_agent_harness_t *)&config;
      check_equal(turbo_agent_harness_create_verified_with_store(&config, &verifier, &harness), SALTS_EINVAL);
      check_null(harness);
      check_equal(runtime.releases, 0); check_equal(memory.releases, 0);
    }
  }

  it("retains explicit-store failure ownership with a complete verifier") {
    turbo_agent_job_verifier_t verifier;
    turbo_agent_job_verifier_init(&verifier);
    verifier.source = verify_source; verifier.evidence = verify_evidence;
    session.runtime_store.list = NULL;
    check_equal(turbo_agent_harness_create_verified_with_store(&config, &verifier, &harness), SALTS_EINVAL);
    check_null(harness); check_equal(runtime.releases, 0); check_equal(memory.releases, 0);
    session.runtime_store.list = store_list;
    session.agent_config.model = "model-without-transport-or-credentials";
    check_equal(turbo_agent_harness_create_verified_with_store(&config, &verifier, &harness), SALTS_EIO);
    check_null(harness); check_equal(runtime.releases, 1); check_equal(memory.releases, 0);
  }
}
