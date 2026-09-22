#include "tinytest.h"
#include "turbo_agent_graph.h"
#include "turbo_agent_state.h"
#include "turbo_agent_tool_executor.h"
#include "turbo_runtime_control.h"
#include "turbo_tool_registry.h"

#include "../src/turbo_agent_tool_executor_internal.h"

#include <salts/thread.h>

#include <stdlib.h>
#include <string.h>

typedef struct tool_executor_probe_s {
  salts_mutex_t mutex;
  int calls;
  int active;
  int max_active;
} tool_executor_probe_t;

typedef struct tool_executor_fault_store_s {
  turbo_agent_runtime_store_t inner;
  int fail_committed_once;
  int puts;
  int committed_seen;
} tool_executor_fault_store_t;

static int tool_executor_fault_put(void *user_data, const char *collection, const char *id,
                                   const char *record_json) {
  tool_executor_fault_store_t *fault = (tool_executor_fault_store_t *)user_data;
  ++fault->puts;
  if (fault->fail_committed_once && strstr(record_json, "committed")) {
    fault->fail_committed_once = 0;
    fault->committed_seen = 1;
    return -1;
  }
  return fault->inner.put(fault->inner.user_data, collection, id, record_json);
}

static int tool_executor_fault_get(void *user_data, const char *collection, const char *id,
                                   char **out_record_json) {
  tool_executor_fault_store_t *fault = (tool_executor_fault_store_t *)user_data;
  return fault->inner.get(fault->inner.user_data, collection, id, out_record_json);
}

static int tool_executor_fault_list(void *user_data, const char *collection, const char *filter_key,
                                    const char *filter_value, char **out_records_json) {
  tool_executor_fault_store_t *fault = (tool_executor_fault_store_t *)user_data;
  return fault->inner.list(fault->inner.user_data, collection, filter_key, filter_value,
                           out_records_json);
}

static void tool_executor_fault_destroy(void *user_data) {
  tool_executor_fault_store_t *fault = (tool_executor_fault_store_t *)user_data;
  if (!fault) return;
  if (fault->inner.user_data_free) fault->inner.user_data_free(fault->inner.user_data);
  free(fault);
}

