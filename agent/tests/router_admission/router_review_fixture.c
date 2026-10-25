#include "turbo_agent_router.h"
#include "turbo_agent_session.h"
#include "turbo_model_provider.h"
#include "turbo_runtime_store_sqlite.h"
#include "../../src/turbo_agent_tool_journal_internal.h"
#include "../../src/turbo_agent_workflow_graph_internal.h"
#include <salts/clock.h>
#include <fmt.h>
#include <tstr.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FAILURE = 2, CRASH = 86 };
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "review fixture line %d: %s\n", __LINE__, #x); return FAILURE; } } while (0)

typedef struct fixture_s {
  const char *cut;
  int approved, inject, healthy, turn, creates, gets, puts, lists;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *host, *worker, *oracle;
  turbo_tool_registry_t *tools;
  turbo_agent_t *authority, *child;
  turbo_agent_router_t *router;
  cmeta_threadpool_t *pool;
  atomic_int queue_entered, queue_open;
} fixture_t;

static void crash_now(const fixture_t *f, const char *boundary) {
  fprintf(stderr, "REVIEW_CRASH cut=%s decision=%s boundary=%s\n", f->cut, f->approved ? "approve" : "reject", boundary);
  fflush(stderr); _Exit(CRASH);
}
static int counter_put(fixture_t *f, const char *id, int n) {
  json_value_t *r = json_create_object(); int rc = SALTS_ENOMEM;
  if (r && !turbo_runtime_json_object_set_int64(r, "count", n))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "counters", id, r);
  json_free(r); return rc;
}
static int counter_get(fixture_t *f, const char *id, int *n) {
  json_value_t *r = NULL; int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "counters", id, &r);
  int64_t v = turbo_runtime_json_value_as_int64(json_object_get(r, "count"), -1);
  if (!rc && (v < 0 || v > 10)) rc = SALTS_EPROTO;
  if (!rc) *n = (int)v;
  json_free(r); return rc;
}
static int counter_add(fixture_t *f, const char *id) {
  int n, rc = counter_get(f, id, &n);
  return rc ? rc : counter_put(f, id, n + 1);
}
static int store_put(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user; ++f->puts; return f->inner.put(f->inner.user_data, collection, id, text);
}
static int store_get(void *user, const char *collection, const char *id, char **out) {
  fixture_t *f = user; ++f->gets; return f->inner.get(f->inner.user_data, collection, id, out);
}
static int store_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  fixture_t *f = user; ++f->lists; return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static int store_create(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  int selected = f->inject && !strcmp(collection, TURBO_AGENT_ROUTER_REVIEW_INTENT_COLLECTION) && !strcmp(id, "review");
  int rc;
  ++f->creates;
  if (!strcmp(collection, TURBO_AGENT_ROUTER_REVIEW_INTENT_COLLECTION)) {
    cmeta_threadpool_stats_t stats = {0};
    int expected = selected && !strcmp(f->cut, "queue_rejected") ? 3 : 1;
    cmeta_threadpool_get_stats(f->pool, &stats);
    if (stats.submitted_tasks != expected) return SALTS_EPROTO;
    fprintf(stderr, "REVIEW_PREQUEUE expected=%d submitted=%lld\n", expected, (long long)stats.submitted_tasks);
  }
  if (selected && !strcmp(f->cut, "before_write")) crash_now(f, "before_atomic_create");
  rc = f->inner.create_if_absent(f->inner.user_data, collection, id, text);
  if (!rc && selected && !strcmp(f->cut, "after_write")) crash_now(f, "after_atomic_create_before_enqueue");
  if (!rc && selected && !strcmp(f->cut, "lost_ack")) return SALTS_EIO;
  return rc;
}
static void store_free(void *user) {
  fixture_t *f = user; f->inner.user_data_free(f->inner.user_data); f->inner = (turbo_agent_runtime_store_t){0};
}
static int open_runtime(const char *directory, const char *name, int create,
    turbo_agent_runtime_t **out, turbo_agent_runtime_store_t *store_out) {
  turbo_runtime_sqlite_config_t c; turbo_agent_runtime_store_t store = {0};
  tstr path = tstr_format("{}/{}", directory, name); int rc;
  REQUIRE(path); turbo_runtime_sqlite_config_init(&c); c.path = path; c.create_if_missing = create;
  rc = turbo_runtime_store_sqlite_open(&c, &store); tstr_free(path); REQUIRE(rc == SALTS_OK);
  if (store_out) *store_out = store;
  else { *out = turbo_agent_runtime_create(&store); REQUIRE(*out); }
  return 0;
}
static int open_fixture(fixture_t *f, const char *directory, int create) {
  turbo_agent_runtime_store_t wrapped = {store_put, store_get, store_list, f, store_free, store_create};
  REQUIRE(!open_runtime(directory, "host.db", create, NULL, &f->inner));
  f->host = turbo_agent_runtime_create(&wrapped); REQUIRE(f->host);
  REQUIRE(!open_runtime(directory, "worker.db", create, &f->worker, NULL));
  REQUIRE(!open_runtime(directory, "oracle.db", create, &f->oracle, NULL));
  return 0;
}

