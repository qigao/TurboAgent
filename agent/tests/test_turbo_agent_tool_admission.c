#include "tinytest.h"
#include "../src/turbo_agent_tool_executor_internal.h"
#include "turbo_agent_graph.h"
#include "turbo_runtime_store.h"
#include "turbo_tool_registry.h"

#include <stdlib.h>
#include <string.h>

/* Unlike schema validation at compile time, the exploratory Agent path
 * historically dispatched textual arguments. Verify that malformed or
 * non-object model output is rejected before the *first* tool/Journaling
 * side effect, including when it occurs in the second call of a batch. */
typedef struct admission_probe_s {
  int callbacks;
  int canonical_guardrails;
} admission_probe_t;

static int admission_tool_handler(const char *arguments_json, char **out,
                                  void *user_data) {
  admission_probe_t *probe = (admission_probe_t *)user_data;
  (void)arguments_json;
  ++probe->callbacks;
  *out = malloc(3);
  if (!*out) return -1;
  memcpy(*out, "ok", 3);
  return 0;
}

static int admission_model_transport(const char *request, char **response, void *user) {
  (void)request;
  (void)response;
  (void)user;
  return -1;
}

static int admission_canonical_guardrail(
    turbo_agent_t *agent, const json_value_t *state, const char *id,
    const char *tool_name, const char *arguments, const char **reason, void *user) {
  admission_probe_t *probe = (admission_probe_t *)user;
  (void)agent;
  (void)state;
  (void)id;
  (void)arguments;
  if (reason) *reason = NULL;
  if (strcmp(tool_name, "admission.probe") != 0) {
    if (reason) *reason = "uncanonical_tool_name";
    return -1;
  }
  ++probe->canonical_guardrails;
  return 0;
}

static turbo_agent_tool_execution_t admission_call(const char *id, const char *arguments) {
  turbo_agent_tool_execution_t call = {0};
  call.call_id = id;
  call.tool_name = "admission_probe";
  call.turn_key = "0";
  call.arguments_json = arguments;
  call.policy = (turbo_tool_execution_policy_t){
      TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE};
  return call;
}

