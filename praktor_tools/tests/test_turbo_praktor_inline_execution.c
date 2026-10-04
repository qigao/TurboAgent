#include "tinytest.h"

#include "turbo_agent_praktor_lowering.h"
#include "turbo_agent_templates.h"
#include "turbo_praktor_tool_pack.h"
#include "turbo_runtime_control.h"

#include <string.h>

typedef enum inline_probe_kind_e {
  INLINE_PROBE_INSPECT = 1,
  INLINE_PROBE_CHANGE = 2,
  INLINE_PROBE_VERIFY = 3
} inline_probe_kind_t;

typedef struct inline_probe_s {
  inline_probe_kind_t kind;
  int calls;
  int verified;
  const char *last_thread_id;
} inline_probe_t;

static turbo_tool_status_t inline_handler(
    const json_value_t *arguments,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result,
    void *user_data) {
  inline_probe_t *probe = (inline_probe_t *)user_data;
  json_value_t *result = NULL;
  (void)arguments;
  if (!probe || !out_result) return TURBO_TOOL_INVALID_ARGUMENT;
  *out_result = NULL;
  ++probe->calls;
  probe->last_thread_id = context ? context->thread_id : NULL;

  result = json_create_object();
  if (!result) return TURBO_TOOL_OUT_OF_MEMORY;
  switch (probe->kind) {
    case INLINE_PROBE_INSPECT:
      json_object_set_bool(result, "observed", true);
      break;
    case INLINE_PROBE_CHANGE:
      json_object_set_bool(result, "changed", true);
      break;
    case INLINE_PROBE_VERIFY:
      json_object_set_bool(result, "verified", probe->verified != 0);
      break;
    default:
      turbo_runtime_json_destroy(result);
      return TURBO_TOOL_ERROR;
  }
  *out_result = result;
  return TURBO_TOOL_OK;
}

static int add_inline_tool(
    turbo_tool_registry_t *registry,
    const char *name,
    turbo_tool_idempotency_t idempotency,
    inline_probe_t *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v4_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = "Inline Praktor execution test tool.";
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1u;
  definition.json_value_context_handler = inline_handler;
  return turbo_tool_registry_add_v4(registry, &definition) == TURBO_TOOL_OK
             ? 0
             : -1;
}

static turbo_tool_registry_t *inline_registry(
    inline_probe_t *inspect,
    inline_probe_t *change,
    inline_probe_t *verify) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  if (!registry) return NULL;
  if (add_inline_tool(
          registry, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          inspect) != 0 ||
      add_inline_tool(
          registry, "repo.patch", TURBO_TOOL_IDEMPOTENCY_NONE,
          change) != 0 ||
      add_inline_tool(
          registry, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
          verify) != 0) {
    turbo_tool_registry_destroy(registry);
    return NULL;
  }
  return registry;
}

static void inline_compiler_config(
    turbo_agent_compiler_config_t *config) {
  static const char *const allowed[] = {"runtime_tools"};
  turbo_agent_compiler_config_init(config);
  config->allowed_capabilities = allowed;
  config->allowed_capability_count = 1u;
}

static void inline_slot(
    turbo_agent_template_slot_t *slot,
    const char *tool,
    const json_value_t *args,
    uint32_t retry_limit) {
  turbo_agent_template_slot_init(slot);
  slot->tool_name = tool;
  slot->arguments = args;
  slot->retry_limit = retry_limit;
}

static turbo_tool_status_t execute_template(
    const turbo_agent_template_plan_t *plan,
    const turbo_tool_registry_t *approved_tools,
    const turbo_tool_execution_context_t *context,
    json_value_t **out_result) {
  turbo_agent_praktor_inline_source_t *source = NULL;
  turbo_praktor_inline_execution_request_t request;
  turbo_tool_status_t status;

  if (out_result) *out_result = NULL;
  if (turbo_agent_template_lower_praktor_inline(
          plan, &source, NULL) != TURBO_AGENT_PRAKTOR_LOWERING_OK ||
      !source) {
    return TURBO_TOOL_ERROR;
  }

  turbo_praktor_inline_execution_request_init(&request);
  request.source_id = turbo_agent_praktor_inline_source_id(source);
  request.workflow_yaml = turbo_agent_praktor_inline_source_yaml(source);
  request.workflow_yaml_size =
      turbo_agent_praktor_inline_source_yaml_size(source);
  request.approved_host_tools = approved_tools;
  request.context = context;
  status = turbo_praktor_execute_inline_workflow(&request, out_result);

  turbo_agent_praktor_inline_source_destroy(source);
  return status;
}

