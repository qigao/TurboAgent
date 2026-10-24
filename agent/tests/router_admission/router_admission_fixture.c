#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include "turbo_runtime_store_sqlite.h"
#include "../../src/turbo_agent_runtime_internal.h"
#include <salts/clock.h>
#include <fmt.h>
#include <tstr.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FAILURE = 2, CRASH = 86 };
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "admission fixture line %d: %s\n", __LINE__, #x); return FAILURE; } } while (0)

typedef struct fixture_s {
  const char *cut;
  int inject, healthy, creates, gets, puts, lists;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *host, *worker, *oracle;
  turbo_agent_t *authority, *child;
  turbo_agent_router_t *router;
  cmeta_threadpool_t *pool;
  atomic_int queue_entered, queue_open;
} fixture_t;

static void crash_now(const fixture_t *f, const char *boundary) {
  fprintf(stderr, "ADMISSION_CRASH cut=%s boundary=%s\n", f->cut, boundary);
  fflush(stderr);
  _Exit(CRASH);
}

static int counter_put(fixture_t *f, const char *id, int n) {
  json_value_t *record = json_create_object();
  int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", n))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "counters", id, record);
  json_free(record);
  return rc;
}

static int counter_get(fixture_t *f, const char *id, int *n) {
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "counters", id, &record);
  int64_t value = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (value < 0 || value > 10)) rc = SALTS_EPROTO;
  if (!rc) *n = (int)value;
  json_free(record);
  return rc;
}

static int fixture_put(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user; ++f->puts;
  return f->inner.put(f->inner.user_data, collection, id, text);
}
static int fixture_get(void *user, const char *collection, const char *id, char **out) {
  fixture_t *f = user; ++f->gets;
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int fixture_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  fixture_t *f = user; ++f->lists;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static int fixture_create(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  int selected = f->inject && !strcmp(collection, TURBO_AGENT_ROUTER_ADMISSION_COLLECTION) && !strcmp(id, "subject");
  int rc;
  ++f->creates;
  if (f->pool && !strcmp(collection, TURBO_AGENT_ROUTER_ADMISSION_COLLECTION)) {
    cmeta_threadpool_stats_t stats = {0};
    int expected = f->inject && !strcmp(f->cut, "queue_rejected") ? 2 : 0;
    cmeta_threadpool_get_stats(f->pool, &stats);
    if (stats.submitted_tasks != expected) return SALTS_EPROTO;
    fprintf(stderr, "ADMISSION_PREQUEUE expected=%d submitted=%lld\n", expected, (long long)stats.submitted_tasks);
  }
  if (selected && !strcmp(f->cut, "before_write")) crash_now(f, "before_atomic_create");
  rc = f->inner.create_if_absent(f->inner.user_data, collection, id, text);
  if (!rc && selected && !strcmp(f->cut, "after_write")) crash_now(f, "after_atomic_create_before_enqueue");
  if (!rc && selected && !strcmp(f->cut, "lost_ack")) return SALTS_EIO;
  return rc;
}
static void fixture_free(void *user) {
  fixture_t *f = user;
  f->inner.user_data_free(f->inner.user_data);
  f->inner = (turbo_agent_runtime_store_t){0};
}

static int open_runtime(const char *directory, const char *name, int create,
    turbo_agent_runtime_t **out, turbo_agent_runtime_store_t *store_out) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store = {0};
  tstr path = tstr_format("{}/{}", directory, name);
  int rc;
  REQUIRE(path);
  turbo_runtime_sqlite_config_init(&config); config.path = path; config.create_if_missing = create;
  rc = turbo_runtime_store_sqlite_open(&config, &store); tstr_free(path);
  REQUIRE(rc == SALTS_OK);
  if (store_out) *store_out = store;
  else { *out = turbo_agent_runtime_create(&store); REQUIRE(*out); }
  return 0;
}

