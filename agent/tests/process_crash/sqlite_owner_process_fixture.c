#include "../../src/turbo_agent_tool_executor_internal.h"
#include "../../src/turbo_agent_tool_journal_internal.h"
#include "turbo_runtime_store_sqlite.h"
#include <salts/clock.h>
#include <salts/thread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

enum { OWNER_FAILURE = 2, OWNER_CRASH = 86 };

/* Only enabled by the explicit holder-crash stage before its Tool is queued.
 * The worker observes this flag through the native executor admission path. */
static atomic_int owner_crash_after_effect;
#define REQUIRE(expr) do { if (!(expr)) { \
  fprintf(stderr, "SQLite owner process line %d: %s\n", __LINE__, #expr); \
  return OWNER_FAILURE; } } while (0)

static int open_store(const char *path, int create, turbo_agent_runtime_t **out) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store = {0};
  int rc;
  *out = NULL;
  turbo_runtime_sqlite_config_init(&config);
  config.path = path;
  config.create_if_missing = create;
  config.busy_timeout_ms = 80;
  rc = turbo_runtime_store_sqlite_open(&config, &store);
  if (rc != SALTS_OK) return rc;
  *out = turbo_agent_runtime_create(&store);
  if (!*out) {
    if (store.user_data_free) store.user_data_free(store.user_data);
    return SALTS_ENOMEM;
  }
  return SALTS_OK;
}

static int count_effects(const char *path) {
  FILE *fp = fopen(path, "rb");
  int count = 0, c, failed;
  if (!fp) return -1;
  while ((c = fgetc(fp)) != EOF) {
    if (c != 'E') { fclose(fp); return -1; }
    ++count;
  }
  failed = ferror(fp);
  if (fclose(fp) != 0) failed = 1;
  return failed ? -1 : count;
}

static turbo_tool_status_t invoke(const char *args,
    const turbo_tool_execution_context_t *ctx, char **out, void *user) {
  FILE *fp;
  (void)args;
  *out = NULL;
  if (!ctx || !ctx->thread_id || strcmp(ctx->thread_id, "owner-thread") ||
      !ctx->run_id || strcmp(ctx->run_id, "owner-run") ||
      !ctx->turn_id || strcmp(ctx->turn_id, "turn") ||
      !ctx->tool_call_id) return TURBO_TOOL_ERROR;
  fp = fopen((const char *)user, "ab");
  if (!fp) return TURBO_TOOL_ERROR;
  if (fputc('E', fp) == EOF || fflush(fp) != 0) { fclose(fp); return TURBO_TOOL_ERROR; }
  if (fclose(fp) != 0) return TURBO_TOOL_ERROR;
  if (atomic_load_explicit(&owner_crash_after_effect, memory_order_acquire)) {
    /* Prove the second real external effect is safely closed before killing
     * the owning process, with the Tool Journal still at STARTED. */
    if (strcmp(ctx->tool_call_id, "interrupted") ||
        count_effects((const char *)user) != 2)
      _Exit(OWNER_FAILURE);
    _Exit(OWNER_CRASH);
  }
  *out = malloc(3);
  if (!*out) return TURBO_TOOL_OUT_OF_MEMORY;
  memcpy(*out, "ok", 3);
  return TURBO_TOOL_OK;
}

static int run_call_expected(turbo_agent_runtime_t *runtime, const char *effects,
                             const char *id, int replay_expected,
                             turbo_tool_status_t expected_status) {
  turbo_tool_registry_t *registry = turbo_tool_registry_create();
  turbo_agent_tool_executor_t *executor = NULL;
  turbo_tool_definition_v4_t tool = {0};
  turbo_agent_tool_execution_t call = {0};
  turbo_agent_policy_t policy = turbo_agent_policy_default();
  int rc = OWNER_FAILURE;
  if (!registry) goto done;
  tool.struct_size = sizeof(tool);
  tool.abi_version = TURBO_TOOL_DEFINITION_V4_ABI_VERSION;
  tool.definition.name = "owner.effect";
  tool.definition.description = "External effect receipt in a private test file";
  tool.definition.parameters_json = "{\"type\":\"object\"}";
  tool.definition.user_data = (void *)effects;
  tool.context_handler = invoke;
  if (turbo_tool_registry_add_v4(registry, &tool) != TURBO_TOOL_OK ||
      turbo_agent_tool_executor_create(NULL, &executor) != SALTS_OK) goto done;
  policy.allow_custom_tools = 1;
  call.call_id = id;
  call.turn_key = "turn";
  call.tool_name = "owner.effect";
  call.arguments_json = "{}";
  rc = turbo_agent_tool_executor_execute(executor, runtime, NULL,
      "owner-thread", "owner-run", NULL, NULL, registry, &policy, &call, 1);
  if (rc || call.status != expected_status || call.replayed != replay_expected ||
      (expected_status == TURBO_TOOL_OK
           ? (!call.output || strcmp(call.output, "ok"))
           : (expected_status != TURBO_TOOL_UNKNOWN_SIDE_EFFECT ||
              call.output || call.detail))) rc = OWNER_FAILURE;
done:
  free(call.output);
  json_free(call.detail);
  turbo_agent_tool_executor_destroy(executor);
  turbo_tool_registry_destroy(registry);
  return rc;
}