static int transport(const char *request, char **out, void *user) {
  fixture_t *f = user;
  static const char first[] = "{\"id\":\"fixture\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"review_effect\",\"arguments\":\"{\\\"value\\\":1}\"}]}";
  static const char healthy[] = "{\"id\":\"fixture\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"call-1\",\"name\":\"review_effect\",\"arguments\":\"{\\\"value\\\":2}\"}]}";
  static const char final[] = "{\"id\":\"fixture\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"done\"}]}]}";
  const char *text = f->turn ? final : f->healthy ? healthy : first;
  *out = NULL;
  if (f->turn > 1 || (f->turn && !strstr(request, "function_call_output")) ||
      counter_add(f, f->healthy ? "healthy_model" : "subject_model")) return -77;
  ++f->turn; *out = malloc(strlen(text) + 1);
  if (!*out) return -77;
  memcpy(*out, text, strlen(text) + 1); return 0;
}
static int guard(turbo_agent_t *agent, const json_value_t *state, const char *id,
    const char *name, const char *arguments, const char **reason, void *user) {
  (void)agent; (void)user;
  if (!turbo_agent_tool_approval_review_matches(state, id, name, arguments)) {
    *reason = "approval_required"; return -1;
  }
  return 0;
}
static turbo_tool_status_t effect(const char *args, const turbo_tool_execution_context_t *context,
    char **out, void *user) {
  fixture_t *f = user; *out = NULL;
  if (!context || !context->cancel_token || !context->run_id || !context->thread_id ||
      strcmp(context->thread_id, "review-thread") || !context->tool_call_id || strcmp(context->tool_call_id, "call-1") ||
      !turbo_agent_current_tool_approval_granted() || strcmp(args, f->healthy ? "{\"value\":2}" : "{\"value\":1}") ||
      counter_add(f, f->healthy ? "healthy_effect" : "subject_effect")) return TURBO_TOOL_ERROR;
  if (f->inject && !strcmp(f->cut, "in_tool")) crash_now(f, "tool_effect_committed_before_return");
  *out = malloc(sizeof("{\"ok\":true}"));
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, "{\"ok\":true}", sizeof("{\"ok\":true}")); return TURBO_TOOL_OK;
}
static int setup_router(fixture_t *f, int allowed) {
  static const char *capabilities[] = {"custom_tools"};
  turbo_tool_definition_v6_t tool = {0}; turbo_agent_config_t ac = {0};
  turbo_agent_router_endpoint_t endpoint = {0}; turbo_agent_router_config_t config;
  turbo_agent_policy_t policy = turbo_agent_policy_default(); turbo_agent_guardrail_t gr = {0};
  cmeta_threadpool_config_t pc = {.num_threads = 1, .queue_capacity = 1};
  f->tools = turbo_tool_registry_create(); REQUIRE(f->tools);
  tool.struct_size = sizeof(tool); tool.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  tool.definition.name = "review.effect"; tool.definition.description = "Independent test effect";
  tool.definition.parameters_json = "{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"integer\"}},\"required\":[\"value\"]}";
  tool.definition.strict = 1; tool.definition.user_data = f; tool.context_handler = effect;
  tool.required_capabilities = capabilities; tool.required_capability_count = 1;
  tool.result_schema_json = "{\"type\":\"object\",\"properties\":{\"ok\":{\"type\":\"boolean\"}},\"required\":[\"ok\"]}";
  tool.strict_result = 1; tool.effect_flags = TURBO_TOOL_EFFECT_WRITE;
  REQUIRE(turbo_tool_registry_add_v6(f->tools, &tool) == TURBO_TOOL_OK);
  ac.model = "review-model"; ac.provider = turbo_model_provider_openai_responses();
  ac.tool_registry = f->tools; ac.transport_fn = transport; ac.transport_user_data = f;
  f->authority = turbo_agent_create(&ac); f->child = turbo_agent_create(&ac); REQUIRE(f->authority && f->child);
  policy.allow_custom_tools = 1; REQUIRE(!turbo_agent_set_tool_policy(f->child, &policy));
  policy.allow_delegate = allowed; REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy));
  gr.before_tool = guard; gr.user_data = f; REQUIRE(!turbo_agent_add_guardrail(f->authority, &gr));
  endpoint.id = "worker"; endpoint.agent = f->child; endpoint.runtime = f->worker;
  endpoint.roles = 3; endpoint.capabilities = 7; endpoint.max_difficulty = 100;
  endpoint.max_input_tokens = endpoint.max_output_tokens = 10000;
  endpoint.input_price_micros = endpoint.output_price_micros = 1000;
  turbo_agent_router_config_init(&config); config.authority_agent = f->authority;
  config.endpoints = &endpoint; config.endpoint_count = 1;
  REQUIRE(turbo_agent_router_create(&config, &f->router) == SALTS_OK);
  f->pool = cmeta_threadpool_create_with_config(&pc); REQUIRE(f->pool); return 0;
}
static int submit(fixture_t *f, const char *id, turbo_agent_router_task_t **out) {
  turbo_agent_router_request_t request; turbo_agent_runtime_parent_link_t parent = {0};
  json_value_t *input = turbo_agent_session_create_input_state_json_value(f->healthy ? "different healthy operation" : "subject operation");
  int rc; REQUIRE(input);
  turbo_agent_router_request_init(&request); request.role = TURBO_AGENT_ROUTER_EXECUTE;
  request.required_capabilities = TURBO_AGENT_ROUTER_TEXT | TURBO_AGENT_ROUTER_TOOL_PROPOSALS;
  request.difficulty = 20; request.input_tokens = request.output_tokens = 1000; request.budget_micros = 20;
  request.timeout_ms = 15000; request.heartbeat_timeout_ms = 10000; request.thread_id = "review-thread";
  parent.parent_agent_run_id = "review-host"; parent.parent_tool_call_id = "delegate"; request.parent_link = &parent;
  rc = turbo_agent_router_submit_with_intent(f->router, f->pool, f->host, id, &request, input, 2, out);
  json_free(input); return rc;
}
static void queue_block(void *user) {
  fixture_t *f = user; atomic_store_explicit(&f->queue_entered, 1, memory_order_release);
  while (!atomic_load_explicit(&f->queue_open, memory_order_acquire)) cmeta_thread_yield();
}
static int crash_stage(fixture_t *f) {
  turbo_agent_router_task_t *task = NULL; json_value_t *view = NULL, *summary = NULL, *state = NULL;
  int rc; REQUIRE(!setup_router(f, 1));
  REQUIRE(submit(f, "initial-subject", &task) == SALTS_OK && task);
  REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  REQUIRE(turbo_agent_router_task_inspect(task, &view) == SALTS_OK);
  REQUIRE(json_get_bool(view, "approval_pending", 0)); json_free(view); view = NULL;
  REQUIRE(turbo_agent_router_task_record_observation(task, f->oracle, "initial") == SALTS_OK);
  f->inject = 1;
  if (!strcmp(f->cut, "queue_rejected")) {
    uint64_t end = cmeta_monotonic_ms() + 2000;
    REQUIRE(cmeta_threadpool_try_submit(f->pool, queue_block, f) == SALTS_OK);
    while (!atomic_load_explicit(&f->queue_entered, memory_order_acquire) && cmeta_monotonic_ms() < end) cmeta_thread_yield();
    REQUIRE(atomic_load(&f->queue_entered)); REQUIRE(cmeta_threadpool_try_submit(f->pool, queue_block, f) == SALTS_OK);
  }
  rc = turbo_agent_router_task_review_with_intent(task, f->pool, f->host, "review", f->approved);
  if (!strcmp(f->cut, "lost_ack") || !strcmp(f->cut, "queue_rejected")) {
    REQUIRE(rc == (!strcmp(f->cut, "lost_ack") ? SALTS_EIO : SALTS_ENOBUFS));
    REQUIRE(turbo_agent_router_task_inspect(task, &view) == SALTS_OK && json_get_bool(view, "approval_pending", 0));
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(view, "review_resumes"), -1) == 0);
    REQUIRE(turbo_agent_router_task_take_result(task, &summary, &state) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(summary, "status"), "interrupted"));
    crash_now(f, !strcmp(f->cut, "lost_ack") ? "after_lost_ack_return" : "after_queue_rejection");
  }
  REQUIRE(rc == SALTS_OK); REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  if (!strcmp(f->cut, "after_observation")) {
    REQUIRE(turbo_agent_router_task_record_observation(task, f->oracle, "terminal") == SALTS_OK);
    crash_now(f, "terminal_observation_before_delivery");
  }
  REQUIRE(!strcmp(f->cut, "before_delivery")); crash_now(f, "terminal_before_delivery"); return FAILURE;
}