static int open_fixture(fixture_t *f, const char *directory, int create) {
  turbo_agent_runtime_store_t wrapped = {fixture_put, fixture_get, fixture_list, f, fixture_free, fixture_create};
  REQUIRE(!open_runtime(directory, "host.db", create, NULL, &f->inner));
  f->host = turbo_agent_runtime_create(&wrapped); REQUIRE(f->host);
  REQUIRE(!open_runtime(directory, "worker.db", create, &f->worker, NULL));
  REQUIRE(!open_runtime(directory, "oracle.db", create, &f->oracle, NULL));
  return 0;
}

static int transport(const char *request, char **out, void *user) {
  fixture_t *f = user;
  turbo_agent_execution_context_t context = {0};
  static const char response[] = "{\"id\":\"fixture\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"fixture draft\"}]}]}";
  int n;
  (void)request; *out = NULL;
  turbo_agent_execution_context_get(&context);
  if (!context.thread_id || strcmp(context.thread_id, f->healthy ? "healthy-thread" : "subject-thread") ||
      counter_get(f, f->healthy ? "healthy_model" : "subject_model", &n) ||
      counter_put(f, f->healthy ? "healthy_model" : "subject_model", n + 1)) return -77;
  if (f->inject && !strcmp(f->cut, "in_model")) crash_now(f, "model_entry_after_counter_commit");
  *out = malloc(sizeof(response));
  if (!*out) return -77;
  memcpy(*out, response, sizeof(response));
  return 0;
}

static int setup_router(fixture_t *f, int allowed) {
  turbo_agent_config_t ac = {0};
  turbo_agent_router_endpoint_t endpoint = {0};
  turbo_agent_router_config_t config;
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  cmeta_threadpool_config_t pc = {.num_threads = 1, .queue_capacity = 1};
  ac.model = "offline-model"; ac.provider = turbo_model_provider_openai_responses();
  ac.transport_fn = transport; ac.transport_user_data = f;
  f->authority = turbo_agent_create(&ac); f->child = turbo_agent_create(&ac);
  REQUIRE(f->authority && f->child);
  policy.allow_delegate = allowed; REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy));
  endpoint.id = "worker"; endpoint.agent = f->child; endpoint.runtime = f->worker;
  endpoint.roles = 3; endpoint.capabilities = TURBO_AGENT_ROUTER_TEXT; endpoint.max_difficulty = 100;
  endpoint.max_input_tokens = endpoint.max_output_tokens = 10000;
  endpoint.input_price_micros = endpoint.output_price_micros = 1000;
  turbo_agent_router_config_init(&config); config.authority_agent = f->authority;
  config.endpoints = &endpoint; config.endpoint_count = 1;
  REQUIRE(turbo_agent_router_create(&config, &f->router) == SALTS_OK);
  f->pool = cmeta_threadpool_create_with_config(&pc); REQUIRE(f->pool);
  return 0;
}

static int submit(fixture_t *f, const char *id, turbo_agent_router_task_t **out_task) {
  turbo_agent_router_request_t request;
  turbo_agent_runtime_parent_link_t parent = {0};
  json_value_t *input = turbo_agent_session_create_input_state_json_value("offline task");
  int rc;
  REQUIRE(input);
  turbo_agent_router_request_init(&request); request.role = TURBO_AGENT_ROUTER_EXECUTE;
  request.difficulty = 20; request.input_tokens = request.output_tokens = 1000;
  request.budget_micros = 10; request.timeout_ms = 15000; request.heartbeat_timeout_ms = 10000;
  request.thread_id = f->healthy ? "healthy-thread" : "subject-thread";
  parent.parent_agent_run_id = "host-run"; parent.parent_tool_call_id = "delegate-call";
  request.parent_link = &parent;
  rc = turbo_agent_router_submit_with_intent(f->router, f->pool, f->host, id, &request, input, 0, out_task);
  json_free(input);
  return rc;
}

static void queue_block(void *user) {
  fixture_t *f = user;
  atomic_store_explicit(&f->queue_entered, 1, memory_order_release);
  while (!atomic_load_explicit(&f->queue_open, memory_order_acquire)) cmeta_thread_yield();
}