static int run_call(turbo_agent_runtime_t *runtime, const char *effects,
                    const char *id, int replay_expected) {
  return run_call_expected(runtime, effects, id, replay_expected, TURBO_TOOL_OK);
}

static int check_records(turbo_agent_runtime_t *runtime, size_t expected) {
  const char *collections[] = {TURBO_AGENT_TOOL_JOURNAL_COLLECTION,
                               TURBO_AGENT_TOOL_CLAIM_COLLECTION};
  const char *phases[] = {"committed", "admitted"};
  for (size_t collection = 0; collection < 2; ++collection) {
    json_value_t *records = NULL;
    int valid = turbo_agent_runtime_store_list_json(runtime, collections[collection],
        NULL, NULL, &records) == SALTS_OK && json_array_size(records) == expected;
    for (size_t i = 0; valid && i < expected; ++i) {
      const json_value_t *rec = json_array_get(records, i);
      const char *phase = json_get_string(rec, "phase");
      const char *run = json_get_string(rec, "run_id");
      valid = phase && run && !strcmp(phase, phases[collection]) &&
              !strcmp(run, "owner-run");
    }
    json_free(records);
    if (!valid) return OWNER_FAILURE;
  }
  return 0;
}

/* After an abruptly terminated owner, no fresh Owner may treat a STARTED
 * record as a committed tool output or re-enter the original effect. */
static int check_crash_records(turbo_agent_runtime_t *runtime, int after_recovery) {
  static const char *const names[] = {"original", "interrupted", "after-crash"};
  static const char *const collections[] = {
      TURBO_AGENT_TOOL_JOURNAL_COLLECTION, TURBO_AGENT_TOOL_CLAIM_COLLECTION};
  const size_t expected = after_recovery ? 3u : 2u;
  for (size_t collection = 0; collection < 2; ++collection) {
    json_value_t *records = NULL;
    int seen[3] = {0, 0, 0};
    int valid = turbo_agent_runtime_store_list_json(runtime, collections[collection],
        NULL, NULL, &records) == SALTS_OK &&
        records && json_type(records) == JSON_ARRAY &&
        json_array_size(records) == expected;
    for (size_t i = 0; valid && i < expected; ++i) {
      const json_value_t *entry = json_array_get(records, i);
      const char *id = json_get_string(entry, "call_id");
      const char *phase = json_get_string(entry, "phase");
      const char *run = json_get_string(entry, "run_id");
      const char *turn = json_get_string(entry, "turn_key");
      const char *tool = json_get_string(entry, "tool_name");
      size_t kind;
      if (!id || !phase || !run || !turn || !tool ||
          strcmp(run, "owner-run") || strcmp(turn, "turn") ||
          strcmp(tool, "owner.effect")) { valid = 0; break; }
      for (kind = 0; kind < expected; ++kind)
        if (!strcmp(names[kind], id)) break;
      if (kind == expected || seen[kind]++) { valid = 0; break; }
      if (collection == 0) {
        const char *want = kind == 1 ? "started" : "committed";
        if (strcmp(phase, want)) { valid = 0; break; }
        if (kind == 1) {
          if (json_object_get(entry, "output") ||
              json_object_get(entry, "detail")) valid = 0;
        } else if (!json_get_string(entry, "output") ||
                   strcmp(json_get_string(entry, "output"), "ok")) valid = 0;
      } else if (strcmp(phase, "admitted")) valid = 0;
    }
    for (size_t i = 0; i < expected; ++i)
      if (seen[i] != 1) valid = 0;
    json_free(records);
    if (!valid) return OWNER_FAILURE;
  }
  return 0;
}

static int seed(const char *db, const char *effects) {
  turbo_agent_runtime_t *runtime = NULL;
  int rc = open_store(db, 1, &runtime);
  if (rc == SALTS_OK) rc = run_call(runtime, effects, "original", 0);
  if (rc == SALTS_OK && (count_effects(effects) != 1 ||
                         check_records(runtime, 1) != 0)) rc = OWNER_FAILURE;
  turbo_agent_runtime_destroy(runtime);
  return rc ? OWNER_FAILURE : 0;
}