spec("Praktor released inline WorkflowPlan bridge") {
  it("executes Change through released ABI and maps verify pass to COMPLETED") {
    inline_probe_t inspect = {INLINE_PROBE_INSPECT, 0, 0, NULL};
    inline_probe_t change = {INLINE_PROBE_CHANGE, 0, 0, NULL};
    inline_probe_t verify = {INLINE_PROBE_VERIFY, 0, 1, NULL};
    turbo_tool_registry_t *registry =
        inline_registry(&inspect, &change, &verify);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *args = json_create_object();
    json_value_t *result = NULL;
    turbo_agent_verify_status_t verify_status;

    check_not_null(registry);
    check_not_null(args);
    inline_compiler_config(&config);
    turbo_agent_change_source_init(&source);
    inline_slot(&source.inspect, "repo.inspect", args, 0u);
    inline_slot(&source.change, "repo.patch", args, 0u);
    inline_slot(&source.verify, "repo.verify", args, 0u);

    check_equal(turbo_agent_compile_change_template(
                    &config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_not_null(plan);

    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.thread_id = "thread-inline";
    context.run_id = "run-inline";
    context.turn_id = "turn-inline";
    context.tool_call_id = "call-inline";

    check_equal(
        execute_template(
            plan, turbo_agent_template_plan_approved_tools(plan),
            &context, &result),
        TURBO_TOOL_OK);
    check_not_null(result);
    verify_status =
        turbo_agent_template_verify_status_from_praktor_result(result);
    check_equal(verify_status, TURBO_AGENT_VERIFY_PASSED);
    check_equal(
        turbo_agent_template_finish_verify(plan, verify_status),
        TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(inspect.calls, 1);
    check_equal(change.calls, 1);
    check_equal(verify.calls, 1);
    check_equal(verify.last_thread_id, "thread-inline");

    turbo_runtime_json_destroy(result);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("returns REPLAN_REQUIRED for Repair semantic verify failure") {
    inline_probe_t inspect = {INLINE_PROBE_INSPECT, 0, 0, NULL};
    inline_probe_t change = {INLINE_PROBE_CHANGE, 0, 0, NULL};
    inline_probe_t verify = {INLINE_PROBE_VERIFY, 0, 0, NULL};
    turbo_tool_registry_t *registry =
        inline_registry(&inspect, &change, &verify);
    turbo_agent_compiler_config_t config;
    turbo_agent_repair_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args = json_create_object();
    json_value_t *result = NULL;
    turbo_agent_verify_status_t verify_status;

    inline_compiler_config(&config);
    turbo_agent_repair_source_init(&source);
    source.plan_version = 1u;
    source.max_replans = 2u;
    inline_slot(&source.diagnose, "repo.inspect", args, 0u);
    inline_slot(&source.change, "repo.patch", args, 0u);
    inline_slot(&source.verify, "repo.verify", args, 0u);

    check_equal(turbo_agent_compile_repair_template(
                    &config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(
        execute_template(
            plan, turbo_agent_template_plan_approved_tools(plan),
            NULL, &result),
        TURBO_TOOL_OK);
    check_not_null(result);
    verify_status =
        turbo_agent_template_verify_status_from_praktor_result(result);
    check_equal(verify_status, TURBO_AGENT_VERIFY_SEMANTIC_FAILURE);
    check_equal(
        turbo_agent_template_finish_verify(plan, verify_status),
        TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_REQUIRED);
    check_equal(inspect.calls, 1);
    check_equal(change.calls, 1);
    check_equal(verify.calls, 1);

    turbo_runtime_json_destroy(result);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("preflights every HostTool before DAG side effects") {
    inline_probe_t inspect = {INLINE_PROBE_INSPECT, 0, 0, NULL};
    inline_probe_t change = {INLINE_PROBE_CHANGE, 0, 0, NULL};
    inline_probe_t verify = {INLINE_PROBE_VERIFY, 0, 1, NULL};
    turbo_tool_registry_t *registry =
        inline_registry(&inspect, &change, &verify);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    turbo_tool_registry_t *incomplete = NULL;
    const char *names[] = {"repo.inspect", "repo.verify"};
    json_value_t *args = json_create_object();
    json_value_t *result = NULL;

    inline_compiler_config(&config);
    turbo_agent_change_source_init(&source);
    inline_slot(&source.inspect, "repo.inspect", args, 0u);
    inline_slot(&source.change, "repo.patch", args, 0u);
    inline_slot(&source.verify, "repo.verify", args, 0u);
    check_equal(turbo_agent_compile_change_template(
                    &config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(
        turbo_tool_registry_project(registry, names, 2u, &incomplete),
        TURBO_TOOL_OK);
    check_not_null(incomplete);

    check_equal(
        execute_template(plan, incomplete, NULL, &result),
        TURBO_TOOL_ERROR);
    check_null(result);
    check_equal(inspect.calls, 0);
    check_equal(change.calls, 0);
    check_equal(verify.calls, 0);

    turbo_tool_registry_destroy(incomplete);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("fails closed when finite retry exceeds released Praktor hard cap") {
    inline_probe_t inspect = {INLINE_PROBE_INSPECT, 0, 0, NULL};
    inline_probe_t change = {INLINE_PROBE_CHANGE, 0, 0, NULL};
    inline_probe_t verify = {INLINE_PROBE_VERIFY, 0, 1, NULL};
    turbo_tool_registry_t *registry =
        inline_registry(&inspect, &change, &verify);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    json_value_t *args = json_create_object();
    json_value_t *result = NULL;

    inline_compiler_config(&config);
    turbo_agent_change_source_init(&source);
    inline_slot(&source.inspect, "repo.inspect", args, 1025u);
    inline_slot(&source.change, "repo.patch", args, 0u);
    inline_slot(&source.verify, "repo.verify", args, 0u);
    check_equal(turbo_agent_compile_change_template(
                    &config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);

    check_equal(
        execute_template(
            plan, turbo_agent_template_plan_approved_tools(plan),
            NULL, &result),
        TURBO_TOOL_ERROR);
    check_null(result);
    check_equal(inspect.calls, 0);
    check_equal(change.calls, 0);
    check_equal(verify.calls, 0);

    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }

  it("propagates cancellation before inline execution starts") {
    inline_probe_t inspect = {INLINE_PROBE_INSPECT, 0, 0, NULL};
    inline_probe_t change = {INLINE_PROBE_CHANGE, 0, 0, NULL};
    inline_probe_t verify = {INLINE_PROBE_VERIFY, 0, 1, NULL};
    turbo_tool_registry_t *registry =
        inline_registry(&inspect, &change, &verify);
    turbo_agent_compiler_config_t config;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *plan = NULL;
    turbo_cancel_source_t *cancel_source = NULL;
    turbo_cancel_token_t *token = NULL;
    turbo_tool_execution_context_t context = {0};
    json_value_t *args = json_create_object();
    json_value_t *result = NULL;

    inline_compiler_config(&config);
    turbo_agent_change_source_init(&source);
    inline_slot(&source.inspect, "repo.inspect", args, 0u);
    inline_slot(&source.change, "repo.patch", args, 0u);
    inline_slot(&source.verify, "repo.verify", args, 0u);
    check_equal(turbo_agent_compile_change_template(
                    &config, registry, &source, &plan, NULL),
                TURBO_AGENT_COMPILE_OK);

    check_equal(turbo_cancel_source_create(NULL, &cancel_source), 0);
    check_equal(turbo_cancel_source_token(cancel_source, &token), 0);
    check_equal(
        turbo_cancel_source_cancel(cancel_source, TURBO_CANCEL_USER), 0);
    context.struct_size = sizeof(context);
    context.abi_version = TURBO_TOOL_EXECUTION_CONTEXT_ABI_VERSION;
    context.cancel_token = token;

    check_equal(
        execute_template(
            plan, turbo_agent_template_plan_approved_tools(plan),
            &context, &result),
        TURBO_TOOL_CANCELLED);
    check_null(result);
    check_equal(inspect.calls, 0);
    check_equal(change.calls, 0);
    check_equal(verify.calls, 0);

    turbo_cancel_token_release(token);
    turbo_cancel_source_destroy(cancel_source);
    turbo_agent_template_plan_destroy(plan);
    turbo_runtime_json_destroy(args);
    turbo_tool_registry_destroy(registry);
  }
}