static int crash_stage(fixture_t *f) {
  turbo_agent_router_task_t *task = NULL;
  int rc;
  REQUIRE(!setup_router(f, 1));
  f->inject = 1;
  if (!strcmp(f->cut, "queue_rejected")) {
    uint64_t end = cmeta_monotonic_ms() + 2000;
    REQUIRE(cmeta_threadpool_try_submit(f->pool, queue_block, f) == SALTS_OK);
    while (!atomic_load_explicit(&f->queue_entered, memory_order_acquire) && cmeta_monotonic_ms() < end)
      cmeta_thread_yield();
    REQUIRE(atomic_load(&f->queue_entered));
    REQUIRE(cmeta_threadpool_try_submit(f->pool, queue_block, f) == SALTS_OK);
  }
  rc = submit(f, "subject", &task);
  if (!strcmp(f->cut, "lost_ack") || !strcmp(f->cut, "queue_rejected")) {
    REQUIRE(rc == (!strcmp(f->cut, "lost_ack") ? SALTS_EIO : SALTS_ENOBUFS));
    REQUIRE(!task);
    crash_now(f, !strcmp(f->cut, "lost_ack") ? "after_lost_ack_return" : "after_queue_rejection");
  }
  REQUIRE(rc == SALTS_OK && task);
  REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  if (!strcmp(f->cut, "after_observation")) {
    REQUIRE(turbo_agent_router_task_record_observation(task, f->host, "terminal") == SALTS_OK);
    crash_now(f, "terminal_observation_before_delivery");
  }
  REQUIRE(!strcmp(f->cut, "before_delivery"));
  crash_now(f, "terminal_before_delivery");
  return FAILURE;
}

static int inspect(fixture_t *f, int healthy_done) {
  json_value_t *record = NULL, *observation = NULL, *run = NULL;
  int expected = !strcmp(f->cut, "in_model") || !strcmp(f->cut, "before_delivery") || !strcmp(f->cut, "after_observation");
  int n, rc = turbo_agent_router_load_intent(f->host, "subject", &record);
  REQUIRE(rc == (!strcmp(f->cut, "before_write") ? SALTS_ENOENT : SALTS_OK));
  if (!rc) {
    REQUIRE(!strcmp(json_get_string(record, "semantics"), "pre_dispatch_intent"));
    REQUIRE(!strcmp(json_get_string(record, "thread_id"), "subject-thread"));
    REQUIRE(!strcmp(json_get_string(record, "parent_run_id"), "host-run"));
    REQUIRE(json_get_string(record, "task_id") && strlen(json_get_string(record, "task_id")) == 36);
  }
  REQUIRE(!counter_get(f, "subject_model", &n) && n == expected);
  REQUIRE(!counter_get(f, "subject_delivery", &n) && n == 0);
  REQUIRE(!counter_get(f, "healthy_model", &n) && n == healthy_done);
  REQUIRE(!counter_get(f, "healthy_delivery", &n) && n == healthy_done);
  if (healthy_done) {
    json_value_t *healthy = NULL;
    REQUIRE(turbo_agent_router_load_intent(f->host, "healthy", &healthy) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(healthy, "thread_id"), "healthy-thread"));
    if (record) REQUIRE(strcmp(json_get_string(healthy, "task_id"), json_get_string(record, "task_id")));
    json_free(healthy);
  }
  if (!strcmp(f->cut, "after_observation")) {
    REQUIRE(turbo_agent_router_load_observation(f->host, "terminal", &observation) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(record, "task_id"), json_get_string(observation, "task_id")));
    REQUIRE(turbo_agent_runtime_get_run(f->worker, json_get_string(observation, "runtime_run_id"), &run) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(run, "thread_id"), "subject-thread"));
    REQUIRE(!strcmp(json_get_string(run, "parent_agent_run_id"), "host-run"));
  }
  fprintf(stderr, "ADMISSION_INSPECT cut=%s intent=%d subject_model=%d subject_delivery=0 healthy=%d\n",
          f->cut, rc == SALTS_OK, expected, healthy_done);
  json_free(record); json_free(observation); json_free(run);
  return 0;
}