static int hold(const char *db, const char *effects, const char *flag,
                int crash_after_check) {
  turbo_agent_runtime_t *runtime = NULL;
  int rc = open_store(db, 0, &runtime);
  uint64_t start;
  if (rc != SALTS_OK) return OWNER_FAILURE;
  if (check_records(runtime, 1) != 0 ||
      fputs("READY\n", stdout) == EOF || fflush(stdout) != 0) {
    turbo_agent_runtime_destroy(runtime); return OWNER_FAILURE;
  }
  start = cmeta_monotonic_ms();
  for (;;) {
    FILE *fp;
    if (cmeta_monotonic_ms() - start > 10000) { rc = OWNER_FAILURE; break; }
    fp = fopen(flag, "rb");
    if (fp) {
      int a = fgetc(fp), b = fgetc(fp), c = fgetc(fp);
      int close_rc = fclose(fp);
      /* The contender publishes its marker after fflush/close; a partial
       * marker observed before that boundary is not a failed owner check. */
      if (a == 'O' && b == 'K' && c == EOF && close_rc == 0) {
        rc = SALTS_OK;
        break;
      }
    }
    cmeta_sleep_ms(10);
  }
  if (rc == SALTS_OK && crash_after_check) {
    atomic_store_explicit(&owner_crash_after_effect, 1, memory_order_release);
    /* The only accepted exit is _Exit(86) inside the real Tool callback after
     * it appends and closes its second external-effect marker. */
    (void)run_call(runtime, effects, "interrupted", 0);
    rc = OWNER_FAILURE;
  }
  turbo_agent_runtime_destroy(runtime);
  return rc ? OWNER_FAILURE : 0;
}

static int contend(const char *db, const char *flag) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store = {0};
  FILE *fp;
  char ready[16] = {0};
  int rc;
  REQUIRE(fgets(ready, sizeof(ready), stdin) && !strcmp(ready, "READY\n"));
  turbo_runtime_sqlite_config_init(&config);
  config.path = db;
  config.create_if_missing = 0;
  config.busy_timeout_ms = 80;
  rc = turbo_runtime_store_sqlite_open(&config, &store);
  if (rc == SALTS_OK) {
    if (store.user_data_free) store.user_data_free(store.user_data);
    return OWNER_FAILURE;
  }
  REQUIRE(rc == SALTS_EBUSY && !store.put && !store.get && !store.list &&
          !store.create_if_absent && !store.user_data);
  fp = fopen(flag, "wb");
  REQUIRE(fp);
  if (fwrite("OK", 1, 2, fp) != 2 || fflush(fp) != 0) {
    fclose(fp); return OWNER_FAILURE;
  }
  REQUIRE(fclose(fp) == 0);
  return 0;
}

static int reopen_store(const char *db, const char *effects) {
  turbo_agent_runtime_t *runtime = NULL;
  int rc = open_store(db, 0, &runtime);
  if (rc == SALTS_OK && count_effects(effects) == 1 &&
      check_records(runtime, 1) == 0)
    rc = run_call(runtime, effects, "original", 1);
  else rc = OWNER_FAILURE;
  if (rc == SALTS_OK && count_effects(effects) == 1)
    rc = run_call(runtime, effects, "independent", 0);
  else rc = OWNER_FAILURE;
  if (rc == SALTS_OK && (count_effects(effects) != 2 ||
                         check_records(runtime, 2) != 0)) rc = OWNER_FAILURE;
  turbo_agent_runtime_destroy(runtime);
  return rc ? OWNER_FAILURE : 0;
}

static int reopen_crashed(const char *db, const char *effects) {
  turbo_agent_runtime_t *runtime = NULL;
  int rc = open_store(db, 0, &runtime);
  if (rc != SALTS_OK) return OWNER_FAILURE;
  if (count_effects(effects) != 2 || check_crash_records(runtime, 0) != 0)
    rc = OWNER_FAILURE;
  if (rc == SALTS_OK)
    rc = run_call_expected(runtime, effects, "interrupted", 1,
                           TURBO_TOOL_UNKNOWN_SIDE_EFFECT);
  if (rc == SALTS_OK && count_effects(effects) == 2)
    rc = run_call(runtime, effects, "original", 1);
  else rc = OWNER_FAILURE;
  if (rc == SALTS_OK && count_effects(effects) == 2)
    rc = run_call(runtime, effects, "after-crash", 0);
  else rc = OWNER_FAILURE;
  if (rc == SALTS_OK &&
      (count_effects(effects) != 3 || check_crash_records(runtime, 1) != 0))
    rc = OWNER_FAILURE;
  turbo_agent_runtime_destroy(runtime);
  return rc ? OWNER_FAILURE : 0;
}

int main(int argc, char **argv) {
  if (argc != 5) return OWNER_FAILURE;
  if (!strcmp(argv[1], "seed")) return seed(argv[2], argv[3]);
  if (!strcmp(argv[1], "hold")) return hold(argv[2], argv[3], argv[4], 0);
  if (!strcmp(argv[1], "hold_crash")) return hold(argv[2], argv[3], argv[4], 1);
  if (!strcmp(argv[1], "contend")) return contend(argv[2], argv[4]);
  if (!strcmp(argv[1], "reopen")) return reopen_store(argv[2], argv[3]);
  if (!strcmp(argv[1], "reopen_crash")) return reopen_crashed(argv[2], argv[3]);
  return OWNER_FAILURE;
}
