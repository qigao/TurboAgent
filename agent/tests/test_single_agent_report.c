#include "tinytest.h"
#include "../examples/single_agent_report.h"
#include "turbo_model_provider.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct report_fixture_s report_fixture_t;
typedef struct report_model_s {
  report_fixture_t *fixture;
  unsigned index;
  atomic_int calls;
  json_value_t *first_request;
} report_model_t;
struct report_fixture_s {
  report_model_t models[4];
  turbo_agent_t *agents[5];
  turbo_agent_runtime_t *runtimes[4];
  turbo_agent_router_endpoint_t endpoints[4];
  turbo_agent_router_t *router, *tool_router;
  turbo_tool_registry_t *tools;
  cmeta_threadpool_t *pool;
  single_agent_report_config_t config;
  document_render_catalog_t catalog;
  atomic_int tool_calls, authority_calls;
  int approval, reviews, context_ok, bad_receipt, no_tool, fail_index, proposal_index, revoke, nul_aggregate, late_review;
};

static char *report_copy(const char *text) {
  char *copy = malloc(strlen(text) + 1);
  if (copy) memcpy(copy, text, strlen(text) + 1);
  return copy;
}

static int report_authority_transport(const char *request, char **out, void *user) {
  report_fixture_t *f = user;
  (void)request;
  *out = NULL;
  atomic_fetch_add(&f->authority_calls, 1);
  return -77; /* The Host authority must never issue a model request. */
}

static int report_transport(const char *request, char **out, void *user) {
  report_model_t *model = user;
  report_fixture_t *f = model->fixture;
  int call = atomic_fetch_add(&model->calls, 1);
  static const char *names[] = {"writer", "reviewer", "planner", "executor"};
  const char *response;
  json_value_t *body = json_parse(request, strlen(request));
  const char *name = body ? json_get_string(body, "model") : NULL;
  *out = NULL;
  if (!name || strcmp(name, names[model->index])) { json_free(body); return -1; }
  if (!call) model->first_request = body;
  else json_free(body);
  if ((int)model->index == f->fail_index) return -77;
  if (model->index == 3 && !call && !f->no_tool) {
    response = "{\"id\":\"exec\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"render-1\","
        "\"name\":\"document_render\",\"arguments\":\"{\\\"title\\\":\\\"Report\\\",\\\"passages\\\":["
        "{\\\"text\\\":\\\"Host-supervised report\\\",\\\"source_id\\\":\\\"brief\\\"}]}\"}]}";
  } else if ((int)model->index == f->proposal_index) {
    response = "{\"id\":\"worker-proposal\",\"output\":[{\"type\":\"function_call\",\"call_id\":\"bad\","
        "\"name\":\"document_render\",\"arguments\":\"{}\"}]}";
  } else if (model->index == 1) {
    response = "{\"id\":\"review\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Review draft with uncertainty\"},\"finish_reason\":\"stop\"}]}";
  } else if (model->index == 2 && f->nul_aggregate) {
    response = "{\"id\":\"plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"draft\\u0000hidden\"}]}]}";
  } else if (model->index == 2) {
    response = "{\"id\":\"plan\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"Synthesize the two drafts using brief\"}]}]}";
  } else {
    response = "{\"id\":\"text\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"output_text\",\"text\":\"Model claims everything is done\"}]}]}";
  }
  *out = report_copy(response);
  return *out ? 0 : -1;
}

static int report_wait_for_deadline(const char *request, const turbo_cancel_token_t *token,
    unsigned timeout_ms, unsigned connect_ms, turbo_agent_transport_response_t *response, void *user) {
  report_model_t *model = user;
  (void)request; (void)timeout_ms; (void)connect_ms;
  atomic_fetch_add(&model->calls, 1);
  if (!token || turbo_cancel_token_wait(token, 5000) != TURBO_CANCEL_WAIT_SIGNALED) return -77;
  response->transport_status = turbo_cancel_token_check(token);
  return response->transport_status;
}