spec("model tool argument structure before durable admission") {
  it("rejects any non-object or malformed batch member with zero callbacks and journal writes") {
    const char *invalid[] = {"[]", "null", "\"string\"", "42", "{\"unfinished\":"};
    admission_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_tool_executor_t *executor = NULL;
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_tool_definition_t definition = {
        "admission_probe", "Bounded admission fixture", "{\"type\":\"object\"}",
        NULL, 0, admission_tool_handler, NULL, &probe, NULL};
    json_value_t *records = NULL;
    turbo_agent_tool_execution_t calls[2];

    check_not_null(registry);
    check_not_null(runtime);
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      calls[0] = admission_call("first", "{\"ok\":true}");
      calls[1] = admission_call("second", invalid[i]);
      check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
          "thread", "run", NULL, NULL, registry, NULL, calls, 2), SALTS_EPROTO);
      check_equal(probe.callbacks, 0);
      check_null(calls[0].output);
      check_null(calls[1].output);
      check_equal(turbo_agent_runtime_store_list_json(
          runtime, "agent_tool_journal", "run_id", "run", &records), SALTS_OK);
      check_equal(json_array_size(records), 0);
      json_free(records);
      records = NULL;
    }

    calls[0] = admission_call("first", "{\"ok\":true}");
    calls[1] = admission_call("second", "{\"nested\":{\"value\":7}}");
    check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
        "thread", "run", NULL, NULL, registry, NULL, calls, 2), SALTS_OK);
    check_equal(probe.callbacks, 2);
    check_equal(calls[0].status, TURBO_TOOL_OK);
    check_equal(calls[1].status, TURBO_TOOL_OK);
    check_equal(calls[0].output, "ok");
    check_equal(calls[1].output, "ok");
    free(calls[0].output);
    free(calls[1].output);

    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
  }

  it("uses the canonical tool and registered execution policy for journal identity") {
    admission_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_tool_executor_t *executor = NULL;
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_tool_definition_v2_t definition = {
        sizeof(turbo_tool_definition_v2_t), TURBO_TOOL_DEFINITION_V2_ABI_VERSION,
        {"admission.probe", "Canonical fixture", "{\"type\":\"object\"}",
         NULL, 0, admission_tool_handler, NULL, &probe, NULL},
        {TURBO_TOOL_EXECUTION_SEQUENTIAL, TURBO_TOOL_IDEMPOTENCY_NONE}};
    turbo_agent_tool_execution_t first = admission_call("same-call", "{}");
    turbo_agent_tool_execution_t replay = admission_call("same-call", "{}");
    json_value_t *records = NULL;
    const json_value_t *journal;

    check_not_null(registry);
    check_not_null(runtime);
    check_equal(turbo_tool_registry_add_v2(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);

    first.tool_name = "admission_probe"; /* OpenAI-compatible model-facing alias */
    first.policy = (turbo_tool_execution_policy_t){
        TURBO_TOOL_EXECUTION_PARALLEL_SAFE, TURBO_TOOL_IDEMPOTENCY_READ_ONLY};
    check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
        "thread", "run", NULL, NULL, registry, NULL, &first, 1), SALTS_OK);
    check_equal(first.tool_name, "admission.probe");
    check_equal(first.policy.mode, TURBO_TOOL_EXECUTION_SEQUENTIAL);
    check_equal(first.policy.idempotency, TURBO_TOOL_IDEMPOTENCY_NONE);
    check_equal(probe.callbacks, 1);

    check_equal(turbo_agent_runtime_store_list_json(
        runtime, "agent_tool_journal", "run_id", "run", &records), SALTS_OK);
    check_equal(json_array_size(records), 1);
    journal = json_array_get(records, 0);
    check_equal(json_get_string(journal, "tool_name"), "admission.probe");
    check_equal(turbo_runtime_json_value_as_int64(
        json_object_get(journal, "execution_mode"), -1), TURBO_TOOL_EXECUTION_SEQUENTIAL);
    json_free(records);

    replay.tool_name = "admission.probe";
    check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
        "thread", "run", NULL, NULL, registry, NULL, &replay, 1), SALTS_OK);
    check_true(replay.replayed);
    check_equal(replay.output, "ok");
    check_equal(probe.callbacks, 1);

    free(first.output);
    free(replay.output);
    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
  }

  it("rejects ambiguous sanitized model tool aliases without any side effects") {
    admission_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_tool_executor_t *executor = NULL;
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    turbo_tool_definition_t dotted = {
        "admission.probe", "Dotted", "{\"type\":\"object\"}",
        NULL, 0, admission_tool_handler, NULL, &probe, NULL};
    turbo_tool_definition_t slashed = dotted;
    turbo_agent_tool_execution_t call = admission_call("ambiguous", "{}");
    json_value_t *records = NULL;
    slashed.name = "admission/probe";
    slashed.description = "Slashed";
    call.tool_name = "admission_probe";

    check_not_null(registry);
    check_not_null(runtime);
    check_equal(turbo_tool_registry_add(registry, &dotted), TURBO_TOOL_OK);
    check_equal(turbo_tool_registry_add(registry, &slashed), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
    check_equal(turbo_agent_tool_executor_execute(executor, runtime, NULL,
        "thread", "run", NULL, NULL, registry, NULL, &call, 1), SALTS_OK);
    check_equal(call.status, TURBO_TOOL_NOT_FOUND);
    check_equal(probe.callbacks, 0);
    check_equal(turbo_agent_runtime_store_list_json(
        runtime, "agent_tool_journal", "run_id", "run", &records), SALTS_OK);
    check_equal(json_array_size(records), 0);
    json_free(records);

    turbo_agent_tool_executor_destroy(executor);
    turbo_tool_registry_destroy(registry);
    turbo_agent_runtime_destroy(runtime);
  }

  it("passes canonical registry tool identity to Workflow Guardrails") {
    static const char model_event[] =
        "{\"kind\":\"model\",\"output_text\":\"\",\"tool_calls\":["
        "{\"call_id\":\"alias-call\",\"name\":\"admission_probe\","
        "\"arguments\":\"{}\"}]}";
    admission_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_tool_definition_t definition = {
        "admission.probe", "Wire-alias fixture", "{\"type\":\"object\"}",
        NULL, 0, admission_tool_handler, NULL, &probe, NULL};
    turbo_agent_config_t config = {0};
    turbo_agent_guardrail_t guardrail = {0};
    turbo_agent_t *agent;
    turbo_graph_exec_ctx_t ctx = {0};
    json_value_t *state = turbo_agent_state_create();
    json_value_t *event;

    check_not_null(registry);
    check_not_null(state);
    check_equal(turbo_tool_registry_add(registry, &definition), TURBO_TOOL_OK);
    config.model = "test";
    config.transport_fn = admission_model_transport;
    config.tool_registry = registry;
    agent = turbo_agent_create(&config);
    check_not_null(agent);
    guardrail.before_tool = admission_canonical_guardrail;
    guardrail.user_data = &probe;
    check_equal(turbo_agent_add_guardrail(agent, &guardrail), 0);
    event = json_parse(model_event, sizeof(model_event) - 1);
    check_not_null(event);
    json_array_add(json_object_get(state, "events"), event);
    ctx.state = state;

    check_equal(turbo_agent_tool_node(&ctx, agent), 0);
    check_equal(probe.canonical_guardrails, 1);
    check_equal(probe.callbacks, 1);

    json_free(state);
    turbo_agent_destroy(agent);
    turbo_tool_registry_destroy(registry);
  }
}
