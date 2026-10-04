#include "turbo_agent_templates.h"

#include "turbo_runtime_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct turbo_agent_template_plan_s {
  turbo_agent_template_kind_t kind;
  uint32_t template_version;
  uint32_t plan_version;
  uint32_t max_replans;
  turbo_agent_executable_dag_t *dag;
};

static void template_diag(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  if (!diagnostic) return;
  memset(diagnostic, 0, sizeof(*diagnostic));
  diagnostic->status = status;
  if (message) {
    size_t n = strlen(message);
    if (n >= sizeof(diagnostic->message)) {
      n = sizeof(diagnostic->message) - 1u;
    }
    memcpy(diagnostic->message, message, n);
    diagnostic->message[n] = '\0';
  }
}

static turbo_agent_compile_status_t template_fail(
    turbo_agent_compile_diagnostic_t *diagnostic,
    turbo_agent_compile_status_t status,
    const char *message) {
  template_diag(diagnostic, status, message);
  return status;
}

static int template_slot_valid(const turbo_agent_template_slot_t *slot) {
  return slot &&
         slot->struct_size == sizeof(*slot) &&
         slot->abi_version == TURBO_AGENT_TEMPLATE_SLOT_ABI_VERSION &&
         slot->tool_name && slot->tool_name[0] &&
         slot->arguments && json_type(slot->arguments) == JSON_OBJECT;
}

static int template_tool_policy(
    const turbo_tool_registry_t *registry,
    const char *tool_name,
    turbo_tool_execution_policy_t *out) {
  const char *canonical = NULL;
  if (!registry || !tool_name || !out ||
      turbo_tool_registry_resolve_name(
          registry, tool_name, &canonical) != TURBO_TOOL_OK ||
      !canonical ||
      turbo_tool_registry_get_execution_policy(
          registry, canonical, out) != TURBO_TOOL_OK) {
    return 0;
  }
  return 1;
}

static turbo_agent_compile_status_t template_validate_slot_policy(
    const turbo_tool_registry_t *registry,
    const turbo_agent_template_slot_t *slot,
    int require_read_only,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_tool_execution_policy_t policy = {0};
  if (!template_tool_policy(registry, slot->tool_name, &policy)) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_UNRESOLVED_TOOL,
        "template slot tool cannot be resolved");
  }
  if (require_read_only) {
    if (policy.idempotency != TURBO_TOOL_IDEMPOTENCY_READ_ONLY) {
      return template_fail(
          diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
          "read-only template slot requires a READ_ONLY tool");
    }
  } else if (policy.idempotency == TURBO_TOOL_IDEMPOTENCY_READ_ONLY) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_TEMPLATE_VIOLATION,
        "change template slot must not use a READ_ONLY tool");
  }
  return TURBO_AGENT_COMPILE_OK;
}

static void template_dag_step(
    turbo_agent_dag_step_source_t *step,
    const char *step_id,
    const turbo_agent_template_slot_t *slot,
    const char *const *depends_on,
    size_t dependency_count,
    uint32_t flags) {
  turbo_agent_dag_step_source_init(step);
  step->step_id = step_id;
  step->tool_name = slot->tool_name;
  step->arguments = slot->arguments;
  step->depends_on = depends_on;
  step->dependency_count = dependency_count;
  step->retry_limit = slot->retry_limit;
  step->flags = flags;
}

static turbo_agent_dag_template_kind_t template_dag_kind(
    turbo_agent_template_kind_t kind) {
  switch (kind) {
    case TURBO_AGENT_TEMPLATE_CHANGE:
      return TURBO_AGENT_DAG_TEMPLATE_CHANGE;
    case TURBO_AGENT_TEMPLATE_REPAIR:
      return TURBO_AGENT_DAG_TEMPLATE_REPAIR;
    default:
      return TURBO_AGENT_DAG_TEMPLATE_GENERIC;
  }
}