static turbo_tool_status_t report_tool(const char *args,
    const turbo_tool_execution_context_t *context, char **out, void *user) {
  report_fixture_t *f = user;
  atomic_fetch_add(&f->tool_calls, 1);
  if (!turbo_agent_current_tool_approval_granted()) return TURBO_TOOL_ERROR;
  if (f->bad_receipt) {
    *out = report_copy("{\"markdown\":false}");
    return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
  }
  return single_agent_report_render(args, context, out, &f->catalog);
}

static int report_host_review(const json_value_t *summary, const json_value_t *state, void *user) {
  report_fixture_t *f = user;
  const char *status = summary ? json_get_string(summary, "status") : NULL;
  ++f->reviews;
  f->context_ok = status && !strcmp(status, "interrupted") &&
      json_get_string(summary, "checkpoint_id") && turbo_agent_state_review_required(state) &&
      turbo_agent_state_review_note(state) && atomic_load(&f->tool_calls) == 0;
  if (f->revoke) {
    /* Actual Host quiescence: both workers and the aggregator were released,
     * the approval segment is terminal, and this is not a runtime callback. */
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    policy.allow_delegate = 1; policy.allow_runtime_tools = 1;
    int rc = turbo_agent_set_tool_policy(f->agents[f->revoke == 1 ? 4 : 3], &policy);
    if (rc) return -1;
  }
  if (f->late_review) {
    uint64_t until = cmeta_monotonic_ms() + f->config.timeout_ms;
    while (cmeta_monotonic_ms() <= until) cmeta_thread_yield();
  }
  return f->approval;
}

/* Read an actual provider user message, rather than searching serialized
 * requests for markers that could also occur in system instructions. */
static json_value_t *report_user_data(const json_value_t *request) {
  const json_value_t *messages = json_object_get(request, "input");
  if (!messages) messages = json_object_get(request, "messages");
  for (size_t i = 0; i < json_array_size(messages); ++i) {
    const json_value_t *message = json_array_get(messages, i);
    const char *role = json_get_string(message, "role");
    const json_value_t *content = json_object_get(message, "content");
    if (!role || strcmp(role, "user")) continue;
    const char *text = json_type(content) == JSON_STRING ? json_string(content)
        : json_get_string(json_array_get(content, 0), "text");
    if (text) return json_parse(text, strlen(text));
  }
  return NULL;
}

