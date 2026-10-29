#include "../src/turbo_agent_tool_executor_internal.h"
#include "../src/turbo_agent_tool_journal_internal.h"
#include "tinytest.h"
#include "turbo_runtime_store_sqlite.h"
#include <fmt.h>
#include <stdlib.h>
#include <string.h>
#include <tstr.h>

/* Production SQLite is a single-active-owner Store; prove reopen/replay
 * through real SQLite file locks, not the shared-memory atomic-claim mock. */
typedef struct sqlite_owner_probe_s { unsigned calls; } sqlite_owner_probe_t;

static turbo_tool_status_t owner_invoke(const char *args,
    const turbo_tool_execution_context_t *ctx, char **out, void *user) {
  static const char result[] = "{\"ok\":true}";
  sqlite_owner_probe_t *p = user;
  (void)args;
  *out = NULL;
  if (!ctx || !ctx->thread_id || strcmp(ctx->thread_id, "thread") ||
      !ctx->run_id || strcmp(ctx->run_id, "run") ||
      !ctx->turn_id || strcmp(ctx->turn_id, "turn") ||
      !ctx->tool_call_id) return TURBO_TOOL_ERROR;
  ++p->calls;
  *out = malloc(sizeof(result));
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, result, sizeof(result));
  return TURBO_TOOL_OK;
}

static turbo_agent_tool_execution_t owner_call(const char *id, const char *args) {
  turbo_agent_tool_execution_t c = {0};
  c.call_id = id;
  c.turn_key = "turn";
  c.tool_name = "owner.probe";
  c.arguments_json = args;
  return c;
}

static void owner_clear(turbo_agent_tool_execution_t *c) {
  free(c->output);
  json_free(c->detail);
  *c = (turbo_agent_tool_execution_t){0};
}

static int owner_execute(turbo_agent_tool_executor_t *e, turbo_agent_runtime_t *r,
                         const turbo_tool_registry_t *registry, int allow,
                         turbo_agent_tool_execution_t *c) {
  turbo_agent_policy_t p = turbo_agent_policy_default();
  p.allow_custom_tools = allow;
  return turbo_agent_tool_executor_execute(e, r, NULL, "thread", "run",
                                            NULL, NULL, registry, &p, c, 1);
}

