#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include "turbo_runtime_store_sqlite.h"
#include <salts/clock.h>
#include <fmt.h>
#include <tstr.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "group fixture line %d: %s\n", __LINE__, #x); return 2; } } while (0)
typedef struct fixture_s fixture_t;
typedef struct worker_s { fixture_t *fixture; int index; } worker_t;
struct fixture_s {
  const char *cut;
  int collect, inject, healthy;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *host, *oracle, *runtime[3];
  turbo_agent_t *authority, *agent[3];
  turbo_agent_router_t *router;
  cmeta_threadpool_t *pool;
  cmeta_mutex_t oracle_mutex;
  atomic_int entered;
  worker_t worker[3];
  json_value_t **input;
  turbo_agent_router_group_result_t *results;
  size_t *count;
};
static const char *worker_ids[] = {"worker-0", "worker-1", "aggregator"};
static void crash_now(fixture_t *f, const char *boundary) {
  fprintf(stderr, "GROUP_CRASH kind=%s cut=%s boundary=%s\n", f->collect ? "collect" : "input", f->cut, boundary);
  fflush(stderr); _Exit(86);
}
static int counter_put(fixture_t *f, const char *id, int n) {
  json_value_t *record = json_create_object(); int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", n))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "counts", id, record);
  json_free(record); return rc;
}
static int counter_get(fixture_t *f, const char *id, int *n) {
  json_value_t *record = NULL; int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "counts", id, &record);
  int64_t value = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (value < 0 || value > 10)) rc = SALTS_EPROTO;
  if (!rc) *n = (int)value;
  json_free(record); return rc;
}
static int counter_add(fixture_t *f, const char *id, int amount) {
  int n, rc;
  cmeta_mutex_lock(&f->oracle_mutex);
  rc = counter_get(f, id, &n);
  if (!rc) rc = counter_put(f, id, n + amount);
  cmeta_mutex_unlock(&f->oracle_mutex);
  return rc;
}
static int host_put(void *u, const char *c, const char *id, const char *s) {
  fixture_t *f = u; return f->inner.put(f->inner.user_data, c, id, s);
}
static int host_get(void *u, const char *c, const char *id, char **s) {
  fixture_t *f = u; return f->inner.get(f->inner.user_data, c, id, s);
}
static int host_list(void *u, const char *c, const char *k, const char *v, char **s) {
  fixture_t *f = u; return f->inner.list(f->inner.user_data, c, k, v, s);
}
static int host_create(void *u, const char *c, const char *id, const char *s) {
  fixture_t *f = u; int rc;
  int selected = f->inject && !strcmp(c, TURBO_AGENT_ROUTER_GROUP_OBSERVATION_COLLECTION) && !strcmp(id, "subject");
  if (selected) {
    json_value_t *record = json_parse(s, strlen(s));
    int valid = record && json_array_size(json_object_get(record, "members")) == 2 &&
        f->input && !*f->input && f->count && !*f->count &&
        f->results && !f->results[0].summary && !f->results[0].state &&
        !f->results[1].summary && !f->results[1].state;
    json_free(record);
    if (!valid) return SALTS_EPROTO;
    fprintf(stderr, "GROUP_PRE_HANDOFF members=2 outputs_empty=1\n");
    if (!strcmp(f->cut, "before_write")) crash_now(f, "before_atomic_create");
  }
  rc = f->inner.create_if_absent(f->inner.user_data, c, id, s);
  if (!rc && selected && !strcmp(f->cut, "after_write")) crash_now(f, "after_atomic_create_before_handoff");
  return !rc && selected && !strcmp(f->cut, "lost_ack") ? SALTS_EIO : rc;
}
static void host_free(void *u) {
  fixture_t *f = u; f->inner.user_data_free(f->inner.user_data);
}
static int open_store(const char *directory, const char *name, int create,
    turbo_agent_runtime_t **runtime, turbo_agent_runtime_store_t *out) {
  turbo_runtime_sqlite_config_t config; turbo_agent_runtime_store_t store = {0};
  tstr path = tstr_format("{}/{}", directory, name); REQUIRE(path);
  turbo_runtime_sqlite_config_init(&config); config.path = path; config.create_if_missing = create;
  int rc = turbo_runtime_store_sqlite_open(&config, &store); tstr_free(path); REQUIRE(rc == SALTS_OK);
  if (out) *out = store;
  else { *runtime = turbo_agent_runtime_create(&store); REQUIRE(*runtime); }
  return 0;
}
static int open_fixture(fixture_t *f, const char *directory, int create) {
  static const char *names[] = {"worker-0.db", "worker-1.db", "aggregator.db"};
  turbo_agent_runtime_store_t wrapped = {host_put, host_get, host_list, f, host_free, host_create};
  cmeta_mutex_init(&f->oracle_mutex); atomic_init(&f->entered, 0);
  REQUIRE(!open_store(directory, "host.db", create, NULL, &f->inner));
  f->host = turbo_agent_runtime_create(&wrapped); REQUIRE(f->host);
  REQUIRE(!open_store(directory, "oracle.db", create, &f->oracle, NULL));
  for (int i = 0; i < 3; ++i) REQUIRE(!open_store(directory, names[i], create, &f->runtime[i], NULL));
  return 0;
}
static int transport(const char *request, char **out, void *u) {
  worker_t *worker = u; fixture_t *f = worker->fixture; int i = worker->index;
  json_value_t *body = json_parse(request, strlen(request)); *out = NULL;
  const char *model = body ? json_get_string(body, "model") : NULL;
  int valid = model && !strcmp(model, worker_ids[i]);
  json_free(body);
  if (!valid) return -77;
  if (i == 2) {
    if (!strstr(request, "worker_results") || !strstr(request, "worker-0") || !strstr(request, "worker-1") ||
        counter_add(f, f->healthy ? "healthy_aggregate" : "subject_aggregate", 1)) return -77;
  } else {
    tstr key = tstr_format("{}_model_{}", f->healthy ? "healthy" : "subject", i);
    int rc = key ? counter_add(f, key, 1) : SALTS_ENOMEM; tstr_free(key);
    if (rc || !strstr(request, f->healthy ? "healthy operation" : "subject operation")) return -77;
    atomic_fetch_add_explicit(&f->entered, 1, memory_order_acq_rel);
    uint64_t end = cmeta_monotonic_ms() + 5000;
    while (atomic_load_explicit(&f->entered, memory_order_acquire) < 2 && cmeta_monotonic_ms() < end) cmeta_thread_yield();
    if (atomic_load(&f->entered) != 2) return -77;
  }
  const char *response = "{\"id\":\"fixture\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"verified draft\"}]}]}";
  *out = malloc(strlen(response) + 1); if (!*out) return -77;
  memcpy(*out, response, strlen(response) + 1); return 0;
}
static int setup(fixture_t *f, int allowed) {
  turbo_agent_config_t config = {0}; turbo_agent_router_endpoint_t endpoints[3] = {0};
  turbo_agent_router_config_t router_config; turbo_agent_policy_t policy = turbo_agent_policy_default();
  cmeta_threadpool_config_t pool_config = {.num_threads = 2, .queue_capacity = 3};
  config.model = "host"; config.provider = turbo_model_provider_openai_responses();
  f->worker[2] = (worker_t){f, 2}; config.transport_fn = transport; config.transport_user_data = &f->worker[2];
  f->authority = turbo_agent_create(&config); REQUIRE(f->authority);
  policy.allow_delegate = allowed; REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy));
  for (int i = 0; i < 3; ++i) {
    f->worker[i] = (worker_t){f, i};
    config.model = worker_ids[i]; config.transport_fn = transport; config.transport_user_data = &f->worker[i];
    f->agent[i] = turbo_agent_create(&config); REQUIRE(f->agent[i]);
    endpoints[i].id = worker_ids[i]; endpoints[i].agent = f->agent[i]; endpoints[i].runtime = f->runtime[i];
    endpoints[i].roles = i == 2 ? TURBO_AGENT_ROUTER_PLAN : TURBO_AGENT_ROUTER_EXECUTE;
    endpoints[i].capabilities = TURBO_AGENT_ROUTER_TEXT; endpoints[i].max_difficulty = 100;
    endpoints[i].max_input_tokens = endpoints[i].max_output_tokens = 10000;
    endpoints[i].input_price_micros = endpoints[i].output_price_micros = (i + 1) * 1000;
  }
  turbo_agent_router_config_init(&router_config); router_config.authority_agent = f->authority;
  router_config.endpoints = endpoints; router_config.endpoint_count = 3;
  REQUIRE(turbo_agent_router_create(&router_config, &f->router) == SALTS_OK);
  f->pool = cmeta_threadpool_create_with_config(&pool_config); REQUIRE(f->pool); return 0;
}
static int submit(fixture_t *f, int i, const json_value_t *input, turbo_agent_router_task_t **task) {
  turbo_agent_router_request_t request; turbo_agent_runtime_parent_link_t parent = {0};
  turbo_agent_router_request_init(&request); request.role = i == 2 ? TURBO_AGENT_ROUTER_PLAN : TURBO_AGENT_ROUTER_EXECUTE;
  request.input_tokens = request.output_tokens = 1000; request.budget_micros = 20;
  request.timeout_ms = 15000; request.heartbeat_timeout_ms = 10000; request.thread_id = worker_ids[i];
  parent.parent_agent_run_id = "group-host"; request.parent_link = &parent;
  tstr id = tstr_format("{}-{}", f->healthy ? "healthy" : "subject", i); REQUIRE(id);
  int rc = turbo_agent_router_submit_with_intent(f->router, f->pool, f->host, id, &request, input, 0, task);
  tstr_free(id); return rc;
}
static int make_group(fixture_t *f, turbo_agent_router_group_t **group) {
  turbo_agent_router_task_t *tasks[2] = {0};
  json_value_t *input = turbo_agent_session_create_input_state_json_value(f->healthy ? "healthy operation" : "subject operation");
  REQUIRE(input);
  for (int i = 0; i < 2; ++i) REQUIRE(submit(f, i, input, &tasks[i]) == SALTS_OK);
  json_free(input);
  for (int i = 0; i < 2; ++i) REQUIRE(turbo_agent_router_task_wait(tasks[i], 5000) == SALTS_OK);
  if (!f->healthy) for (int i = 0; i < 2; ++i) {
    tstr id = tstr_format("source-{}", i); REQUIRE(id);
    int rc = turbo_agent_router_task_record_observation(tasks[i], f->oracle, id); tstr_free(id); REQUIRE(rc == SALTS_OK);
  }
  REQUIRE(turbo_agent_router_group_adopt(f->healthy ? "healthy-group" : "subject-group", tasks, 2, group) == SALTS_OK);
  return 0;
}
static int handoff(fixture_t *f, turbo_agent_router_group_t *group, const char *id,
    json_value_t **input, turbo_agent_router_group_result_t *results, size_t *count) {
  f->input = input; f->results = results; f->count = count;
  return f->collect ? turbo_agent_router_group_take_results_with_observation(group, f->host, id, results, 2, count)
      : turbo_agent_router_group_build_input_with_observation(group, "Synthesize exact worker evidence", f->host, id, input);
}
static int consume(fixture_t *f, json_value_t *input, turbo_agent_router_group_result_t *results, size_t count) {
  if (!f->collect) {
    turbo_agent_router_task_t *aggregate = NULL; json_value_t *summary = NULL, *state = NULL;
    REQUIRE(input && submit(f, 2, input, &aggregate) == SALTS_OK);
    REQUIRE(turbo_agent_router_task_wait(aggregate, 5000) == SALTS_OK);
    REQUIRE(turbo_agent_router_task_take_result(aggregate, &summary, &state) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(summary, "status"), "completed"));
    json_free(summary); json_free(state); REQUIRE(turbo_agent_router_task_release(aggregate) == SALTS_OK);
  } else {
    REQUIRE(count == 2);
    for (size_t i = 0; i < count; ++i) REQUIRE(results[i].state && results[i].summary &&
        !strcmp(json_get_string(results[i].summary, "status"), "completed"));
  }
  REQUIRE(!counter_add(f, f->healthy ? "healthy_delivery" : "subject_delivery", 1)); return 0;
}
static int crash_stage(fixture_t *f) {
  turbo_agent_router_group_t *group = NULL; json_value_t *input = NULL, *view = NULL;
  turbo_agent_router_group_result_t results[2] = {0}; size_t count = 0;
  REQUIRE(!setup(f, 1)); REQUIRE(!make_group(f, &group)); f->inject = 1;
  int rc = handoff(f, group, "subject", &input, results, &count);
  if (!strcmp(f->cut, "lost_ack")) {
    REQUIRE(rc == SALTS_EIO && !input && !count && !results[0].state && !results[1].summary);
    REQUIRE(turbo_agent_router_group_inspect(group, &view) == SALTS_OK);
    REQUIRE(!json_get_bool(view, "input_built", 1) && !json_get_bool(view, "results_taken", 1));
    crash_now(f, "after_lost_ack_return");
  }
  REQUIRE(rc == SALTS_OK);
  REQUIRE(!counter_add(f, "subject_handoff", f->collect ? 2 : 1));
  if (!strcmp(f->cut, "after_handoff")) crash_now(f, "after_handoff_before_delivery");
  REQUIRE(!strcmp(f->cut, "after_delivery")); REQUIRE(!consume(f, input, results, count));
  crash_now(f, "after_application_delivery"); return 2;
}
static int inspect(fixture_t *f, int healthy) {
  json_value_t *record = NULL;
  int after = !strcmp(f->cut, "after_handoff") || !strcmp(f->cut, "after_delivery");
  int delivered = !strcmp(f->cut, "after_delivery"), n;
  int rc = turbo_agent_router_load_group_observation(f->host, "subject", &record);
  REQUIRE(rc == (!strcmp(f->cut, "before_write") ? SALTS_ENOENT : SALTS_OK));
  if (!rc) {
    REQUIRE(!strcmp(json_get_string(record, "group_id"), "subject-group"));
    REQUIRE(!strcmp(json_get_string(record, "handoff_kind"), f->collect ? "result_collection" : "aggregation_input"));
    REQUIRE(json_array_size(json_object_get(record, "members")) == 2);
  }
  for (int i = 0; i < 2; ++i) {
    json_value_t *source = NULL, *run = NULL, *intent = NULL;
    tstr id = tstr_format("source-{}", i); REQUIRE(id);
    REQUIRE(turbo_agent_router_load_observation(f->oracle, id, &source) == SALTS_OK); tstr_free(id);
    id = tstr_format("subject-{}", i); REQUIRE(id);
    REQUIRE(turbo_agent_router_load_intent(f->host, id, &intent) == SALTS_OK); tstr_free(id);
    REQUIRE(!strcmp(json_get_string(intent, "task_id"), json_get_string(source, "task_id")));
    REQUIRE(turbo_agent_runtime_get_run(f->runtime[i], json_get_string(source, "runtime_run_id"), &run) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(run, "id"), json_get_string(source, "runtime_run_id")));
    REQUIRE(!strcmp(json_get_string(run, "thread_id"), worker_ids[i]));
    REQUIRE(!strcmp(json_get_string(run, "parent_agent_run_id"), "group-host"));
    if (record) {
      const json_value_t *member = json_array_get(json_object_get(record, "members"), i);
      REQUIRE(!strcmp(json_get_string(member, "task_id"), json_get_string(source, "task_id")));
      REQUIRE(!strcmp(json_get_string(member, "execution_id"), json_get_string(source, "execution_id")));
      REQUIRE(!strcmp(json_get_string(member, "runtime_run_id"), json_get_string(source, "runtime_run_id")));
      REQUIRE(!strcmp(json_get_string(member, "thread_id"), worker_ids[i]));
    }
    id = tstr_format("subject_model_{}", i); REQUIRE(id); REQUIRE(!counter_get(f, id, &n) && n == 1); tstr_free(id);
    id = tstr_format("healthy_model_{}", i); REQUIRE(id); REQUIRE(!counter_get(f, id, &n) && n == healthy); tstr_free(id);
    json_free(source); json_free(run); json_free(intent);
  }
  REQUIRE(!counter_get(f, "subject_handoff", &n) && n == after * (f->collect ? 2 : 1));
  REQUIRE(!counter_get(f, "subject_delivery", &n) && n == delivered);
  REQUIRE(!counter_get(f, "subject_aggregate", &n) && n == (!f->collect && delivered));
  REQUIRE(!counter_get(f, "healthy_handoff", &n) && n == healthy * (f->collect ? 2 : 1));
  REQUIRE(!counter_get(f, "healthy_delivery", &n) && n == healthy);
  REQUIRE(!counter_get(f, "healthy_aggregate", &n) && n == (!f->collect && healthy));
  if (healthy) {
    json_value_t *fresh = NULL;
    REQUIRE(turbo_agent_router_load_group_observation(f->host, "healthy", &fresh) == SALTS_OK);
    for (int i = 0; i < 2; ++i) {
      const json_value_t *member = json_array_get(json_object_get(fresh, "members"), i);
      json_value_t *new_run = NULL;
      REQUIRE(turbo_agent_runtime_get_run(f->runtime[i], json_get_string(member, "runtime_run_id"), &new_run) == SALTS_OK);
      REQUIRE(!strcmp(json_get_string(new_run, "thread_id"), worker_ids[i])); json_free(new_run);
      if (record) REQUIRE(strcmp(json_get_string(member, "runtime_run_id"),
          json_get_string(json_array_get(json_object_get(record, "members"), i), "runtime_run_id")));
    }
    json_free(fresh);
  }
  fprintf(stderr, "GROUP_INSPECT kind=%s cut=%s old_handoff=%d old_delivery=%d healthy_delivery=%d\n",
      f->collect ? "collect" : "input", f->cut, after * (f->collect ? 2 : 1), delivered, healthy);
  json_free(record); return 0;
}
static int recover(fixture_t *f) {
  turbo_agent_router_group_t *group = NULL; turbo_agent_router_task_t *denied = NULL;
  json_value_t *input = NULL, *old = NULL, *after = NULL;
  turbo_agent_router_group_result_t results[2] = {0}; size_t count = 0;
  int exists = strcmp(f->cut, "before_write") != 0; char *before = NULL, *after_text = NULL;
  REQUIRE(turbo_agent_router_load_group_observation(f->host, "subject", &old) == (exists ? SALTS_OK : SALTS_ENOENT));
  if (exists) { before = json_serialize(old, NULL); REQUIRE(before); }
  f->healthy = 1; REQUIRE(!setup(f, 0));
  json_value_t *goal = turbo_agent_session_create_input_state_json_value("healthy operation"); REQUIRE(goal);
  REQUIRE(submit(f, 0, goal, &denied) == SALTS_EPERM && !denied); json_free(goal);
  turbo_agent_policy_t policy = turbo_agent_policy_default(); policy.allow_delegate = 1;
  REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy)); REQUIRE(!make_group(f, &group));
  if (exists) {
    REQUIRE(handoff(f, group, "subject", &input, results, &count) == SALTS_EALREADY);
    REQUIRE(!input && !count && !results[0].summary && !results[1].state);
    REQUIRE(turbo_agent_router_load_group_observation(f->host, "subject", &after) == SALTS_OK);
    after_text = json_serialize(after, NULL); REQUIRE(after_text && !strcmp(before, after_text));
  }
  REQUIRE(handoff(f, group, "healthy", &input, results, &count) == SALTS_OK);
  REQUIRE(!counter_add(f, "healthy_handoff", f->collect ? 2 : 1)); REQUIRE(!consume(f, input, results, count));
  REQUIRE(turbo_agent_router_group_release(group) == SALTS_OK);
  for (int i = 0; i < 2; ++i) { json_free(results[i].summary); json_free(results[i].state); }
  json_free(input); json_free(old); json_free(after); json_serialize_free(before); json_serialize_free(after_text);
  fprintf(stderr, "GROUP_RECOVER old_handoff_retried=0 healthy_delivery=1\n"); return 0;
}
static void cleanup(fixture_t *f) {
  cmeta_threadpool_destroy(f->pool); turbo_agent_router_destroy(f->router);
  turbo_agent_destroy(f->authority);
  for (int i = 0; i < 3; ++i) { turbo_agent_destroy(f->agent[i]); turbo_agent_runtime_destroy(f->runtime[i]); }
  turbo_agent_runtime_destroy(f->host); turbo_agent_runtime_destroy(f->oracle); cmeta_mutex_destroy(&f->oracle_mutex);
}
int main(int argc, char **argv) {
  fixture_t f = {0}; int rc; REQUIRE(argc == 5);
  f.cut = argv[2]; f.collect = !strcmp(argv[3], "collect"); REQUIRE(f.collect || !strcmp(argv[3], "input"));
  REQUIRE(!open_fixture(&f, argv[4], !strcmp(argv[1], "init")));
  if (!strcmp(argv[1], "init")) {
    static const char *ids[] = {"subject_model_0", "subject_model_1", "subject_aggregate", "subject_handoff", "subject_delivery",
        "healthy_model_0", "healthy_model_1", "healthy_aggregate", "healthy_handoff", "healthy_delivery"};
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i) REQUIRE(!counter_put(&f, ids[i], 0));
    rc = 0;
  } else if (!strcmp(argv[1], "crash")) rc = crash_stage(&f);
  else if (!strcmp(argv[1], "inspect")) rc = inspect(&f, 0);
  else if (!strcmp(argv[1], "recover")) rc = recover(&f);
  else if (!strcmp(argv[1], "verify")) rc = inspect(&f, 1);
  else rc = 2;
  if (!rc) cleanup(&f);
  return rc;
}