static int inspect(fixture_t *f, int healthy_done) {
  json_value_t *initial = NULL, *admission = NULL, *review = NULL, *checkpoint = NULL, *journal = NULL, *claims = NULL, *terminal = NULL;
  int completed = !strcmp(f->cut, "before_delivery") || !strcmp(f->cut, "after_observation");
  int effect_count = f->approved && (completed || !strcmp(f->cut, "in_tool"));
  int models = f->approved && completed ? 2 : 1, n, rc;
  REQUIRE(turbo_agent_router_load_observation(f->oracle, "initial", &initial) == SALTS_OK);
  REQUIRE(turbo_agent_router_load_intent(f->host, "initial-subject", &admission) == SALTS_OK);
  REQUIRE(!strcmp(json_get_string(initial, "task_id"), json_get_string(admission, "task_id")));
  REQUIRE(turbo_agent_runtime_get_checkpoint(f->worker, json_get_string(initial, "runtime_checkpoint_id"), &checkpoint) == SALTS_OK);
  REQUIRE(!strcmp(json_get_string(checkpoint, "run_id"), json_get_string(initial, "runtime_run_id")));
  REQUIRE(!strcmp(json_get_string(checkpoint, "thread_id"), "review-thread"));
  rc = turbo_agent_router_load_review_intent(f->host, "review", &review);
  REQUIRE(rc == (!strcmp(f->cut, "before_write") ? SALTS_ENOENT : SALTS_OK));
  if (!rc) {
    REQUIRE(!strcmp(json_get_string(review, "task_id"), json_get_string(initial, "task_id")));
    REQUIRE(!strcmp(json_get_string(review, "previous_execution_id"), json_get_string(initial, "execution_id")));
    REQUIRE(strcmp(json_get_string(review, "execution_id"), json_get_string(initial, "execution_id")));
    REQUIRE(!strcmp(json_get_string(review, "source_run_id"), json_get_string(initial, "runtime_run_id")));
    REQUIRE(!strcmp(json_get_string(review, "source_checkpoint_id"), json_get_string(initial, "runtime_checkpoint_id")));
    REQUIRE(!strcmp(json_get_string(review, "admission_id"), "initial-subject"));
    REQUIRE(!strcmp(json_get_string(review, "decision"), f->approved ? "approve" : "reject"));
    REQUIRE(turbo_runtime_json_value_as_int64(json_object_get(review, "deadline_mono_ms"), 0) ==
            turbo_runtime_json_value_as_int64(json_object_get(initial, "deadline_mono_ms"), -1));
  }
  REQUIRE(!counter_get(f, "subject_model", &n) && n == models);
  REQUIRE(!counter_get(f, "subject_effect", &n) && n == effect_count);
  REQUIRE(!counter_get(f, "subject_delivery", &n) && n == 0);
  REQUIRE(!counter_get(f, "healthy_model", &n) && n == 2 * healthy_done);
  REQUIRE(!counter_get(f, "healthy_effect", &n) && n == healthy_done);
  REQUIRE(!counter_get(f, "healthy_delivery", &n) && n == healthy_done);
  REQUIRE(turbo_agent_runtime_store_list_json(f->worker, TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
      "run_id", json_get_string(initial, "runtime_run_id"), &journal) == SALTS_OK);
  REQUIRE(turbo_agent_runtime_store_list_json(f->worker, TURBO_AGENT_TOOL_CLAIM_COLLECTION,
      "run_id", json_get_string(initial, "runtime_run_id"), &claims) == SALTS_OK);
  REQUIRE(json_array_size(journal) == (size_t)effect_count && json_array_size(claims) == (size_t)effect_count);
  if (effect_count) {
    const json_value_t *entry = json_array_get(journal, 0);
    REQUIRE(!strcmp(json_get_string(entry, "phase"), completed ? "committed" : "started"));
    REQUIRE(!strcmp(json_get_string(entry, "call_id"), "call-1"));
    REQUIRE(!strcmp(json_get_string(json_array_get(claims, 0), "phase"), "admitted"));
    if (completed) REQUIRE(!strcmp(json_get_string(entry, "output"), "{\"ok\":true}"));
  }
  if (!strcmp(f->cut, "after_observation")) {
    REQUIRE(turbo_agent_router_load_observation(f->oracle, "terminal", &terminal) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(terminal, "task_id"), json_get_string(review, "task_id")));
    REQUIRE(!strcmp(json_get_string(terminal, "execution_id"), json_get_string(review, "execution_id")));
    REQUIRE(!strcmp(json_get_string(terminal, "runtime_run_id"), json_get_string(review, "source_run_id")));
  }
  if (healthy_done) {
    json_value_t *healthy = NULL, *observed = NULL, *run = NULL;
    REQUIRE(turbo_agent_router_load_review_intent(f->host, "healthy-review", &healthy) == SALTS_OK);
    REQUIRE(turbo_agent_router_load_observation(f->oracle, "healthy-terminal", &observed) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(healthy, "execution_id"), json_get_string(observed, "execution_id")));
    REQUIRE(strcmp(json_get_string(healthy, "task_id"), json_get_string(initial, "task_id")));
    REQUIRE(turbo_agent_runtime_get_run(f->worker, json_get_string(observed, "runtime_run_id"), &run) == SALTS_OK);
    REQUIRE(!strcmp(json_get_string(run, "thread_id"), "review-thread"));
    json_free(healthy); json_free(observed); json_free(run);
  }
  fprintf(stderr, "REVIEW_INSPECT cut=%s decision=%s intent=%d subject_model=%d subject_effect=%d subject_delivery=0 healthy=%d\n",
          f->cut, f->approved ? "approve" : "reject", rc == SALTS_OK, models, effect_count, healthy_done);
  json_free(initial); json_free(admission); json_free(review); json_free(checkpoint); json_free(journal); json_free(claims); json_free(terminal);
  return 0;
}

