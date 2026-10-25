#include "../../examples/single_agent_report.h"
#include "turbo_model_provider.h"
#include "turbo_runtime_store_sqlite.h"
#include <fmt.h>
#include <stdatomic.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "report fixture line %d: %s\n", __LINE__, #x); return 2; } } while (0)
typedef struct fixture_s fixture_t;
typedef struct model_s { fixture_t *fixture; int index; atomic_int calls; } model_t;
struct fixture_s {
  const char *cut, *scope;
  int inject;
  model_t model[4];
  turbo_agent_runtime_store_t base;
  turbo_agent_runtime_t *host, *oracle, *runtime[4];
  turbo_agent_t *agent[5];
  turbo_agent_router_t *router, *tool_router;
  turbo_tool_registry_t *tools;
  cmeta_threadpool_t *pool;
  document_render_catalog_t catalog;
  single_agent_report_config_t config;
  single_agent_report_diagnostics_t diagnostics;
  cmeta_mutex_t oracle_mutex;
  atomic_int entered;
  json_value_t **delivery;
};
static const char *names[] = {"writer", "reviewer", "planner", "executor"};
static const char *files[] = {"draft.db", "review.db", "synthesis.db", "delivery.db"};
static int stop_cut(const fixture_t *f) { return !strncmp(f->cut, "stop_", 5); }
static void crash_now(fixture_t *f, const char *boundary) {
  fprintf(stderr, "REPORT_CRASH cut=%s boundary=%s\n", f->cut, boundary); fflush(stderr); _Exit(86);
}
static int counter_put(fixture_t *f, const char *id, int n) {
  json_value_t *record = json_create_object(); int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", n))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "counts", id, record);
  json_free(record); return rc;
}
static int counter_get(fixture_t *f, const char *id, int *n) {
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "counts", id, &record);
  int64_t value = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (value < 0 || value > 10)) rc = SALTS_EPROTO;
  if (!rc) *n = (int)value;
  json_free(record); return rc;
}
static int counter_add(fixture_t *f, const char *kind) {
  tstr id = tstr_format("{}-{}", f->scope, kind); int n, rc;
  if (!id) return SALTS_ENOMEM;
  cmeta_mutex_lock(&f->oracle_mutex);
  rc = counter_get(f, id, &n);
  if (!rc) rc = counter_put(f, id, n + 1);
  cmeta_mutex_unlock(&f->oracle_mutex); tstr_free(id); return rc;
}
static int host_put(void *u, const char *c, const char *id, const char *text) {
  fixture_t *f = u; return f->base.put(f->base.user_data, c, id, text);
}
static int host_get(void *u, const char *c, const char *id, char **text) {
  fixture_t *f = u; return f->base.get(f->base.user_data, c, id, text);
}
static int host_list(void *u, const char *c, const char *k, const char *v, char **text) {
  fixture_t *f = u; return f->base.list(f->base.user_data, c, k, v, text);
}
static int host_create(void *u, const char *c, const char *id, const char *text) {
  fixture_t *f = u; int rc, manifest = !strcmp(c, SINGLE_AGENT_REPORT_MANIFESTS) && !strcmp(id, "subject");
  int fan_in = !strcmp(c, TURBO_AGENT_ROUTER_GROUP_OBSERVATION_COLLECTION) && !strcmp(id, "subject-fan-in");
  int review = !strcmp(c, TURBO_AGENT_ROUTER_REVIEW_INTENT_COLLECTION) && !strcmp(id, "subject-review");
  int ready = !strcmp(c, SINGLE_AGENT_REPORT_READY) && !strcmp(id, "subject-ready");
  int stopped_task = !strcmp(c, TURBO_AGENT_ROUTER_OBSERVATION_COLLECTION) && !strcmp(id, "subject-stop-aggregate");
  int stop = !strcmp(c, SINGLE_AGENT_REPORT_STOPS) && !strcmp(id, "subject-stop");
  if (f->inject && (manifest || fan_in || review || ready || stopped_task || stop)) {
    if (!f->delivery || *f->delivery) return SALTS_EPROTO;
    fprintf(stderr, "REPORT_PREPUBLISH delivery_null=1\n");
    if (manifest && !strcmp(f->cut, "before_manifest")) crash_now(f, "before_manifest_create");
    if (ready && !strcmp(f->cut, "before_ready")) crash_now(f, "before_ready_create");
    if (stopped_task && !strcmp(f->cut, "stop_before_observation")) crash_now(f, "after_drain_before_terminal_observation");
    if (stop && !strcmp(f->cut, "stop_before_record")) crash_now(f, "after_terminal_observation_before_stop_record");
  }
  rc = f->base.create_if_absent(f->base.user_data, c, id, text);
  if (!rc && f->inject) {
    if (manifest && !strcmp(f->cut, "after_manifest")) crash_now(f, "after_manifest_before_admission");
    if (fan_in && !strcmp(f->cut, "after_fan_in")) crash_now(f, "after_cohort_before_aggregation");
    if (review && !strcmp(f->cut, "after_review")) crash_now(f, "after_review_intent_before_enqueue");
    if (ready && !strcmp(f->cut, "after_ready")) crash_now(f, "after_ready_before_publication");
    if (stopped_task && !strcmp(f->cut, "stop_after_observation")) crash_now(f, "after_terminal_observation_before_stop_record");
    if (stop && !strcmp(f->cut, "stop_after_record")) crash_now(f, "after_stop_record_before_error_return");
    if (stop && !strcmp(f->cut, "stop_lost_ack")) return SALTS_EIO;
  }
  return rc;
}
static void host_free(void *u) {
  fixture_t *f = u; f->base.user_data_free(f->base.user_data);
}
static int open_store(const char *directory, const char *name, int create,
    turbo_agent_runtime_t **out, turbo_agent_runtime_store_t *base) {
  turbo_runtime_sqlite_config_t config; turbo_agent_runtime_store_t store = {0};
  tstr path = tstr_format("{}/{}", directory, name); REQUIRE(path);
  turbo_runtime_sqlite_config_init(&config); config.path = path; config.create_if_missing = create;
  int rc = turbo_runtime_store_sqlite_open(&config, &store); tstr_free(path); REQUIRE(rc == SALTS_OK);
  if (base) *base = store;
  else { *out = turbo_agent_runtime_create(&store); REQUIRE(*out); }
  return 0;
}
static int open_fixture(fixture_t *f, const char *directory, int create) {
  turbo_agent_runtime_store_t store = {host_put, host_get, host_list, f, host_free, host_create};
  cmeta_mutex_init(&f->oracle_mutex); atomic_init(&f->entered, 0); f->scope = "subject";
  REQUIRE(!open_store(directory, "host.db", create, NULL, &f->base));
  f->host = turbo_agent_runtime_create(&store); REQUIRE(f->host);
  REQUIRE(!open_store(directory, "oracle.db", create, &f->oracle, NULL));
  for (int i = 0; i < 4; ++i) REQUIRE(!open_store(directory, files[i], create, &f->runtime[i], NULL));
  return 0;
}
static int transport(const char *request, char **out, void *u) {
  model_t *model = u; fixture_t *f = model->fixture; int i = model->index;
  json_value_t *body = json_parse(request, strlen(request)); *out = NULL;
  int valid = body && json_get_string(body, "model") && !strcmp(json_get_string(body, "model"), names[i]);
  json_free(body); if (!valid) return -77;
  int call = atomic_fetch_add(&model->calls, 1);
  tstr kind = tstr_format("model-{}", i);
  int rc = kind ? counter_add(f, kind) : SALTS_ENOMEM; tstr_free(kind); if (rc) return -77;
  if (i == 2 && f->inject && stop_cut(f)) return -77;
  if (i < 2) {
    if (!strstr(request, f->config.goal)) return -77;
    atomic_fetch_add_explicit(&f->entered, 1, memory_order_acq_rel);
    uint64_t until = cmeta_monotonic_ms() + 5000;
    while (atomic_load_explicit(&f->entered, memory_order_acquire) < 2 && cmeta_monotonic_ms() < until) cmeta_thread_yield();
    if (atomic_load(&f->entered) != 2) return -77;
  }
  const char *text;
  if (i == 3 && !call) text = "{\"id\":\"exec\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"render-1\","
      "\"name\":\"document_render\",\"arguments\":\"{\\\"title\\\":\\\"Report\\\",\\\"passages\\\":["
      "{\\\"text\\\":\\\"Reviewed report\\\",\\\"source_id\\\":\\\"brief\\\"}]}\"}]}";
  else if (i == 1) text = "{\"id\":\"review\",\"choices\":[{\"message\":{\"role\":\"assistant\","
      "\"content\":\"Careful draft\"},\"finish_reason\":\"stop\"}]}";
  else text = "{\"id\":\"draft\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"Careful draft\"}]}]}";
  *out = malloc(strlen(text) + 1); if (!*out) return -77; memcpy(*out, text, strlen(text) + 1); return 0;
}
static int authority_transport(const char *request, char **out, void *u) {
  (void)request; (void)u; *out = NULL; return -77;
}
static turbo_tool_status_t render(const char *args, const turbo_tool_execution_context_t *context, char **out, void *u) {
  fixture_t *f = u;
  if (!turbo_agent_current_tool_approval_granted() || counter_add(f, "tool")) return TURBO_TOOL_ERROR;
  return single_agent_report_render(args, context, out, &f->catalog);
}
static int review(const json_value_t *summary, const json_value_t *state, void *u) {
  fixture_t *f = u;
  if (!summary || !turbo_agent_state_review_required(state) || counter_add(f, "review")) return -1;
  return 1;
}
static int setup(fixture_t *f) {
  static const document_render_source_t sources[] = {{"brief", "host-v1"}};
  turbo_agent_policy_t policy = turbo_agent_policy_default(); policy.allow_delegate = policy.allow_runtime_tools = 1;
  turbo_agent_guardrail_t guard = {0}; turbo_agent_router_config_t c;
  turbo_agent_router_endpoint_t endpoints[4] = {0};
  cmeta_threadpool_config_t pool_config = {.num_threads = 2, .queue_capacity = 2};
  f->catalog = (document_render_catalog_t){sources, 1};
  f->tools = turbo_tool_registry_create(); REQUIRE(f->tools);
  turbo_tool_definition_v6_t tool = single_agent_report_tool_definition(&f->catalog);
  tool.definition.json_value_handler = NULL; tool.definition.user_data = f; tool.context_handler = render;
  REQUIRE(turbo_tool_registry_add_v6(f->tools, &tool) == TURBO_TOOL_OK);
  for (int i = 0; i < 5; ++i) {
    turbo_agent_config_t config = {0}; config.model = i < 4 ? names[i] : "host";
    config.provider = i == 1 ? turbo_model_provider_openai_compatible_chat_completions() : turbo_model_provider_openai_responses();
    config.tool_registry = f->tools;
    if (i < 4) {
      f->model[i].fixture = f; f->model[i].index = i; atomic_init(&f->model[i].calls, 0);
      config.transport_fn = transport; config.transport_user_data = &f->model[i];
    } else { config.transport_fn = authority_transport; config.transport_user_data = f; }
    f->agent[i] = turbo_agent_create(&config); REQUIRE(f->agent[i]);
    REQUIRE(!turbo_agent_set_tool_policy(f->agent[i], &policy));
    if (i < 4) {
      endpoints[i].id = names[i]; endpoints[i].agent = f->agent[i]; endpoints[i].runtime = f->runtime[i];
      endpoints[i].roles = i == 2 ? TURBO_AGENT_ROUTER_PLAN : TURBO_AGENT_ROUTER_EXECUTE;
      endpoints[i].capabilities = TURBO_AGENT_ROUTER_TEXT | (i == 3 ? TURBO_AGENT_ROUTER_TOOL_PROPOSALS : 0);
      endpoints[i].max_difficulty = 100; endpoints[i].max_input_tokens = endpoints[i].max_output_tokens = 10000;
      endpoints[i].input_price_micros = endpoints[i].output_price_micros = 1000;
      guard = (turbo_agent_guardrail_t){0}; guard.after_model = single_agent_report_response_guard;
      REQUIRE(!turbo_agent_add_guardrail(f->agent[i], &guard));
    }
  }
  guard = (turbo_agent_guardrail_t){0}; guard.before_tool = single_agent_report_guard;
  REQUIRE(!turbo_agent_add_guardrail(f->agent[4], &guard));
  turbo_agent_router_config_init(&c); c.authority_agent = f->agent[4]; c.endpoints = endpoints; c.endpoint_count = 3;
  REQUIRE(turbo_agent_router_create(&c, &f->router) == SALTS_OK);
  c.endpoints = &endpoints[3]; c.endpoint_count = 1;
  REQUIRE(turbo_agent_router_create(&c, &f->tool_router) == SALTS_OK);
  f->pool = cmeta_threadpool_create_with_config(&pool_config); REQUIRE(f->pool);
  f->config.router = f->router; f->config.tool_router = f->tool_router; f->config.pool = f->pool;
  f->config.authority = f->agent[4]; f->config.tools = f->tools;
  f->config.parent_run_id = "report-host"; f->config.goal = "subject goal"; f->config.brief = "Host-approved brief";
  f->config.timeout_ms = 15000; f->config.review = review; f->config.review_user = f;
  f->config.audit_runtime = f->host; f->config.audit_id = "subject";
  f->config.diagnostics = &f->diagnostics;
  turbo_agent_router_request_init(&f->config.worker);
  f->config.worker.input_tokens = f->config.worker.output_tokens = 1000; f->config.worker.budget_micros = 10;
  f->config.aggregate = f->config.execute = f->config.worker; f->config.reserved_budget_micros = 40; return 0;
}
static int crash_stage(fixture_t *f) {
  json_value_t *delivery = NULL; const char *phase = NULL;
  REQUIRE(!setup(f)); f->inject = 1; f->delivery = &delivery;
  int rc = single_agent_report_run(&f->config, &delivery, &phase);
  if (stop_cut(f)) {
    int lost_ack = !strcmp(f->cut, "stop_lost_ack");
    REQUIRE(rc == SALTS_EIO && !delivery);
    REQUIRE(!strcmp(phase, lost_ack ? "stop-audit" : "aggregation"));
    REQUIRE(f->diagnostics.operation_code == SALTS_EIO && !strcmp(f->diagnostics.operation_phase, "aggregation"));
    REQUIRE(f->diagnostics.stop_audit_attempted && f->diagnostics.stop_audit_code == (lost_ack ? SALTS_EIO : 0));
    REQUIRE(!counter_add(f, "stop-return"));
    REQUIRE(lost_ack || !strcmp(f->cut, "stop_after_return"));
    crash_now(f, lost_ack ? "after_stop_lost_ack_return" : "after_stop_error_return");
  }
  REQUIRE(rc == SALTS_OK && delivery);
  REQUIRE(!counter_add(f, "handoff"));
  if (!strcmp(f->cut, "after_return")) crash_now(f, "after_return_before_delivery");
  REQUIRE(!strcmp(f->cut, "after_delivery")); REQUIRE(!counter_add(f, "delivery"));
  crash_now(f, "after_application_delivery"); return 2;
}
static int inspect(fixture_t *f, int healthy) {
  int no_work = !strcmp(f->cut, "before_manifest") || !strcmp(f->cut, "after_manifest");
  int drafts_only = !strcmp(f->cut, "after_fan_in"), paused = !strcmp(f->cut, "after_review");
  int stopped = stop_cut(f), terminal = !no_work && !drafts_only && !paused && !stopped;
  int returned = !strcmp(f->cut, "after_return") || !strcmp(f->cut, "after_delivery");
  int delivered = !strcmp(f->cut, "after_delivery"), n;
  json_value_t *manifest = NULL, *record = NULL;
  REQUIRE(turbo_agent_runtime_store_get_json_status(f->host, SINGLE_AGENT_REPORT_MANIFESTS, "subject", &manifest) ==
      (!strcmp(f->cut, "before_manifest") ? SALTS_ENOENT : SALTS_OK));
  for (int i = 0; i < 4; ++i) {
    tstr id = tstr_format("subject-model-{}", i); REQUIRE(id);
    int expected = no_work || (drafts_only && i >= 2) || (stopped && i == 3) ? 0 : i == 3 ? (paused ? 1 : 2) : 1;
    REQUIRE(!counter_get(f, id, &n) && n == expected); tstr_free(id);
    id = tstr_format("healthy-model-{}", i); REQUIRE(id);
    REQUIRE(!counter_get(f, id, &n) && n == healthy * (i == 3 ? 2 : 1)); tstr_free(id);
  }
  REQUIRE(!counter_get(f, "subject-review", &n) && n == (paused || terminal));
  REQUIRE(!counter_get(f, "subject-tool", &n) && n == terminal);
  REQUIRE(!counter_get(f, "subject-handoff", &n) && n == returned);
  REQUIRE(!counter_get(f, "subject-delivery", &n) && n == delivered);
  REQUIRE(!counter_get(f, "healthy-review", &n) && n == healthy);
  REQUIRE(!counter_get(f, "healthy-tool", &n) && n == healthy);
  REQUIRE(!counter_get(f, "healthy-handoff", &n) && n == healthy);
  REQUIRE(!counter_get(f, "healthy-delivery", &n) && n == healthy);
  int stop_returned = !strcmp(f->cut, "stop_after_return") || !strcmp(f->cut, "stop_lost_ack");
  REQUIRE(!counter_get(f, "subject-stop-return", &n) && n == stop_returned);
  REQUIRE(!counter_get(f, "healthy-stop-return", &n) && n == 0);
  if (!no_work) {
    REQUIRE(turbo_agent_router_load_group_observation(f->host, "subject-fan-in", &record) == SALTS_OK);
    for (int i = 0; i < 2; ++i) {
      const json_value_t *member = json_array_get(json_object_get(record, "members"), i);
      json_value_t *run = NULL; REQUIRE(turbo_agent_runtime_get_run(f->runtime[i], json_get_string(member, "runtime_run_id"), &run) == SALTS_OK);
      REQUIRE(!strcmp(json_get_string(run, "thread_id"), i ? "subject-worker-b" : "subject-worker-a")); json_free(run);
    }
    json_free(record); record = NULL;
  }
  if (paused || terminal) {
    json_value_t *pending = NULL, *intent = NULL;
    REQUIRE(turbo_agent_router_load_observation(f->host, "subject-pending", &pending) == SALTS_OK);
    REQUIRE(turbo_agent_router_load_review_intent(f->host, "subject-review", &intent) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(intent, "source_checkpoint_id"), json_get_string(pending, "runtime_checkpoint_id")));
    json_value_t *checkpoint = NULL;
    REQUIRE(turbo_agent_runtime_get_checkpoint(f->runtime[3], json_get_string(intent, "source_checkpoint_id"), &checkpoint) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(checkpoint, "run_id"), json_get_string(intent, "source_run_id")));
    json_free(checkpoint); json_free(pending); json_free(intent);
  }
  if (terminal) {
    REQUIRE(turbo_agent_router_load_observation(f->host, "subject-execute-final", &record) == SALTS_OK);
    json_value_t *run = NULL;
    REQUIRE(turbo_agent_runtime_get_run(f->runtime[3], json_get_string(record, "runtime_run_id"), &run) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(run, "thread_id"), "subject-execute")); json_free(run);
    json_value_t *ready = NULL;
    REQUIRE(turbo_agent_runtime_store_get_json_status(f->host, SINGLE_AGENT_REPORT_READY, "subject-ready", &ready) ==
        (!strcmp(f->cut, "before_ready") ? SALTS_ENOENT : SALTS_OK));
    if (ready) REQUIRE(!strcmp(json_get_string(ready, "execution_id"), json_get_string(record, "execution_id")));
    json_free(ready); json_free(record);
  }
  if (stopped) {
    json_value_t *intent = NULL, *observation = NULL, *stop = NULL, *run = NULL;
    int observed = strcmp(f->cut, "stop_before_observation") != 0;
    int recorded = !strcmp(f->cut, "stop_after_record") || stop_returned;
    REQUIRE(turbo_agent_router_load_intent(f->host, "subject-aggregate", &intent) == SALTS_OK);
    REQUIRE(turbo_agent_router_load_observation(f->host, "subject-stop-aggregate", &observation) == (observed ? SALTS_OK : SALTS_ENOENT));
    if (observation) {
      REQUIRE(!strcmp(json_get_string(observation, "task_id"), json_get_string(intent, "task_id")));
      REQUIRE(!strcmp(json_get_string(observation, "thread_id"), "subject-aggregate"));
      REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(observation, "execution_status"), -1) == TURBO_AGENT_EXECUTION_FAILED);
      const char *run_id = json_get_string(observation, "runtime_run_id");
      if (run_id) {
        REQUIRE(turbo_agent_runtime_get_run(f->runtime[2], run_id, &run) == SALTS_OK);
        REQUIRE(!strcmp(json_get_string(run, "thread_id"), "subject-aggregate"));
      }
    }
    REQUIRE(turbo_agent_runtime_store_get_json_status(f->host, SINGLE_AGENT_REPORT_STOPS, "subject-stop", &stop) == (recorded ? SALTS_OK : SALTS_ENOENT));
    if (stop) {
      REQUIRE(!strcmp(json_get_string(stop, "semantics"), "post_drain_report_stop_observation"));
      REQUIRE(!strcmp(json_get_string(stop, "operation_phase"), "aggregation"));
      REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(stop, "operation_code"), 0) == SALTS_EIO);
      const json_value_t *tasks = json_object_get(stop, "tasks"), *entry = json_array_get(tasks, 0);
      REQUIRE(json_array_size(tasks) == 1 && !strcmp(json_get_string(entry, "observation_id"), "subject-stop-aggregate"));
      REQUIRE(!strcmp(json_get_string(entry, "execution_id"), json_get_string(observation, "execution_id")));
      REQUIRE(!strcmp(json_get_string(entry, "observation_boundary"), "after_drain"));
      REQUIRE(json_array_size(json_object_get(stop, "acknowledged_gates")) == 5);
    }
    json_free(intent); json_free(observation); json_free(stop); json_free(run);
  }
  json_free(manifest);
  fprintf(stderr, "REPORT_INSPECT cut=%s old_handoff=%d old_delivery=%d healthy=%d\n", f->cut, returned, delivered, healthy);
  return 0;
}
static int recover(fixture_t *f) {
  json_value_t *delivery = NULL; const char *phase = NULL;
  REQUIRE(!setup(f));
  if (strcmp(f->cut, "before_manifest")) {
    REQUIRE(single_agent_report_run(&f->config, &delivery, &phase) == SALTS_EALREADY && !delivery);
  }
  turbo_agent_policy_t policy = turbo_agent_policy_default(); policy.allow_runtime_tools = 1;
  REQUIRE(!turbo_agent_set_tool_policy(f->agent[4], &policy));
  f->config.audit_id = "denied";
  REQUIRE(single_agent_report_run(&f->config, &delivery, &phase) == SALTS_EPERM && !delivery);
  policy.allow_delegate = 1; REQUIRE(!turbo_agent_set_tool_policy(f->agent[4], &policy));
  f->scope = "healthy"; f->config.audit_id = "healthy"; f->config.goal = "healthy goal"; f->delivery = &delivery;
  REQUIRE(single_agent_report_run(&f->config, &delivery, &phase) == SALTS_OK && delivery);
  REQUIRE(!counter_add(f, "handoff")); REQUIRE(!counter_add(f, "delivery")); json_free(delivery);
  fprintf(stderr, "REPORT_RECOVER old_report_retried=0 healthy_delivery=1\n"); return 0;
}
static void cleanup(fixture_t *f) {
  cmeta_threadpool_destroy(f->pool); turbo_agent_router_destroy(f->router); turbo_agent_router_destroy(f->tool_router);
  for (int i = 0; i < 5; ++i) turbo_agent_destroy(f->agent[i]);
  turbo_tool_registry_destroy(f->tools);
  for (int i = 0; i < 4; ++i) turbo_agent_runtime_destroy(f->runtime[i]);
  turbo_agent_runtime_destroy(f->host); turbo_agent_runtime_destroy(f->oracle); cmeta_mutex_destroy(&f->oracle_mutex);
}
int main(int argc, char **argv) {
  fixture_t f = {0}; int rc; REQUIRE(argc == 4); f.cut = argv[2];
  REQUIRE(!open_fixture(&f, argv[3], !strcmp(argv[1], "init")));
  if (!strcmp(argv[1], "init")) {
    static const char *kinds[] = {"model-0", "model-1", "model-2", "model-3", "review", "tool", "handoff", "delivery", "stop-return"};
    for (int i = 0; i < 2; ++i) for (size_t j = 0; j < sizeof(kinds)/sizeof(kinds[0]); ++j) {
      tstr id = tstr_format("{}-{}", i ? "healthy" : "subject", kinds[j]); REQUIRE(id);
      rc = counter_put(&f, id, 0); tstr_free(id); REQUIRE(!rc);
    }
    rc = 0;
  } else if (!strcmp(argv[1], "crash")) rc = crash_stage(&f);
  else if (!strcmp(argv[1], "inspect")) rc = inspect(&f, 0);
  else if (!strcmp(argv[1], "recover")) rc = recover(&f);
  else if (!strcmp(argv[1], "verify")) rc = inspect(&f, 1);
  else rc = 2;
  if (!rc) cleanup(&f);
  return rc;
}