static turbo_agent_compile_status_t template_compile_three_step(
    turbo_agent_template_kind_t kind,
    uint32_t plan_version,
    uint32_t max_replans,
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *registry,
    const char *first_id,
    const turbo_agent_template_slot_t *first,
    const turbo_agent_template_slot_t *change,
    const turbo_agent_template_slot_t *verify,
    turbo_agent_template_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  turbo_agent_dag_step_source_t steps[3];
  turbo_agent_dag_source_t source;
  turbo_agent_executable_dag_t *dag = NULL;
  turbo_agent_template_plan_t *plan = NULL;
  const char *change_dep[1];
  const char *verify_dep[1];
  turbo_agent_compile_status_t status;

  if (out_plan) *out_plan = NULL;
  template_diag(diagnostic, TURBO_AGENT_COMPILE_OK, "");

  if (!config || !registry || !out_plan ||
      plan_version == 0u ||
      !template_slot_valid(first) ||
      !template_slot_valid(change) ||
      !template_slot_valid(verify)) {
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid template source");
  }

  status = template_validate_slot_policy(
      registry, first, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_validate_slot_policy(
      registry, change, 0, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;
  status = template_validate_slot_policy(
      registry, verify, 1, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;

  change_dep[0] = first_id;
  verify_dep[0] = "change";
  template_dag_step(
      &steps[0], first_id, first, NULL, 0u,
      TURBO_AGENT_DAG_STEP_NONE);
  template_dag_step(
      &steps[1], "change", change, change_dep, 1u,
      TURBO_AGENT_DAG_STEP_APPROVAL_BEFORE |
          TURBO_AGENT_DAG_STEP_CHECKPOINT_AFTER);
  template_dag_step(
      &steps[2], "verify", verify, verify_dep, 1u,
      TURBO_AGENT_DAG_STEP_NONE);

  turbo_agent_dag_source_init(&source);
  source.steps = steps;
  source.step_count = 3u;
  source.replan_budget = max_replans;
  source.template_kind = template_dag_kind(kind);
  source.plan_generation = plan_version;

  status = turbo_agent_compile_dag(
      config, registry, &source, &dag, diagnostic);
  if (status != TURBO_AGENT_COMPILE_OK) return status;

  plan = (turbo_agent_template_plan_t *)calloc(1, sizeof(*plan));
  if (!plan) {
    turbo_agent_executable_dag_destroy(dag);
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_OUT_OF_MEMORY,
        "could not allocate template plan");
  }
  plan->kind = kind;
  plan->template_version = 1u;
  plan->plan_version = plan_version;
  plan->max_replans = max_replans;
  plan->dag = dag;

  *out_plan = plan;
  template_diag(diagnostic, TURBO_AGENT_COMPILE_OK, "ok");
  return TURBO_AGENT_COMPILE_OK;
}

void turbo_agent_template_slot_init(turbo_agent_template_slot_t *slot) {
  if (!slot) return;
  memset(slot, 0, sizeof(*slot));
  slot->struct_size = sizeof(*slot);
  slot->abi_version = TURBO_AGENT_TEMPLATE_SLOT_ABI_VERSION;
}

void turbo_agent_change_source_init(turbo_agent_change_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION;
  source->plan_version = 1u;
  turbo_agent_template_slot_init(&source->inspect);
  turbo_agent_template_slot_init(&source->change);
  turbo_agent_template_slot_init(&source->verify);
}

void turbo_agent_repair_source_init(turbo_agent_repair_source_t *source) {
  if (!source) return;
  memset(source, 0, sizeof(*source));
  source->struct_size = sizeof(*source);
  source->abi_version = TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION;
  source->plan_version = 1u;
  turbo_agent_template_slot_init(&source->diagnose);
  turbo_agent_template_slot_init(&source->change);
  turbo_agent_template_slot_init(&source->verify);
}

turbo_agent_compile_status_t turbo_agent_compile_change_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_change_source_t *source,
    turbo_agent_template_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  if (!source || source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_CHANGE_SOURCE_ABI_VERSION) {
    if (out_plan) *out_plan = NULL;
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid Change template source");
  }
  return template_compile_three_step(
      TURBO_AGENT_TEMPLATE_CHANGE, source->plan_version, 0u,
      config, source_registry, "inspect",
      &source->inspect, &source->change, &source->verify,
      out_plan, diagnostic);
}

turbo_agent_compile_status_t turbo_agent_compile_repair_template(
    const turbo_agent_compiler_config_t *config,
    const turbo_tool_registry_t *source_registry,
    const turbo_agent_repair_source_t *source,
    turbo_agent_template_plan_t **out_plan,
    turbo_agent_compile_diagnostic_t *diagnostic) {
  if (!source || source->struct_size != sizeof(*source) ||
      source->abi_version != TURBO_AGENT_REPAIR_SOURCE_ABI_VERSION ||
      source->plan_version == 0u ||
      (source->plan_version - 1u) > source->max_replans) {
    if (out_plan) *out_plan = NULL;
    return template_fail(
        diagnostic, TURBO_AGENT_COMPILE_INVALID_ARGUMENT,
        "invalid Repair template source/version");
  }
  return template_compile_three_step(
      TURBO_AGENT_TEMPLATE_REPAIR, source->plan_version,
      source->max_replans, config, source_registry, "diagnose",
      &source->diagnose, &source->change, &source->verify,
      out_plan, diagnostic);
}

void turbo_agent_template_plan_destroy(turbo_agent_template_plan_t *plan) {
  if (!plan) return;
  turbo_agent_executable_dag_destroy(plan->dag);
  memset(plan, 0, sizeof(*plan));
  free(plan);
}

turbo_agent_template_kind_t turbo_agent_template_plan_kind(
    const turbo_agent_template_plan_t *plan) {
  return plan ? plan->kind : TURBO_AGENT_TEMPLATE_INVALID;
}

uint32_t turbo_agent_template_plan_version(
    const turbo_agent_template_plan_t *plan) {
  return plan ? plan->plan_version : 0u;
}

uint32_t turbo_agent_template_plan_max_replans(
    const turbo_agent_template_plan_t *plan) {
  return plan ? plan->max_replans : 0u;
}

uint64_t turbo_agent_template_plan_hash(
    const turbo_agent_template_plan_t *plan) {
  return plan && plan->dag
             ? turbo_agent_executable_dag_hash(plan->dag)
             : 0u;
}

const turbo_agent_executable_dag_t *turbo_agent_template_plan_dag(
    const turbo_agent_template_plan_t *plan) {
  return plan ? plan->dag : NULL;
}

const turbo_tool_registry_t *turbo_agent_template_plan_approved_tools(
    const turbo_agent_template_plan_t *plan) {
  return plan && plan->dag
             ? turbo_agent_executable_dag_approved_tools(plan->dag)
             : NULL;
}

turbo_agent_verify_status_t
turbo_agent_template_verify_status_from_praktor_result(
    const json_value_t *praktor_result) {
  const char *workflow_status;
  const json_value_t *outputs;
  const json_value_t *verify_result;
  const json_value_t *passed;

  if (!praktor_result || json_type(praktor_result) != JSON_OBJECT) {
    return TURBO_AGENT_VERIFY_EXECUTION_FAILURE;
  }
  workflow_status = json_get_string(praktor_result, "workflow_status");
  if (!workflow_status || strcmp(workflow_status, "success") != 0) {
    return TURBO_AGENT_VERIFY_EXECUTION_FAILURE;
  }
  outputs = json_object_get(praktor_result, "outputs");
  if (!outputs || json_type(outputs) != JSON_OBJECT) {
    return TURBO_AGENT_VERIFY_EXECUTION_FAILURE;
  }
  verify_result = json_object_get(outputs, "verify_result");
  if (!verify_result || json_type(verify_result) != JSON_OBJECT) {
    return TURBO_AGENT_VERIFY_EXECUTION_FAILURE;
  }
  passed = json_object_get(verify_result, "passed");
  if (!passed || json_type(passed) != JSON_BOOL) {
    return TURBO_AGENT_VERIFY_EXECUTION_FAILURE;
  }
  return json_bool(passed)
             ? TURBO_AGENT_VERIFY_PASSED
             : TURBO_AGENT_VERIFY_SEMANTIC_FAILURE;
}

turbo_agent_template_outcome_t turbo_agent_template_finish_verify(
    const turbo_agent_template_plan_t *plan,
    turbo_agent_verify_status_t verify_status) {
  if (!plan ||
      (verify_status != TURBO_AGENT_VERIFY_PASSED &&
       verify_status != TURBO_AGENT_VERIFY_SEMANTIC_FAILURE &&
       verify_status != TURBO_AGENT_VERIFY_EXECUTION_FAILURE)) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_INVALID_ARGUMENT;
  }
  if (verify_status == TURBO_AGENT_VERIFY_PASSED) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED;
  }
  if (verify_status == TURBO_AGENT_VERIFY_EXECUTION_FAILURE) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_EXECUTION_FAILED;
  }
  if (plan->kind != TURBO_AGENT_TEMPLATE_REPAIR) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_VERIFY_FAILED;
  }
  if (plan->plan_version <= plan->max_replans) {
    return TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_REQUIRED;
  }
  return TURBO_AGENT_TEMPLATE_OUTCOME_REPLAN_LIMIT_REACHED;
}