static int tool_executor_transport(const char *request_json, char **out_response_json,
                                   void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static char *tool_executor_strdup(const char *text) {
  size_t length = strlen(text) + 1;
  char *copy = (char *)malloc(length);
  if (copy) memcpy(copy, text, length);
  return copy;
}

static int tool_executor_probe_handler(const char *arguments_json, char **out_output,
                                       void *user_data) {
  tool_executor_probe_t *probe = (tool_executor_probe_t *)user_data;
  salts_mutex_lock(&probe->mutex);
  ++probe->calls;
  ++probe->active;
  if (probe->active > probe->max_active) probe->max_active = probe->active;
  salts_mutex_unlock(&probe->mutex);
  salts_sleep_ms(20);
  *out_output = tool_executor_strdup(arguments_json);
  salts_mutex_lock(&probe->mutex);
  --probe->active;
  salts_mutex_unlock(&probe->mutex);
  return *out_output ? 0 : -1;
}

static json_value_t *tool_executor_state(const char *const *call_ids, const char *const *names,
                                         const char *const *arguments, size_t count,
                                         int malformed_last) {
  json_value_t *state = turbo_agent_state_create();
  json_value_t *events = turbo_json_object_get(state, "events");
  json_value_t *event = turbo_json_create_object();
  json_value_t *calls = turbo_json_create_array();
  size_t index;
  if (!state || !events || !event || !calls) goto fail;
  turbo_json_object_set_string(event, "kind", "model");
  turbo_json_object_set_string(event, "output_text", "");
  for (index = 0; index < count; ++index) {
    json_value_t *call = turbo_json_create_object();
    if (!call) goto fail;
    turbo_json_object_set_string(call, "call_id", call_ids[index]);
    if (!(malformed_last && index + 1 == count)) {
      turbo_json_object_set_string(call, "name", names[index]);
      turbo_json_object_set_string(call, "arguments", arguments[index]);
    }
    turbo_json_array_add(calls, call);
  }
  turbo_json_object_add(event, "tool_calls", calls);
  turbo_json_array_add(events, event);
  return state;
fail:
  turbo_runtime_json_destroy(calls);
  turbo_runtime_json_destroy(event);
  turbo_runtime_json_destroy(state);
  return NULL;
}

static turbo_agent_t *tool_executor_agent(turbo_tool_registry_t *registry) {
  turbo_agent_config_t config = {0};
  config.model = "gpt-5.4";
  config.transport_fn = tool_executor_transport;
  config.tool_registry = registry;
  return turbo_agent_create(&config);
}

spec("turbo agent tool executor") {

  it("should validate a complete batch before the first side effect") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {"probe", "Probe", "{\"type\":\"object\"}",
                                          NULL,    1,       tool_executor_probe_handler,
                                          NULL,    &probe,  NULL};
    turbo_agent_t *agent;
    const char *ids[] = {"one", "two"};
    const char *names[] = {"probe", "probe"};
    const char *args[] = {"{\"n\":1}", "{\"n\":2}"};
    json_value_t *state;
    turbo_graph_exec_ctx_t ctx = {0};

    salts_mutex_init(&probe.mutex);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    agent = tool_executor_agent(registry);
    check_not_null(agent);
    state = tool_executor_state(ids, names, args, 2, 1);
    check_not_null(state);
    ctx.state = state;
    check_int_ne(turbo_agent_tool_node(&ctx, agent), 0);
    check_int_eq(probe.calls, 0);
    check_str_eq(turbo_agent_state_model_error_phase(state), "tool");

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should overlap parallel-safe callbacks and commit outputs in call order") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_v2_t definition = {
        sizeof(turbo_tool_definition_v2_t),
        TURBO_TOOL_DEFINITION_V2_ABI_VERSION,
        {"probe", "Probe", "{\"type\":\"object\"}", NULL, 1, tool_executor_probe_handler, NULL,
         &probe, NULL},
        {TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY}};
    turbo_agent_tool_executor_config_t executor_config;
    turbo_agent_t *agent;
    const char *ids[] = {"one", "two", "three"};
    const char *names[] = {"probe", "probe", "probe"};
    const char *args[] = {"{\"n\":1}", "{\"n\":2}", "{\"n\":3}"};
    json_value_t *state;
    const json_value_t *events;
    const json_value_t *results;
    const json_value_t *outputs;
    turbo_graph_exec_ctx_t ctx = {0};

    salts_mutex_init(&probe.mutex);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add_v2(registry, &definition), TURBO_TOOL_OK);
    agent = tool_executor_agent(registry);
    check_not_null(agent);
    turbo_agent_tool_executor_config_init(&executor_config);
    executor_config.max_workers = 3;
    executor_config.queue_capacity = 3;
    check_int_eq(turbo_agent_tool_executor_configure(agent, &executor_config), SALTS_OK);
    state = tool_executor_state(ids, names, args, 3, 0);
    check_not_null(state);
    ctx.state = state;
    check_int_eq(turbo_agent_tool_node(&ctx, agent), 0);
    check_int_eq(probe.calls, 3);
    check_true(probe.max_active >= 2);
    events = turbo_json_object_get(state, "events");
    results = turbo_json_array_get(events, turbo_json_array_size(events) - 1);
    outputs = turbo_json_object_get(results, "outputs");
    check_size_eq(turbo_json_array_size(outputs), 3);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(outputs, 0), "call_id"), "one");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(outputs, 1), "call_id"), "two");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(outputs, 2), "call_id"), "three");

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should enforce output bounds without committing oversized output") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {"probe", "Probe", "{\"type\":\"object\"}",
                                          NULL,    1,       tool_executor_probe_handler,
                                          NULL,    &probe,  NULL};
    turbo_agent_tool_executor_config_t executor_config;
    turbo_agent_t *agent;
    const char *ids[] = {"one"};
    const char *names[] = {"probe"};
    const char *args[] = {"{\"value\":\"long\"}"};
    json_value_t *state;
    const json_value_t *events;
    const json_value_t *result;
    const char *output;
    turbo_graph_exec_ctx_t ctx = {0};

    salts_mutex_init(&probe.mutex);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    agent = tool_executor_agent(registry);
    check_not_null(agent);
    turbo_agent_tool_executor_config_init(&executor_config);
    executor_config.max_output_bytes = 4;
    check_int_eq(turbo_agent_tool_executor_configure(agent, &executor_config), SALTS_OK);
    state = tool_executor_state(ids, names, args, 1, 0);
    ctx.state = state;
    check_int_eq(turbo_agent_tool_node(&ctx, agent), 0);
    events = turbo_json_object_get(state, "events");
    result =
        turbo_json_array_get(turbo_json_object_get(turbo_json_array_get(events, 1), "outputs"), 0);
    output = turbo_json_get_string(result, "output");
    check_not_null(output);
    check_not_null(strstr(output, "tool_output_limit_exceeded"));

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should reject denied capabilities before the first tool side effect") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    const char *required[] = {"runtime_tools", "network"};
    turbo_tool_definition_v3_t definition = {
        sizeof(turbo_tool_definition_v3_t),
        TURBO_TOOL_DEFINITION_V3_ABI_VERSION,
        {"remote", "Remote", "{\"type\":\"object\"}", NULL, 1, tool_executor_probe_handler, NULL,
         &probe, NULL},
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE},
        required,
        2};
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_t *agent;
    const char *ids[] = {"one"};
    const char *names[] = {"remote"};
    const char *args[] = {"{}"};
    json_value_t *state;
    const json_value_t *events;
    const json_value_t *result;
    const char *output;
    turbo_graph_exec_ctx_t ctx = {0};

    salts_mutex_init(&probe.mutex);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add_v3(registry, &definition), TURBO_TOOL_OK);
    agent = tool_executor_agent(registry);
    check_not_null(agent);
    policy.allow_network = 0;
    check_int_eq(turbo_agent_set_tool_policy(agent, &policy), 0);
    state = tool_executor_state(ids, names, args, 1, 0);
    check_not_null(state);
    ctx.state = state;
    check_int_eq(turbo_agent_tool_node(&ctx, agent), 0);
    check_int_eq(probe.calls, 0);
    check_str_eq(turbo_agent_state_guardrail_rejection_phase(state), "tool_policy");
    check_str_eq(turbo_agent_state_guardrail_rejection_reason(state), "network_disabled");
    events = turbo_json_object_get(state, "events");
    result =
        turbo_json_array_get(turbo_json_object_get(turbo_json_array_get(events, 1), "outputs"), 0);
    output = turbo_json_get_string(result, "output");
    check_not_null(output);
    check_not_null(strstr(output, "network_disabled"));

    turbo_runtime_json_destroy(state);
    turbo_agent_destroy(agent);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should enforce capability policy inside the executor without a workflow precheck") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_tool_executor_t *executor = NULL;
    const char *required[] = {"network"};
    turbo_tool_definition_v3_t definition = {
        sizeof(turbo_tool_definition_v3_t),
        TURBO_TOOL_DEFINITION_V3_ABI_VERSION,
        {"remote", "Remote", "{\"type\":\"object\"}", NULL, 1, tool_executor_probe_handler, NULL,
         &probe, NULL},
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE},
        required,
        1};
    turbo_agent_policy_t policy = turbo_agent_policy_default();
    turbo_agent_tool_execution_t call = {
        "direct-policy-call",
        "remote",
        "{}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    call.turn_key = "0";

    salts_mutex_init(&probe.mutex);
    check_not_null(registry);
    check_int_eq(turbo_tool_registry_add_v3(registry, &definition), TURBO_TOOL_OK);
    check_int_eq(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    policy.allow_network = 0;
    check_int_eq(turbo_agent_tool_executor_execute(executor, NULL, NULL, NULL, NULL, registry,
                                                   &policy, &call, 1),
                 SALTS_OK);
    check_int_eq(probe.calls, 0);
    check_int_eq(call.status, TURBO_TOOL_ERROR);
    check_str_eq(call.policy_reason, "network_disabled");

    free(call.output);
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should observe cancellation before invoking a synchronous callback") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {"probe", "Probe", "{\"type\":\"object\"}",
                                          NULL,    1,       tool_executor_probe_handler,
                                          NULL,    &probe,  NULL};
    turbo_agent_tool_executor_t *executor = NULL;
    turbo_cancel_source_t *source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_agent_tool_execution_t call = {
        "cancel-call",
        "probe",
        "{}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    call.turn_key = "0";

    salts_mutex_init(&probe.mutex);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_int_eq(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    check_int_eq(turbo_cancel_source_create(NULL, &source), SALTS_OK);
    check_int_eq(turbo_cancel_source_token(source, &token), SALTS_OK);
    check_int_eq(turbo_cancel_source_cancel(source, TURBO_CANCEL_USER), SALTS_OK);
    check_int_eq(turbo_agent_tool_executor_execute(executor, NULL, token, NULL, NULL, registry,
                                                   NULL, &call, 1),
                 SALTS_OK);
    check_int_eq(call.status, TURBO_TOOL_CANCELLED);
    check_int_eq(probe.calls, 0);

    free(call.output);
    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(source);
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should not retry a callback after a started journal loses its commit") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {"probe", "Probe", "{\"type\":\"object\"}",
                                          NULL,    1,       tool_executor_probe_handler,
                                          NULL,    &probe,  NULL};
    turbo_agent_tool_executor_t *executor = NULL;
    tool_executor_fault_store_t *fault = (tool_executor_fault_store_t *)calloc(1, sizeof(*fault));
    turbo_agent_runtime_store_t store = {0};
    turbo_agent_runtime_t *runtime;
    turbo_agent_tool_execution_t first = {
        "journal-call",
        "probe",
        "{}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    turbo_agent_tool_execution_t recovered = {
        "journal-call",
        "probe",
        "{}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    first.turn_key = "0";
    recovered.turn_key = "0";

    salts_mutex_init(&probe.mutex);
    check_not_null(fault);
    fault->inner = turbo_agent_runtime_store_memory_create();
    fault->fail_committed_once = 1;
    store.put = tool_executor_fault_put;
    store.get = tool_executor_fault_get;
    store.list = tool_executor_fault_list;
    store.user_data = fault;
    store.user_data_free = tool_executor_fault_destroy;
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_int_eq(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &first, 1),
                 SALTS_OK);
    check_int_eq(fault->puts, 3);
    check_int_eq(fault->committed_seen, 1);
    check_int_eq(first.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_int_eq(probe.calls, 1);
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &recovered, 1),
                 SALTS_OK);
    check_int_eq(recovered.status, TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_int_eq(probe.calls, 1);

    free(first.output);
    free(recovered.output);
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
    salts_mutex_destroy(&probe.mutex);
  }

  it("should replay committed output and reject a changed call identity") {
    tool_executor_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {"probe", "Probe", "{\"type\":\"object\"}",
                                          NULL,    1,       tool_executor_probe_handler,
                                          NULL,    &probe,  NULL};
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_agent_tool_executor_t *executor = NULL;
    turbo_agent_tool_execution_t first = {
        "replay-call",
        "probe",
        "{\"value\":1}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    turbo_agent_tool_execution_t replay = {
        "replay-call",
        "probe",
        "{\"value\":1}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    turbo_agent_tool_execution_t changed = {
        "replay-call",
        "probe",
        "{\"value\":2}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    turbo_agent_tool_execution_t next_turn = {
        "replay-call",
        "probe",
        "{\"value\":1}",
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    first.turn_key = "0";
    replay.turn_key = "0";
    changed.turn_key = "0";
    next_turn.turn_key = "1";

    salts_mutex_init(&probe.mutex);
    check_not_null(runtime);
    check_int_eq(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_int_eq(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &first, 1),
                 SALTS_OK);
    check_int_eq(probe.calls, 1);
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &replay, 1),
                 SALTS_OK);
    check_int_eq(probe.calls, 1);
    check_true(replay.replayed != 0);
    check_str_eq(replay.output, "{\"value\":1}");
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &next_turn, 1),
                 SALTS_OK);
    check_int_eq(probe.calls, 2);
    check_int_eq(turbo_agent_tool_executor_execute(executor, runtime, NULL, "thread", "run",
                                                   registry, NULL, &changed, 1),
                 SALTS_EPROTO);
    check_int_eq(probe.calls, 2);

    free(first.output);
    free(replay.output);
    free(changed.output);
    free(next_turn.output);
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
    salts_mutex_destroy(&probe.mutex);
  }
}
