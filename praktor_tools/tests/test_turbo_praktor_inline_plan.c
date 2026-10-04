#include "tinytest.h"
#include "turbo_praktor_tool_pack.h"
#include "turbo_agent_praktor_lowering.h"
#include "turbo_agent_templates.h"
#include "turbo_runtime_json.h"

typedef struct bridge_probe_s {
  int inspect_calls;
  int change_calls;
  int verify_calls;
} bridge_probe_t;

static turbo_tool_status_t inspect_cb(
    const json_value_t *args,
    const turbo_tool_execution_context_t *ctx,
    json_value_t **out,
    void *user_data) {
  bridge_probe_t *p = (bridge_probe_t *)user_data;
  (void)args; (void)ctx;
  ++p->inspect_calls;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t change_cb(
    const json_value_t *args,
    const turbo_tool_execution_context_t *ctx,
    json_value_t **out,
    void *user_data) {
  bridge_probe_t *p = (bridge_probe_t *)user_data;
  (void)args; (void)ctx;
  ++p->change_calls;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t verify_cb(
    const json_value_t *args,
    const turbo_tool_execution_context_t *ctx,
    json_value_t **out,
    void *user_data) {
  bridge_probe_t *p = (bridge_probe_t *)user_data;
  (void)args; (void)ctx;
  ++p->verify_calls;
  if (p->verify_calls < 3) return TURBO_TOOL_ERROR;
  *out = json_create_object();
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  json_object_set_bool(*out, "verified", true);
  return TURBO_TOOL_OK;
}

static int add_tool(turbo_tool_registry_t *r, const char *name,
                    turbo_tool_idempotency_t idem,
                    turbo_tool_json_value_context_handler_fn cb,
                    bridge_probe_t *probe) {
  static const char *caps[] = {"runtime_tools"};
  turbo_tool_definition_v4_t d = {0};
  d.struct_size = sizeof(d);
  d.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  d.definition.name = name;
  d.definition.description = name;
  d.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  d.definition.strict = 1;
  d.definition.user_data = probe;
  d.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  d.execution_policy.idempotency = idem;
  d.required_capabilities = caps;
  d.required_capability_count = 1u;
  d.json_value_context_handler = cb;
  return turbo_tool_registry_add_v4(r, &d) == TURBO_TOOL_OK ? 0 : -1;
}

spec("released Praktor inline TemplatePlan bridge") {
  it("executes Change through inline WorkflowPlan and classifies verify") {
    bridge_probe_t probe = {0};
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_agent_compiler_config_t compiler;
    turbo_agent_change_source_t source;
    turbo_agent_template_plan_t *template_plan = NULL;
    turbo_agent_praktor_inline_source_t *lowered = NULL;
    turbo_praktor_inline_plan_config_t inline_config;
    turbo_praktor_inline_plan_t *inline_plan = NULL;
    json_value_t *empty = json_create_object();
    json_value_t *result = NULL;
    const char *allowed[] = {"runtime_tools"};

    check_not_null(registry);
    check_not_null(empty);
    check_equal(add_tool(registry, "repo.inspect",
                         TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         inspect_cb, &probe), 0);
    check_equal(add_tool(registry, "repo.patch",
                         TURBO_TOOL_IDEMPOTENCY_NONE,
                         change_cb, &probe), 0);
    check_equal(add_tool(registry, "repo.verify",
                         TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         verify_cb, &probe), 0);

    turbo_agent_compiler_config_init(&compiler);
    compiler.allowed_capabilities = allowed;
    compiler.allowed_capability_count = 1u;
    turbo_agent_change_source_init(&source);
    source.inspect.tool_name = "repo.inspect";
    source.inspect.arguments = empty;
    source.change.tool_name = "repo.patch";
    source.change.arguments = empty;
    source.verify.tool_name = "repo.verify";
    source.verify.arguments = empty;
    source.verify.retry_limit = 2u;

    check_equal(turbo_agent_compile_change_template(
                    &compiler, registry, &source, &template_plan, NULL),
                TURBO_AGENT_COMPILE_OK);
    check_equal(turbo_agent_template_lower_praktor_inline(
                    template_plan, &lowered, NULL),
                TURBO_AGENT_PRAKTOR_LOWERING_OK);

    turbo_praktor_inline_plan_config_init(&inline_config);
    inline_config.source_id = turbo_agent_praktor_inline_source_id(lowered);
    inline_config.workflow_yaml = turbo_agent_praktor_inline_source_yaml(lowered);
    inline_config.workflow_yaml_size =
        turbo_agent_praktor_inline_source_yaml_size(lowered);
    inline_config.approved_host_tools =
        turbo_agent_template_plan_approved_tools(template_plan);

    check_equal(turbo_praktor_inline_plan_compile(
                    &inline_config, &inline_plan),
                TURBO_TOOL_OK);
    check_not_null(inline_plan);
    check_equal(turbo_praktor_inline_plan_execute(
                    inline_plan, NULL, &result),
                TURBO_TOOL_OK);
    check_not_null(result);
    check_equal(probe.inspect_calls, 1);
    check_equal(probe.change_calls, 1);
    check_equal(probe.verify_calls, 3);
    check_equal(turbo_agent_template_finish_praktor_result(
                    template_plan, result),
                TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);

    turbo_runtime_json_destroy(result);
    turbo_praktor_inline_plan_destroy(inline_plan);
    turbo_agent_praktor_inline_source_destroy(lowered);
    turbo_agent_template_plan_destroy(template_plan);
    turbo_runtime_json_destroy(empty);
    turbo_tool_registry_destroy(registry);
  }
}