json_value_t *turbo_agent_template_plan_certificate_json_value(
    const turbo_agent_template_plan_t *plan) {
  const turbo_agent_template_descriptor_t *descriptor;
  json_value_t *root = NULL;
  json_value_t *dag = NULL;
  json_value_t *field = NULL;
  char hash_text[17];

  if (!plan || !plan->dag) return NULL;
  descriptor = turbo_agent_template_descriptor(plan->kind);
  if (!descriptor) return NULL;

  root = json_create_object();
  dag = turbo_agent_executable_dag_certificate_json_value(plan->dag);
  if (!root || !dag) goto fail;

#define TEMPLATE_SET_STRING(key, value) \
  do { \
    field = json_create_string((value)); \
    if (!field || turbo_runtime_json_object_set(root, (key), field) != \
                      TURBO_RUNTIME_JSON_OK) goto fail; \
    field = NULL; \
  } while (0)

#define TEMPLATE_SET_INT(key, value) \
  do { \
    field = json_create_int64((int64_t)(value)); \
    if (!field || turbo_runtime_json_object_set(root, (key), field) != \
                      TURBO_RUNTIME_JSON_OK) goto fail; \
    field = NULL; \
  } while (0)

  TEMPLATE_SET_INT("version", TURBO_AGENT_TEMPLATE_PLAN_CERTIFICATE_VERSION);
  TEMPLATE_SET_STRING("template", descriptor->name);
  TEMPLATE_SET_INT("template_version", descriptor->version);
  TEMPLATE_SET_INT("plan_version", plan->plan_version);
  TEMPLATE_SET_INT("max_replans", plan->max_replans);
  snprintf(hash_text, sizeof(hash_text), "%016llx",
           (unsigned long long)turbo_agent_executable_dag_hash(plan->dag));
  TEMPLATE_SET_STRING("plan_hash", hash_text);

  if (turbo_runtime_json_object_set(root, "dag", dag) !=
      TURBO_RUNTIME_JSON_OK) goto fail;
  dag = NULL;

#undef TEMPLATE_SET_INT
#undef TEMPLATE_SET_STRING
  return root;

fail:
  turbo_runtime_json_destroy(field);
  turbo_runtime_json_destroy(dag);
  turbo_runtime_json_destroy(root);
  return NULL;
}
