#include "turbo_agent_templates.h"

#include <string.h>

static turbo_agent_compile_status_t template_fail(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  if (diagnostic) {
    memset(diagnostic, 0, sizeof(*diagnostic));
    diagnostic->status = status;
    if (message) {
      size_t length = strlen(message);
      if (length >= sizeof(diagnostic->message)) {
        length = sizeof(diagnostic->message) - 1u;
      }
      memcpy(diagnostic->message, message, length);
      diagnostic->message[length] = '\0';
    }
  }
  return status;
}

static int template_tool_source_valid(
    const turbo_agent_template_tool_source_t *source) {
  return source &&
         source->struct_size == sizeof(*source) &&
         source->abi_version == TURBO_AGENT_TEMPLATE_TOOL_SOURCE_ABI_VERSION &&
         source->tool_name && source->tool_name[0] &&
         source->arguments && json_type(source->arguments) == JSON_OBJECT;
}

static turbo_agent_compile_status_t template_role_policy(
    const turbo_tool_registry_t *registry,
    const turbo_agent_template_tool_source_t *source,
    int require_read_only,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  const char *canonical_name = NULL;
  turbo_tool_execution_policy_t policy = {0};

  if (!registry || !template_tool_source_valid(source)) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid template tool source");
  }
  if (turbo_tool_registry_resolve_name(
          registry, source->tool_name, &canonical_name) != TURBO_TOOL_OK ||
      !canonical_name ||
      turbo_tool_registry_get_execution_policy(
          registry, canonical_name, &policy) != TURBO_TOOL_OK) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
        "template tool cannot be resolved during compilation");
  }

  if (require_read_only &&
      policy.idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "template read/verify role requires a READ_ONLY tool");
  }
  if (!require_read_only &&
      policy.idempotency == TURBO_TOOL_IDEMPOTENCY_READ_ONLY) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "template change role requires a mutating tool");
  }
  return TURBO_AGENT_COMPILE_OK;
}

static void template_dag_step(
    turbo_agent_dag_step_source_t *step,
    const char *step_id,
    const turbo_agent_template_tool_source_t *source,
    const char *const *depends_on,
    size_t dependency_count,
    uint32_t flags) {
  turbo_agent_dag_step_source_init(step);
  step->step_id = step_id;
  step->tool_name = source->tool_name;
  step->arguments = source->arguments;
  step->depends_on = depends_on;
  step->dependency_count = dependency_count;
  step->retry_limit = source->retry_limit;
  step->flags = flags;
}

void turbo_agent_template_tool_source_init(
    turbo_agent_template_tool_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_TEMPLATE_TOOL_SOURCE_ABI_VERSION;
}

void turbo_agent_change_source_init(turbo_agent_change_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION;
  turbo_agent_template_tool_source_init(&source->inspect);
  turbo_agent_template_tool_source_init(&source->change);
  turbo_agent_template_tool_source_init(&source->verify);
}

void turbo_agent_repair_source_init(turbo_agent_repair_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION;
  turbo_agent_template_tool_source_init(&source->diagnose);
  turbo_agent_template_tool_source_init(&source->change);
  turbo_agent_template_tool_source_init(&source->verify);
}

turbo_agent_compile_status_t turbo_agent_compile_change_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_change_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  static const char *const change_dep[] = {"inspect"};
  static const char *const verify_dep[] = {"change"};
  turbo_agent_dag_step_source_t steps[3];
  turbo_agent_dag_source_t dag;
  turbo_agent_compile_status_t status;

  if (out_plan) *out_plan = NULL;
  if (!source || source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION ||
      !template_tool_source_valid(&source->inspect) ||
      !template_tool_source_valid(&source->change) ||
      !template_tool_source_valid(&source->verify)) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid Change template source");
  }

  status = template_role_policy(
      source_registry, &source->inspect, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_role_policy(
      source_registry, &source->change, 0, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_role_policy(
      source_registry, &source->verify, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;

  template_dag_step(
      &steps[0], "inspect", &source->inspect, NULL, 0u,
      TURBO_AGENT_DAG_STEP_NONE);
  template_dag_step(
      &steps[1], "change", &source->change, change_dep, 1u,
      TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
          TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);
  template_dag_step(
      &steps[2], "verify", &source->verify, verify_dep, 1u,
      TURBO_AGENT_DAG_STEP_NONE);

  turbo_agent_dag_source_init(&dag);
  dag.steps = steps;
  dag.step_count = 3u;
  dag.replan_budget = 0u;
  dag.template_kind = TURBO_AGENT_TEMPLATE_CHANGE;
  dag.plan_generation = source->plan_generation;

  return turbo_agent_compile_dag(
      config, source_registry, &dag, out_plan, diagnostic);
}

turbo_agent_compile_status_t turbo_agent_compile_repair_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_repair_source_t *source,
    turbo_agent_executable_dag_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  static const char *const change_dep[] = {"diagnose"};
  static const char *const verify_dep[] = {"change"};
  turbo_agent_dag_step_source_t steps[3];
  turbo_agent_dag_source_t dag;
  turbo_agent_compile_status_t status;

  if (out_plan) *out_plan = NULL;
  if (!source || source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION ||
      !template_tool_source_valid(&source->diagnose) ||
      !template_tool_source_valid(&source->change) ||
      !template_tool_source_valid(&source->verify)) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid Repair template source");
  }
  if (source->plan_generation > source->replan_budget) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "Repair plan generation exceeds the finite replan budget");
  }

  status = template_role_policy(
      source_registry, &source->diagnose, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_role_policy(
      source_registry, &source->change, 0, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_role_policy(
      source_registry, &source->verify, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;

  template_dag_step(
      &steps[0], "diagnose", &source->diagnose, NULL, 0u,
      TURBO_AGENT_DAG_STEP_NONE);
  template_dag_step(
      &steps[1], "change", &source->change, change_dep, 1u,
      TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
          TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);
  template_dag_step(
      &steps[2], "verify", &source->verify, verify_dep, 1u,
      TURBO_AGENT_DAG_STEP_NONE);

  turbo_agent_dag_source_init(&dag);
  dag.steps = steps;
  dag.step_count = 3u;
  dag.replan_budget = source->replan_budget;
  dag.template_kind = TURBO_AGENT_TEMPLATE_REPAIR;
  dag.plan_generation = source->plan_generation;

  return turbo_agent_compile_dag(
      config, source_registry, &dag, out_plan, diagnostic);
}

turbo_agent_repair_outcome_t turbo_agent_repair_classify_verify_result(
    const json_value_t *verify_result) {
  const json_value_t *verified;
  if (!verify_result || json_type(verify_result) != JSON_OBJECT) {
    return TURBO_AGENT_REPAIR_OUTCOME_INVALID;
  }
  verified = json_object_get(verify_result, "verified");
  if (!verified || json_type(verified) != JSON_BOOL) {
    return TURBO_AGENT_REPAIR_OUTCOME_INVALID;
  }
  return json_bool(verified)
             ? TURBO_AGENT_REPAIR_OUTCOME_COMPLETED
             : TURBO_AGENT_REPAIR_OUTCOME_REPLAN_REQUIRED;
}
