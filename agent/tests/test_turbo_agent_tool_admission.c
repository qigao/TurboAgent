#include "tinytest.h"
#include "../src/turbo_agent_tool_executor_internal.h"
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
}