spec("SQLite Tool Journal owner-lock and reopen evidence") {
  static char *dir;
  static tstr path;
  static turbo_runtime_sqlite_config_t cfg;
  static turbo_agent_runtime_store_t primary, competing;
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_tool_executor_t *executor;
  static turbo_tool_registry_t *registry;
  static sqlite_owner_probe_t probe;
  static turbo_agent_tool_execution_t first, replay, denied, conflict, fresh;
  static json_value_t *rows;

  before_each() {
    turbo_tool_definition_v4_t definition = {0};
    dir = tt_make_temp_dir("tool-owner-lock");
    check_not_null(dir);
    path = tstr_format("{}/journal.db", dir);
    check_not_null(path);
    primary = competing = (turbo_agent_runtime_store_t){0};
    probe = (sqlite_owner_probe_t){0};
    first = replay = denied = conflict = fresh = (turbo_agent_tool_execution_t){0};
    runtime = NULL; registry = NULL; executor = NULL; rows = NULL;
    turbo_runtime_sqlite_config_init(&cfg);
    cfg.path = path;
    cfg.busy_timeout_ms = 20;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &primary), SALTS_OK);
    runtime = turbo_agent_runtime_create(&primary);
    if (runtime) primary = (turbo_agent_runtime_store_t){0};
    check_not_null(runtime);
    registry = turbo_tool_registry_create(); check_not_null(registry);
    definition.struct_size = sizeof(definition);
    definition.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
    definition.definition.name = "owner.probe";
    definition.definition.description = "Owned SQLite Tool Journal fixture";
    definition.definition.parameters_json = "{\"type\":\"object\"}";
    definition.definition.user_data = &probe;
    definition.context_handler = owner_invoke;
    check_equal(turbo_tool_registry_add_v4(registry, &definition), TURBO_TOOL_OK);
    check_equal(turbo_agent_tool_executor_create(NULL, &executor), SALTS_OK);
  }
  after_each() {
    turbo_agent_tool_executor_destroy(executor); executor = NULL;
    turbo_tool_registry_destroy(registry); registry = NULL;
    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    if (primary.user_data_free) primary.user_data_free(primary.user_data);
    if (competing.user_data_free) competing.user_data_free(competing.user_data);
    primary = competing = (turbo_agent_runtime_store_t){0};
    owner_clear(&first); owner_clear(&replay); owner_clear(&denied);
    owner_clear(&conflict); owner_clear(&fresh);
    json_free(rows); rows = NULL;
    tstr_free(path); path = NULL;
    if (dir) tt_remove_tree(dir);
    free(dir); dir = NULL;
  }

  it("rejects simultaneous SQLite owner and replays only the canonical committed effect") {
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &competing), SALTS_EBUSY);
    check_null(competing.put);
    check_null(competing.get);
    check_null(competing.list);
    check_null(competing.create_if_absent);
    check_null(competing.user_data);

    first = owner_call("same", "{}");
    check_equal(owner_execute(executor, runtime, registry, 1, &first), SALTS_OK);
    check_equal(first.status, TURBO_TOOL_OK);
    check_false(first.replayed);
    check_equal(first.output, "{\"ok\":true}");
    check_equal(probe.calls, 1u);
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &competing), SALTS_EBUSY);
    check_null(competing.user_data);

    turbo_agent_runtime_destroy(runtime); runtime = NULL;
    cfg.create_if_missing = 0;
    check_equal(turbo_runtime_store_sqlite_open(&cfg, &competing), SALTS_OK);
    check_not_null(competing.create_if_absent);
    runtime = turbo_agent_runtime_create(&competing);
    if (runtime) competing = (turbo_agent_runtime_store_t){0};
    check_not_null(runtime);

    denied = owner_call("same", "{}");
    check_equal(owner_execute(executor, runtime, registry, 0, &denied), SALTS_OK);
    check_equal(denied.status, TURBO_TOOL_ERROR);
    check_equal(denied.policy_reason, "custom_tools_disabled");
    check_null(denied.output);
    check_equal(probe.calls, 1u);
    replay = owner_call("same", "{}");
    check_equal(owner_execute(executor, runtime, registry, 1, &replay), SALTS_OK);
    check_equal(replay.status, TURBO_TOOL_OK);
    check_true(replay.replayed);
    check_equal(replay.output, "{\"ok\":true}");
    check_equal(probe.calls, 1u);
    conflict = owner_call("same", "{\"other\":true}");
    check_equal(owner_execute(executor, runtime, registry, 1, &conflict), SALTS_EPROTO);
    check_null(conflict.output);
    check_equal(probe.calls, 1u);
    fresh = owner_call("different", "{}");
    check_equal(owner_execute(executor, runtime, registry, 1, &fresh), SALTS_OK);
    check_equal(fresh.status, TURBO_TOOL_OK);
    check_false(fresh.replayed);
    check_equal(probe.calls, 2u);

    check_equal(turbo_agent_runtime_store_list_json(runtime,
        TURBO_AGENT_TOOL_JOURNAL_COLLECTION, NULL, NULL, &rows), SALTS_OK);
    check_equal(json_array_size(rows), 2u);
    for (size_t i = 0; i < json_array_size(rows); ++i)
      check_equal(json_get_string(json_array_get(rows, i), "phase"), "committed");
    json_free(rows); rows = NULL;
    check_equal(turbo_agent_runtime_store_list_json(runtime,
        TURBO_AGENT_TOOL_CLAIM_COLLECTION, NULL, NULL, &rows), SALTS_OK);
    check_equal(json_array_size(rows), 2u);
    for (size_t i = 0; i < json_array_size(rows); ++i)
      check_equal(json_get_string(json_array_get(rows, i), "phase"), "admitted");
  }
}