static int reject_stage(fixture_t *f) {
  turbo_agent_router_task_t *task = NULL;
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  json_value_t *before = NULL, *after = NULL;
  char *text_before, *text_after;
  int gets;
  REQUIRE(turbo_agent_router_load_intent(f->host, "subject", &before) == SALTS_OK);
  REQUIRE(!setup_router(f, 0)); gets = f->gets;
  REQUIRE(submit(f, "subject", &task) == SALTS_EPERM && !task);
  REQUIRE(f->creates == 0 && f->gets == gets && f->puts == 0 && f->lists == 0);
  policy.allow_delegate = 1; REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy));
  REQUIRE(submit(f, "subject", &task) == SALTS_EALREADY && !task);
  REQUIRE(f->creates == 1 && f->gets == gets && f->puts == 0 && f->lists == 0);
  REQUIRE(turbo_agent_router_load_intent(f->host, "subject", &after) == SALTS_OK);
  text_before = json_serialize(before, NULL); text_after = json_serialize(after, NULL);
  REQUIRE(text_before && text_after && !strcmp(text_before, text_after));
  json_serialize_free(text_before); json_serialize_free(text_after);
  json_free(before); json_free(after);
  REQUIRE(!inspect(f, 0));
  fprintf(stderr, "ADMISSION_REJECT cut=%s duplicate=EALREADY unchanged=1\n", f->cut);
  return 0;
}

static int healthy_stage(fixture_t *f) {
  turbo_agent_router_task_t *task = NULL;
  json_value_t *summary = NULL, *state = NULL;
  f->healthy = 1;
  REQUIRE(!setup_router(f, 1));
  REQUIRE(submit(f, "healthy", &task) == SALTS_OK && task);
  REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  REQUIRE(turbo_agent_router_task_take_result(task, &summary, &state) == SALTS_OK);
  REQUIRE(summary && !strcmp(json_get_string(summary, "status"), "completed"));
  REQUIRE(!strcmp(json_get_string(json_object_get(state, "model_route"), "admission_id"), "healthy"));
  REQUIRE(!counter_put(f, "healthy_delivery", 1));
  REQUIRE(turbo_agent_router_task_release(task) == SALTS_OK);
  json_free(summary); json_free(state);
  return 0;
}

static void cleanup(fixture_t *f) {
  atomic_store(&f->queue_open, 1);
  cmeta_threadpool_destroy(f->pool);
  turbo_agent_router_destroy(f->router);
  turbo_agent_destroy(f->child); turbo_agent_destroy(f->authority);
  turbo_agent_runtime_destroy(f->host); turbo_agent_runtime_destroy(f->worker); turbo_agent_runtime_destroy(f->oracle);
}

int main(int argc, char **argv) {
  fixture_t f = {0};
  int rc;
  REQUIRE(argc == 4);
  f.cut = argv[2]; atomic_init(&f.queue_entered, 0); atomic_init(&f.queue_open, 0);
  REQUIRE(!open_fixture(&f, argv[3], !strcmp(argv[1], "init")));
  if (!strcmp(argv[1], "init")) {
    REQUIRE(!counter_put(&f, "subject_model", 0)); REQUIRE(!counter_put(&f, "subject_delivery", 0));
    REQUIRE(!counter_put(&f, "healthy_model", 0)); REQUIRE(!counter_put(&f, "healthy_delivery", 0));
    rc = 0;
  } else if (!strcmp(argv[1], "crash")) rc = crash_stage(&f);
  else if (!strcmp(argv[1], "inspect")) rc = inspect(&f, 0);
  else if (!strcmp(argv[1], "reject")) rc = reject_stage(&f);
  else if (!strcmp(argv[1], "healthy")) rc = healthy_stage(&f);
  else if (!strcmp(argv[1], "verify")) rc = inspect(&f, 1);
  else rc = FAILURE;
  if (!rc) cleanup(&f);
  return rc;
}