spec("one Host goal composed from different model workers") {
  static report_fixture_t f;
  static json_value_t *delivery;
  static const char *phase;
  before_each() {
    static const document_render_source_t sources[] = {{"brief", "host-v1"}};
    static const char *names[] = {"writer", "reviewer", "planner", "executor"};
    memset(&f, 0, sizeof(f));
    f.approval = 1; f.fail_index = f.proposal_index = -1;
    f.catalog = (document_render_catalog_t){sources, 1};
    atomic_init(&f.tool_calls, 0);
    atomic_init(&f.authority_calls, 0);
    f.tools = turbo_tool_registry_create(); check_not_null(f.tools);
    turbo_tool_definition_v6_t tool = single_agent_report_tool_definition(&f.catalog);
    tool.definition.json_value_handler = NULL; tool.context_handler = report_tool;
    tool.definition.user_data = &f;
    check_equal(turbo_tool_registry_add_v6(f.tools, &tool), TURBO_TOOL_OK);
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    policy.allow_delegate = 1; policy.allow_runtime_tools = 1;
    for (unsigned i = 0; i < 5; ++i) {
      turbo_agent_config_t c = {0};
      c.model = i < 4 ? names[i] : "authority";
      c.provider = i == 1 ? turbo_model_provider_openai_compatible_chat_completions()
                          : turbo_model_provider_openai_responses();
      c.tool_registry = f.tools;
      if (i < 4) {
        f.models[i].fixture = &f; f.models[i].index = i;
        atomic_init(&f.models[i].calls, 0);
        c.transport_fn = report_transport; c.transport_user_data = &f.models[i];
      } else {
        c.transport_fn = report_authority_transport; c.transport_user_data = &f;
      }
      f.agents[i] = turbo_agent_create(&c); check_not_null(f.agents[i]);
      check_equal(turbo_agent_set_tool_policy(f.agents[i], &policy), SALTS_OK);
      if (i < 4) {
        turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
        f.runtimes[i] = turbo_agent_runtime_create(&store); check_not_null(f.runtimes[i]);
        f.endpoints[i].id = names[i]; f.endpoints[i].agent = f.agents[i]; f.endpoints[i].runtime = f.runtimes[i];
        f.endpoints[i].roles = i == 2 ? TURBO_AGENT_ROUTER_PLAN : TURBO_AGENT_ROUTER_EXECUTE;
        f.endpoints[i].capabilities = TURBO_AGENT_ROUTER_TEXT | (i == 3 ? TURBO_AGENT_ROUTER_TOOL_PROPOSALS : 0);
        f.endpoints[i].max_difficulty = 100;
        f.endpoints[i].max_input_tokens = f.endpoints[i].max_output_tokens = 10000;
        f.endpoints[i].input_price_micros = f.endpoints[i].output_price_micros = 1000;
        turbo_agent_guardrail_t response_guard = {0};
        response_guard.after_model = single_agent_report_response_guard;
        check_equal(turbo_agent_add_guardrail(f.agents[i], &response_guard), SALTS_OK);
      }
    }
    turbo_agent_guardrail_t guard = {0}; guard.before_tool = single_agent_report_guard;
    check_equal(turbo_agent_add_guardrail(f.agents[4], &guard), SALTS_OK);
    turbo_agent_router_config_t router_config;
    turbo_agent_router_config_init(&router_config);
    router_config.authority_agent = f.agents[4]; router_config.endpoints = f.endpoints; router_config.endpoint_count = 3;
    check_equal(turbo_agent_router_create(&router_config, &f.router), SALTS_OK);
    router_config.endpoints = &f.endpoints[3]; router_config.endpoint_count = 1;
    check_equal(turbo_agent_router_create(&router_config, &f.tool_router), SALTS_OK);
    cmeta_threadpool_config_t pc = {.num_threads = 2, .queue_capacity = 2};
    f.pool = cmeta_threadpool_create_with_config(&pc); check_not_null(f.pool);
    f.config.router = f.router; f.config.tool_router = f.tool_router;
    f.config.pool = f.pool; f.config.authority = f.agents[4]; f.config.tools = f.tools;
    f.config.parent_run_id = "one-host-task"; f.config.goal = "Write a careful report";
    f.config.brief = "Approved Host context"; f.config.timeout_ms = 10000;
    f.config.review = report_host_review; f.config.review_user = &f;
    turbo_agent_router_request_init(&f.config.worker);
    f.config.worker.input_tokens = f.config.worker.output_tokens = 1000;
    f.config.worker.budget_micros = 10;
    f.config.aggregate = f.config.execute = f.config.worker;
    f.config.reserved_budget_micros = 40;
    delivery = NULL; phase = NULL;
  }
  after_each() {
    /* A successful destroy proves all accepted tasks/reservations were released
     * by the Host flow, including early failure and partial admission. */
    check_equal(atomic_load(&f.authority_calls), 0);
    check_equal(turbo_agent_router_destroy(f.router), SALTS_OK);
    check_equal(turbo_agent_router_destroy(f.tool_router), SALTS_OK);
    cmeta_threadpool_destroy(f.pool);
    for (unsigned i = 0; i < 4; ++i) {
      turbo_agent_runtime_destroy(f.runtimes[i]); json_free(f.models[i].first_request);
    }
    for (unsigned i = 0; i < 5; ++i) turbo_agent_destroy(f.agents[i]);
    turbo_tool_registry_destroy(f.tools); json_free(delivery);
  }

  it("hands off one goal and canonical evidence, approves exactly one Host call, and delivers the tool artifact") {
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_OK);
    check_equal(phase, "delivery"); check_not_null(delivery);
    check_equal(f.reviews, 1); check_true(f.context_ok); check_equal(atomic_load(&f.tool_calls), 1);
    for (unsigned i = 0; i < 4; ++i) check_equal(atomic_load(&f.models[i].calls), i == 3 ? 2 : 1);
    check_equal(json_get_string(delivery, "verification"), "reference_identity_only");
    check_equal(json_get_string(delivery, "markdown"), "# Report\n\nHost\\-supervised report\n\nSource: brief @ host\\-v1\n\n");
    for (unsigned i = 0; i < 2; ++i) {
      json_value_t *data = report_user_data(f.models[i].first_request); check_not_null(data);
      check_equal(json_get_string(data, "host_goal"), f.config.goal);
      check_equal(json_get_string(data, "source_text"), f.config.brief); json_free(data);
    }
    json_value_t *evidence = report_user_data(f.models[2].first_request); check_not_null(evidence);
    const json_value_t *workers = json_object_get(evidence, "worker_results");
    check_equal(json_get_string(evidence, "host_task"), f.config.goal); check_equal(json_array_size(workers), 2u);
    for (unsigned i = 0; i < 2; ++i) {
      const json_value_t *worker = json_array_get(workers, i);
      const char *run = json_get_string(worker, "run_id");
      json_value_t *canonical = NULL;
      check_not_null(run);
      check_equal(turbo_agent_runtime_get_run(f.runtimes[i], run, &canonical), SALTS_OK);
      check_equal(json_get_string(worker, "parent_agent_run_id"), f.config.parent_run_id);
      check_equal(json_get_string(canonical, "id"), run); json_free(canonical);
    }
    json_value_t *execution = report_user_data(f.models[3].first_request); check_not_null(execution);
    check_equal(json_get_string(execution, "host_goal"), f.config.goal);
    check_equal(json_get_string(execution, "source_text"), f.config.brief);
    check_equal(json_get_string(execution, "aggregation_text"), "Synthesize the two drafts using brief");
    json_value_t *canonical = NULL;
    check_equal(turbo_agent_runtime_get_run(f.runtimes[2], json_get_string(execution, "aggregation_run_id"), &canonical), SALTS_OK);
    check_equal(json_get_string(canonical, "parent_agent_run_id"), f.config.parent_run_id);
    json_free(canonical); json_free(evidence); json_free(execution);
  }

  for (int decision = 0; decision < 2; ++decision) {
    it("stops after Host rejection or abort without an effect or final model turn, case=%d", decision) {
      f.approval = decision ? -1 : 0;
      check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ECANCELED);
      check_null(delivery); check_equal(phase, "approval"); check_equal(f.reviews, 1);
      check_true(f.context_ok); check_equal(atomic_load(&f.tool_calls), 0);
      check_equal(atomic_load(&f.models[3].calls), 1);
    }
  }

  for (int owner = 1; owner <= 2; ++owner) {
    it("rejects same-grant policy replacement at a quiescent Host approval boundary, owner=%d", owner) {
      f.revoke = owner;
      check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EPERM);
      check_null(delivery); check_equal(f.reviews, 1); check_true(f.context_ok);
      check_equal(atomic_load(&f.tool_calls), 0); check_equal(atomic_load(&f.models[3].calls), 1);
    }
  }

  it("does not accept a model completion claim without the required approved tool receipt") {
    f.no_tool = 1;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EPROTO);
    check_null(delivery); check_equal(f.reviews, 0); check_equal(atomic_load(&f.tool_calls), 0);
  }

  it("withholds a malformed tool result before a follow-up model or delivery") {
    f.bad_receipt = 1;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EIO);
    check_null(delivery); check_equal(atomic_load(&f.tool_calls), 1);
    check_equal(atomic_load(&f.models[3].calls), 1);
  }

  for (int index = 0; index < 4; ++index) {
    it("drains accepted owners and withholds delivery after Provider failure, model=%d", index) {
      f.fail_index = index;
      check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EIO);
      check_null(delivery); check_equal(atomic_load(&f.tool_calls), 0); check_equal(f.reviews, 0);
      if (index < 2) check_equal(atomic_load(&f.models[2].calls), 0);
      if (index < 3) check_equal(atomic_load(&f.models[3].calls), 0);
    }
  }

  it("halts unresolved worker proposals without executing them or entering aggregation") {
    f.proposal_index = 0;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EPROTO);
    check_null(delivery); check_equal(atomic_load(&f.tool_calls), 0);
    check_equal(atomic_load(&f.models[2].calls), 0);
  }

  it("releases the first accepted worker when the second has no qualified endpoint") {
    check_equal(turbo_agent_router_destroy(f.router), SALTS_OK);
    turbo_agent_router_config_t c; turbo_agent_router_config_init(&c);
    c.authority_agent = f.agents[4]; c.endpoints = f.endpoints; c.endpoint_count = 1;
    check_equal(turbo_agent_router_create(&c, &f.router), SALTS_OK); f.config.router = f.router;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EBUSY);
    check_null(delivery); check_equal(atomic_load(&f.tool_calls), 0);
    check_equal(atomic_load(&f.models[1].calls), 0); check_equal(atomic_load(&f.models[2].calls), 0);
  }

  it("reserves the entire request estimate before any model admission") {
    f.config.reserved_budget_micros = 39;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ENOBUFS);
    check_null(delivery); check_equal(phase, "admission");
    for (unsigned i = 0; i < 4; ++i) check_equal(atomic_load(&f.models[i].calls), 0);
  }

  it("rejects an embedded NUL in the raw aggregate response before accepting a normalized handoff") {
    f.nul_aggregate = 1;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_EIO);
    check_null(delivery); check_equal(phase, "aggregation");
    check_equal(atomic_load(&f.models[3].calls), 0); check_equal(atomic_load(&f.tool_calls), 0);
  }

  it("cancels and drains actual worker execution at the whole-task deadline without advancing stages") {
    f.config.timeout_ms = 100;
    check_equal(turbo_agent_transport_v2_set(f.agents[0], report_wait_for_deadline, &f.models[0], NULL), SALTS_OK);
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ETIMEDOUT);
    check_null(delivery); check_equal(phase, "workers");
    check_equal(atomic_load(&f.models[2].calls), 0); check_equal(atomic_load(&f.tool_calls), 0);
  }

  it("rejects overflowing reservation arithmetic before starting a model") {
    f.config.worker.budget_micros = UINT64_MAX;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ERANGE);
    check_null(delivery); check_equal(phase, "admission");
    for (unsigned i = 0; i < 4; ++i) check_equal(atomic_load(&f.models[i].calls), 0);
  }

  it("rejects an approval returned after the Host deadline before any resumed tool or model call") {
    f.config.timeout_ms = 1000; f.late_review = 1;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ETIMEDOUT);
    check_null(delivery); check_equal(phase, "approval"); check_equal(f.reviews, 1);
    check_true(f.context_ok); check_equal(atomic_load(&f.tool_calls), 0);
    check_equal(atomic_load(&f.models[3].calls), 1);
  }

  it("rejects an oversized Host context packet before model admission") {
    tstr brief = tstr_new_len(NULL, TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1u);
    check_not_null(brief); memset(brief, 'x', TURBO_AGENT_ROUTER_GROUP_MAX_TEXT_BYTES + 1u);
    f.config.brief = brief;
    check_equal(single_agent_report_run(&f.config, &delivery, &phase), SALTS_ENOBUFS);
    check_null(delivery); check_equal(phase, "admission"); tstr_free(brief);
    for (unsigned i = 0; i < 4; ++i) check_equal(atomic_load(&f.models[i].calls), 0);
  }
}
