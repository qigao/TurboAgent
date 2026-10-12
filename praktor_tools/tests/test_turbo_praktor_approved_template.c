#include "tinytest.h"
#include "turbo_praktor_approved_template.h"
#include "turbo_runtime_json.h"
#include "turbo_runtime_store_sqlite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct approved_probe_s {
  int inspect;
  int change;
  int verify;
} approved_probe_t;

static turbo_tool_status_t approved_inspect(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->inspect;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t approved_change(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->change;
  *out = json_create_object();
  return *out ? TURBO_TOOL_OK : TURBO_TOOL_OUT_OF_MEMORY;
}

static turbo_tool_status_t approved_verify(
    const json_value_t *args, const turbo_tool_execution_context_t *context,
    json_value_t **out, void *user) {
  approved_probe_t *p = (approved_probe_t *)user;
  (void)args; (void)context;
  ++p->verify;
  *out = json_create_object();
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  if (turbo_runtime_json_object_set(*out, "verified", json_create_bool(1)) !=
      TURBO_RUNTIME_JSON_OK) return TURBO_TOOL_OUT_OF_MEMORY;
  return TURBO_TOOL_OK;
}

static int approved_add_tool(
    turbo_tool_registry_t *registry, const char *name,
    turbo_tool_idempotency_t idempotency, turbo_tool_effect_flags_t effects,
    turbo_tool_json_value_context_handler_fn callback, approved_probe_t *probe) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_tool_definition_v6_t definition = {0};
  definition.struct_size = sizeof(definition);
  definition.abi_version = TURBO_TOOL_DEFINITION_V6_ABI_VERSION;
  definition.definition.name = name;
  definition.definition.description = name;
  definition.definition.parameters_json =
      "{\"type\":\"object\",\"additionalProperties\":true}";
  definition.definition.strict = 1;
  definition.definition.user_data = probe;
  definition.execution_policy.mode = TURBO_TOOL_EXECUTION_SEQUENTIAL;
  definition.execution_policy.idempotency = idempotency;
  definition.required_capabilities = caps;
  definition.required_capability_count = 1;
  definition.json_value_context_handler = callback;
  definition.effect_flags = effects;
  return turbo_tool_registry_add_v6(registry, &definition) == TURBO_TOOL_OK;
}

static turbo_tool_registry_t *approved_registry(
    approved_probe_t *probe, turbo_tool_effect_flags_t change_effect) {
  turbo_tool_registry_t *r = turbo_tool_registry_create();
  if (!r) return NULL;
  if (!approved_add_tool(r, "repo.inspect", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         TURBO_TOOL_EFFECT_READ, approved_inspect, probe) ||
      !approved_add_tool(r, "repo.change", TURBO_TOOL_IDEMPOTENCY_NONE,
                         change_effect, approved_change, probe) ||
      !approved_add_tool(r, "repo.verify", TURBO_TOOL_IDEMPOTENCY_READ_ONLY,
                         TURBO_TOOL_EFFECT_READ, approved_verify, probe)) {
    turbo_tool_registry_destroy(r);
    return NULL;
  }
  return r;
}

static turbo_agent_template_plan_t *approved_template(
    turbo_tool_registry_t *registry, json_value_t *arguments, int retry) {
  static const char *const caps[] = {"runtime_tools"};
  turbo_agent_compiler_config_t compiler;
  turbo_agent_change_source_t src;
  turbo_agent_template_plan_t *plan = NULL;
  turbo_agent_compiler_config_init(&compiler);
  compiler.allowed_capabilities = caps;
  compiler.allowed_capability_count = 1;
  turbo_agent_change_source_init(&src);
  src.inspect.tool_name = "repo.inspect";
  src.inspect.arguments = arguments;
  src.change.tool_name = "repo.change";
  src.change.arguments = arguments;
  src.verify.tool_name = "repo.verify";
  src.verify.arguments = arguments;
  src.verify.retry_limit = (uint32_t)retry;
  if (turbo_agent_compile_change_template(
          &compiler, registry, &src, &plan, NULL) != TURBO_AGENT_COMPILE_OK)
    return NULL;
  return plan;
}

