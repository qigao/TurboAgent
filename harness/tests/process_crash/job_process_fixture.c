#include "turbo_agent_job.h"
#include "turbo_runtime_store_sqlite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "Job fixture line %d: %s\n", __LINE__, #x); return 90; } } while (0)

typedef struct fixture_s {
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime;
  const char *cut;
  int inject, claims, puts;
} fixture_t;

static int is_root(const char *cut) { return !strncmp(cut, "root_", 5); }
static int is_binding(const char *cut) { return !strncmp(cut, "binding_", 8); }
static int is_before(const char *cut) { return strstr(cut, "_before") != NULL; }
static int lost_ack(const char *cut) { return strstr(cut, "_lost_ack") != NULL; }

static int fixture_get(void *data, const char *collection, const char *id, char **out) {
  fixture_t *f = data;
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int fixture_put(void *data, const char *collection, const char *id, const char *record) {
  fixture_t *f = data;
  ++f->puts;
  return f->inner.put(f->inner.user_data, collection, id, record);
}
static int fixture_list(void *data, const char *collection, const char *key,
    const char *value, char **out) {
  fixture_t *f = data;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static int fixture_claim(void *data, const char *collection, const char *id, const char *record) {
  fixture_t *f = data;
  int rc;
  ++f->claims;
  if (f->inject && is_before(f->cut)) _Exit(86);
  rc = f->inner.create_if_absent(f->inner.user_data, collection, id, record);
  if (f->inject && rc == SALTS_OK) {
    if (lost_ack(f->cut)) return SALTS_EIO;
    _Exit(86);
  }
  return rc;
}
static void fixture_free(void *data) {
  fixture_t *f = data;
  f->inner.user_data_free(f->inner.user_data);
}
static int fixture_open(fixture_t *f, const char *directory) {
  char path[2048];
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t wrapped;
  int rc;
  REQUIRE(snprintf(path, sizeof(path), "%s/job.db", directory) > 0);
  REQUIRE(strlen(directory) + 8 < sizeof(path));
  turbo_runtime_sqlite_config_init(&config);
  config.path = path;
  rc = turbo_runtime_store_sqlite_open(&config, &f->inner);
  REQUIRE(rc == SALTS_OK);
  wrapped = (turbo_agent_runtime_store_t){.get = fixture_get, .put = fixture_put,
      .list = fixture_list, .create_if_absent = fixture_claim,
      .user_data = f, .user_data_free = fixture_free};
  f->runtime = turbo_agent_runtime_create(&wrapped);
  if (!f->runtime) fixture_free(f);
  REQUIRE(f->runtime);
  return 0;
}
static int create_job(fixture_t *f, turbo_agent_job_t **job) {
  return turbo_agent_job_create(f->runtime, "job", "inspect repository", "verified evidence", job);
}
static int propose(turbo_agent_job_t *job) {
  return turbo_agent_job_propose(job, 0, "{\"template_id\":\"inspect\"}",
      strlen("{\"template_id\":\"inspect\"}"), "fixture-source-v1");
}
static int bind_attempt(turbo_agent_job_t *job, uint32_t revision) {
  return turbo_agent_job_bind_compiled_inspect(job, revision, "stable-thread", "stable-attempt");
}
static int no_execution_records(fixture_t *f) {
  const char *collections[] = {"compiled_inspect_attempts", "runs", "agent_tool_journal"};
  size_t i;
  for (i = 0; i < sizeof(collections) / sizeof(collections[0]); ++i) {
    json_value_t *records = NULL;
    REQUIRE(turbo_agent_runtime_store_list_json(f->runtime, collections[i], NULL, NULL, &records) == SALTS_OK);
    REQUIRE(records && json_array_size(records) == 0);
    json_free(records);
  }
  return 0;
}
static int check_snapshot(turbo_agent_job_t *job, int revision) {
  json_value_t *snapshot = NULL;
  REQUIRE(turbo_agent_job_get_snapshot(job, &snapshot) == SALTS_OK);
  REQUIRE(json_get_int(snapshot, "revision", -1) == revision);
  REQUIRE(!strcmp(json_get_string(snapshot, "goal"), "inspect repository"));
  REQUIRE(!strcmp(json_get_string(snapshot, "state"), revision == 0 ? "created" :
      revision == 1 ? "proposed" : "attempt_bound"));
  if (revision >= 1) {
    const json_value_t *proposal = json_object_get(snapshot, "proposal");
    REQUIRE(proposal && !strcmp(json_get_string(proposal, "source_revision"), "fixture-source-v1"));
    REQUIRE(!strcmp(json_get_string(proposal, "source_json"), "{\"template_id\":\"inspect\"}"));
  }
  if (revision == 2) {
    const json_value_t *attempt = json_object_get(snapshot, "attempt");
    REQUIRE(attempt && json_get_int(attempt, "proposal_version", -1) == 1);
    REQUIRE(!strcmp(json_get_string(attempt, "thread_id"), "stable-thread"));
    REQUIRE(!strcmp(json_get_string(attempt, "attempt_id"), "stable-attempt"));
  }
  REQUIRE(!json_object_get(snapshot, "result"));
  json_free(snapshot);
  return 0;
}
static int run(fixture_t *f, const char *stage) {
  turbo_agent_job_t *job = NULL;
  int rc;
  if (!strcmp(stage, "init")) {
    if (!is_root(f->cut)) {
      REQUIRE(create_job(f, &job) == SALTS_OK);
      if (is_binding(f->cut)) REQUIRE(propose(job) == SALTS_OK);
    }
  } else if (!strcmp(stage, "crash")) {
    if (!is_root(f->cut)) REQUIRE(turbo_agent_job_open(f->runtime, "job", &job) == SALTS_OK);
    f->inject = 1;
    rc = is_root(f->cut) ? create_job(f, &job) :
        is_binding(f->cut) ? bind_attempt(job, 1) : propose(job);
    REQUIRE(lost_ack(f->cut) && rc == SALTS_EIO);
    _Exit(87); /* The publication error reached the application before exit. */
  } else if (!strcmp(stage, "inspect") || !strcmp(stage, "verify")) {
    int revision = !strcmp(stage, "verify") ? 2 : is_root(f->cut) ? 0 :
        is_binding(f->cut) ? (is_before(f->cut) ? 1 : 2) : (is_before(f->cut) ? 0 : 1);
    rc = turbo_agent_job_open(f->runtime, "job", &job);
    if (!strcmp(stage, "inspect") && is_root(f->cut) && is_before(f->cut)) {
      REQUIRE(rc == SALTS_ENOENT && !job);
    } else {
      REQUIRE(rc == SALTS_OK);
      REQUIRE(check_snapshot(job, revision) == 0);
    }
    REQUIRE(f->claims == 0 && f->puts == 0);
  } else if (!strcmp(stage, "recover")) {
    if (is_root(f->cut) && is_before(f->cut)) REQUIRE(create_job(f, &job) == SALTS_OK);
    else REQUIRE(turbo_agent_job_open(f->runtime, "job", &job) == SALTS_OK);
    if (is_root(f->cut) || (!is_binding(f->cut) && is_before(f->cut))) {
      REQUIRE(propose(job) == SALTS_OK);
      REQUIRE(bind_attempt(job, 1) == SALTS_OK);
    } else if (!is_binding(f->cut)) {
      REQUIRE(propose(job) == SALTS_EBUSY);
      REQUIRE(bind_attempt(job, 1) == SALTS_OK);
    } else if (is_before(f->cut)) REQUIRE(bind_attempt(job, 1) == SALTS_OK);
    else {
      REQUIRE(bind_attempt(job, 1) == SALTS_EBUSY);
      REQUIRE(bind_attempt(job, 2) == SALTS_EBUSY);
      REQUIRE(turbo_agent_job_propose(job, 2, "replace", 7, "different") == SALTS_EBUSY);
    }
    REQUIRE(check_snapshot(job, 2) == 0);
  } else return 90;
  turbo_agent_job_destroy(job);
  REQUIRE(f->puts == 0);
  return no_execution_records(f);
}
int main(int argc, char **argv) {
  fixture_t fixture = {0};
  int rc;
  if (argc != 4) return 90;
  fixture.cut = argv[2];
  rc = fixture_open(&fixture, argv[3]);
  if (!rc) rc = run(&fixture, argv[1]);
  turbo_agent_runtime_destroy(fixture.runtime);
  return rc;
}