static int recover(fixture_t *f) {
  turbo_agent_router_task_t *task = NULL; json_value_t *old = NULL, *after = NULL, *summary = NULL, *state = NULL;
  turbo_agent_policy_t policy = turbo_agent_policy_default(); char *before_text = NULL, *after_text = NULL;
  int exists = strcmp(f->cut, "before_write") != 0, gets, puts, lists;
  REQUIRE(turbo_agent_router_load_review_intent(f->host, "review", &old) == (exists ? SALTS_OK : SALTS_ENOENT));
  if (exists) { before_text = json_serialize(old, NULL); REQUIRE(before_text); }
  f->healthy = 1; REQUIRE(!setup_router(f, 0));
  REQUIRE(submit(f, "denied-healthy", &task) == SALTS_EPERM && !task);
  policy.allow_custom_tools = policy.allow_delegate = 1; REQUIRE(!turbo_agent_set_tool_policy(f->authority, &policy));
  REQUIRE(submit(f, "initial-healthy", &task) == SALTS_OK && task);
  REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  if (exists) {
    gets = f->gets; puts = f->puts; lists = f->lists;
    REQUIRE(turbo_agent_router_task_review_with_intent(task, f->pool, f->host, "review", 1) == SALTS_EALREADY);
    REQUIRE(f->gets == gets && f->puts == puts && f->lists == lists);
    REQUIRE(turbo_agent_router_load_review_intent(f->host, "review", &after) == SALTS_OK);
    after_text = json_serialize(after, NULL); REQUIRE(after_text && !strcmp(before_text, after_text));
  }
  REQUIRE(turbo_agent_router_task_review_with_intent(task, f->pool, f->host, "healthy-review", 1) == SALTS_OK);
  REQUIRE(turbo_agent_router_task_wait(task, 5000) == SALTS_OK);
  REQUIRE(turbo_agent_router_task_record_observation(task, f->oracle, "healthy-terminal") == SALTS_OK);
  REQUIRE(turbo_agent_router_task_take_result(task, &summary, &state) == SALTS_OK);
  REQUIRE(!strcmp(json_get_string(summary, "status"), "completed")); REQUIRE(!counter_add(f, "healthy_delivery"));
  REQUIRE(turbo_agent_router_task_release(task) == SALTS_OK);
  fprintf(stderr, "REVIEW_RECOVER cut=%s decision=%s old_effect_retried=0 healthy_effect=1 healthy_delivery=1\n", f->cut, f->approved ? "approve" : "reject");
  json_serialize_free(before_text); json_serialize_free(after_text); json_free(old); json_free(after); json_free(summary); json_free(state);
  return 0;
}
static void cleanup(fixture_t *f) {
  atomic_store(&f->queue_open, 1); cmeta_threadpool_destroy(f->pool); turbo_agent_router_destroy(f->router);
  turbo_agent_destroy(f->child); turbo_agent_destroy(f->authority); turbo_tool_registry_destroy(f->tools);
  turbo_agent_runtime_destroy(f->host); turbo_agent_runtime_destroy(f->worker); turbo_agent_runtime_destroy(f->oracle);
}
int main(int argc, char **argv) {
  fixture_t f = {0}; int rc;
  REQUIRE(argc == 5); f.cut = argv[2]; f.approved = !strcmp(argv[3], "approve");
  REQUIRE(f.approved || !strcmp(argv[3], "reject")); REQUIRE(strcmp(f.cut, "in_tool") || f.approved);
  atomic_init(&f.queue_entered, 0); atomic_init(&f.queue_open, 0);
  REQUIRE(!open_fixture(&f, argv[4], !strcmp(argv[1], "init")));
  if (!strcmp(argv[1], "init")) {
    static const char *ids[] = {"subject_model", "subject_effect", "subject_delivery", "healthy_model", "healthy_effect", "healthy_delivery"};
    for (size_t i = 0; i < sizeof(ids)/sizeof(ids[0]); ++i) REQUIRE(!counter_put(&f, ids[i], 0));
    rc = 0;
  } else if (!strcmp(argv[1], "crash")) rc = crash_stage(&f);
  else if (!strcmp(argv[1], "inspect")) rc = inspect(&f, 0);
  else if (!strcmp(argv[1], "recover")) rc = recover(&f);
  else if (!strcmp(argv[1], "verify")) rc = inspect(&f, 1);
  else rc = FAILURE;
  if (!rc) cleanup(&f);
  return rc;
}