spec("approved Praktor ABI 2.6 execution / durable attempt") {
  it("rejects absent host approval and replays a committed three-step workflow") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_agent_template_outcome_t outcome;
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(registry); check_not_null(runtime); check_not_null(plan);
    check_true(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A"), TURBO_TOOL_OK);

    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(outcome, TURBO_AGENT_TEMPLATE_OUTCOME_COMPLETED);
    check_equal(replayed, 0);
    check_not_null(result);
    check_equal(probe.inspect, 1);
    check_equal(probe.change, 1);
    check_equal(probe.verify, 1);
    json_free(result); result = NULL;
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_A", "host_approval_A",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(replayed, 1);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    json_free(result);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("prevents execution after STARTED recovery or mismatched approved identity") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object(), *record = NULL;
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_agent_template_outcome_t outcome;
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(plan);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "attempt_B", "approved_B"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "WRONG_APPROVAL",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);

    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "approved_B",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    json_free(result); result = NULL;
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "praktor_template_attempts", "008_thread_A_attempt_B", &record),
        SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(
        record, "phase", "started"), TURBO_RUNTIME_JSON_OK);
    check_equal(turbo_agent_runtime_store_put_json(
        runtime, "praktor_template_attempts", "008_thread_A_attempt_B", record),
        SALTS_OK);
    json_free(record);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "attempt_B", "approved_B",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
    check_equal(replayed, 0);
    check_equal(probe.inspect + probe.change + probe.verify, 3);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("rejects unknown effects and backend retry before approval is recorded") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(
        &probe, TURBO_TOOL_EFFECT_UNKNOWN);
    turbo_agent_runtime_store_t store = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *runtime = turbo_agent_runtime_create(&store);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    json_value_t *record = NULL;
    check_not_null(plan);
    check_false(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "reject", "approved"), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(turbo_agent_runtime_store_get_json_status(
        runtime, "praktor_template_approvals", "008_thread_A_reject", &record), SALTS_ENOENT);
    turbo_agent_template_plan_destroy(plan);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);

    registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    store = turbo_agent_runtime_store_memory_create();
    runtime = turbo_agent_runtime_create(&store);
    plan = approved_template(registry, args, 1);
    check_not_null(plan);
    check_false(turbo_agent_template_praktor_execution_preflight(plan));
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "retry", "approved"), TURBO_TOOL_INVALID_ARGUMENT);
    check_equal(probe.inspect + probe.change + probe.verify, 0);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_agent_runtime_destroy(runtime);
    turbo_tool_registry_destroy(registry);
  }

  it("replays a committed outcome across an exclusive SQLite store reopen") {
    approved_probe_t probe = {0};
    turbo_tool_registry_t *registry = approved_registry(&probe, TURBO_TOOL_EFFECT_WRITE);
    json_value_t *args = json_create_object();
    turbo_agent_template_plan_t *plan = approved_template(registry, args, 0);
    turbo_runtime_sqlite_config_t cfg;
    turbo_agent_runtime_store_t store = {0};
    turbo_agent_runtime_t *runtime;
    turbo_agent_template_outcome_t outcome;
    char *directory = tt_make_temp_dir("praktor-approved");
    char db_path[1024];
    json_value_t *result = NULL;
    int replayed = -1;

    check_not_null(registry); check_not_null(plan); check_not_null(directory);
    check_true(snprintf(db_path, sizeof(db_path), "%s/approval.db", directory) <
               (int)sizeof(db_path));
    turbo_runtime_sqlite_config_init(&cfg);
    cfg.path = db_path;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_not_null(runtime);
    check_equal(turbo_praktor_template_approval_commit(
        runtime, plan, "thread_A", "sqlite", "host_reviewed"), TURBO_TOOL_OK);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "sqlite", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    json_free(result); result = NULL;
    check_equal(probe.change, 1);
    turbo_agent_runtime_destroy(runtime);

    cfg.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &store), SALTS_OK);
    runtime = turbo_agent_runtime_create(&store);
    check_equal(turbo_praktor_template_execute_approved(
        runtime, plan, "thread_A", "sqlite", "host_reviewed",
        NULL, &replayed, &outcome, &result), TURBO_TOOL_OK);
    check_equal(replayed, 1);
    check_equal(probe.change, 1);
    json_free(result);
    turbo_agent_runtime_destroy(runtime);
    turbo_agent_template_plan_destroy(plan);
    json_free(args);
    turbo_tool_registry_destroy(registry);
    tt_remove_tree(directory);
    free(directory);
  }
}
