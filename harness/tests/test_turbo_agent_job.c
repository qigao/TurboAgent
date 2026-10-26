#include "tinytest.h"
#include "turbo_agent_job.h"
#include "turbo_agent_job_cancel.h"
#include <stdlib.h>
#include <string.h>

typedef struct job_store_s {
  turbo_agent_runtime_store_t inner;
  int reads, claims, puts, lists;
  int fail_read, fail_claim, after_claim, collide;
} job_store_t;

static int job_get(void *data, const char *collection, const char *id, char **out) {
  job_store_t *store = data;
  ++store->reads;
  if (store->fail_read) { *out = NULL; return store->fail_read; }
  return store->inner.get(store->inner.user_data, collection, id, out);
}
static int job_put(void *data, const char *collection, const char *id, const char *record) {
  job_store_t *store = data;
  ++store->puts;
  return store->inner.put(store->inner.user_data, collection, id, record);
}
static int job_list(void *data, const char *collection, const char *key,
    const char *value, char **out) {
  job_store_t *store = data;
  ++store->lists;
  return store->inner.list(store->inner.user_data, collection, key, value, out);
}
static int job_claim(void *data, const char *collection, const char *id, const char *record) {
  job_store_t *store = data;
  int rc;
  ++store->claims;
  if (store->fail_claim && !store->after_claim) return store->fail_claim;
  if (store->collide) {
    /* Another publisher wins after the reader's preflight. */
    json_value_t *winner = json_parse(record, strlen(record));
    char *text;
    store->collide = 0;
    if (!winner || turbo_runtime_json_object_set_string(winner, "source_json", "winner")) {
      json_free(winner);
      return SALTS_ENOMEM;
    }
    text = json_serialize(winner, NULL);
    rc = text ? store->inner.create_if_absent(store->inner.user_data, collection, id, text) : SALTS_ENOMEM;
    json_serialize_free(text);
    json_free(winner);
    if (rc != SALTS_OK) return rc;
  }
  rc = store->inner.create_if_absent(store->inner.user_data, collection, id, record);
  return rc == SALTS_OK && store->fail_claim ? store->fail_claim : rc;
}
static void job_store_free(void *data) {
  job_store_t *store = data;
  store->inner.user_data_free(store->inner.user_data);
}

spec("Job proposal coordination") {
  static job_store_t store;
  static turbo_agent_runtime_t *runtime;
  static turbo_agent_job_t *job, *other;
  static json_value_t *snapshot, *record, *original;

  before_each() {
    turbo_agent_runtime_store_t callbacks;
    store = (job_store_t){.inner = turbo_agent_runtime_store_memory_create()};
    callbacks = (turbo_agent_runtime_store_t){.put = job_put, .get = job_get, .list = job_list,
        .create_if_absent = job_claim, .user_data = &store, .user_data_free = job_store_free};
    runtime = turbo_agent_runtime_create(&callbacks);
    check_not_null(runtime);
    check_equal(turbo_agent_job_create(runtime, "job-1", "inspect repository", "verified evidence", &job), SALTS_OK);
  }
  after_each() {
    turbo_agent_job_destroy(job);
    turbo_agent_job_destroy(other);
    job = other = NULL;
    json_free(snapshot); json_free(record); json_free(original);
    snapshot = record = original = NULL;
    turbo_agent_runtime_destroy(runtime);
  }

  it("freezes proposal versions, binds once, and keeps independent snapshots") {
    const char source[] = {'{', '}', '\n'};
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "created");
    check_equal(json_get_int(snapshot, "revision", -1), 0);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 0, "thread", "attempt"), SALTS_EBUSY);
    check_equal(turbo_agent_job_propose(job, 0, source, sizeof(source), "source-v1"), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, 1, "changed", 7, "source-v2"), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "created");
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 2, "thread", "attempt"), SALTS_OK);
    json_free(snapshot); snapshot = NULL;
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "state"), "attempt_bound");
    check_equal(json_get_int(snapshot, "revision", -1), 3);
    check_equal(json_get_int(json_object_get(snapshot, "proposal"), "revision", -1), 2);
    check_equal(json_get_string(json_object_get(snapshot, "attempt"), "attempt_id"), "attempt");
    check_null(json_object_get(snapshot, "result"));
    check_equal(turbo_agent_job_propose(job, 3, "new", 3, "source-v3"), SALTS_EBUSY);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 3, "thread", "different"), SALTS_EBUSY);
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_001", &record), SALTS_OK);
    check_equal(json_get_string(record, "source_json"), "{}\n");
    check_equal(store.puts, 0);
    check_equal(store.lists, 0);
  }

  it("rejects duplicate jobs, stale heads and invalid untrusted identifiers") {
    int claims = store.claims;
    const char nul_source[] = {'{', '\0', '}'};
    check_equal(turbo_agent_job_create(runtime, "job-1", "replacement", "replacement", &other), SALTS_EALREADY);
    check_null(other);
    check_equal(turbo_agent_job_open(runtime, "../escape", &other), SALTS_EINVAL);
    check_equal(turbo_agent_job_create(runtime, "x/y", "goal", "criteria", &other), SALTS_EINVAL);
    check_equal(turbo_agent_job_propose(job, 0, nul_source, sizeof(nul_source), "source"), SALTS_EINVAL);
    check_equal(turbo_agent_job_propose(job, 0, "x", TURBO_AGENT_JOB_MAX_SOURCE_BYTES + 1, "source"), SALTS_EINVAL);
    check_equal(turbo_agent_job_propose(job, UINT32_MAX, "x", 1, "source"), SALTS_EBUSY);
    check_equal(store.claims, claims + 1);
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "../thread", "attempt"), SALTS_EINVAL);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "thread", ""), SALTS_EINVAL);
    check_equal(turbo_agent_job_open(runtime, "job-1", &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, "stale", 5, "source"), SALTS_EBUSY);
  }

  it("propagates read failures without treating them as absence or performing a write") {
    int claims = store.claims;
    store.fail_read = SALTS_EIO;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_EIO);
    check_equal(turbo_agent_job_open(runtime, "job-1", &other), SALTS_EIO);
    check_null(other);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EIO);
    check_null(snapshot);
    check_equal(store.claims, claims);
    check_equal(store.puts, 0);
  }

  it("does not emulate unsupported claims or retry a pre-publication failure") {
    store.fail_claim = SALTS_ENOTSUP;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_ENOTSUP);
    check_equal(store.claims, 2);
    check_equal(store.puts, 0);
    store.fail_claim = SALTS_ENOSPC;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_ENOSPC);
    store.fail_claim = 0;
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_int(snapshot, "revision", -1), 0);
  }

  it("preserves a lost descriptor acknowledgement and reconciles only on explicit open") {
    int reads = store.reads;
    store.fail_claim = SALTS_EIO;
    store.after_claim = 1;
    check_equal(turbo_agent_job_create(runtime, "lost-job", "goal", "criteria", &other), SALTS_EIO);
    check_null(other);
    check_equal(store.reads - reads, TURBO_AGENT_JOB_MAX_REVISIONS + 1);
    store.fail_claim = 0;
    check_equal(turbo_agent_job_open(runtime, "lost-job", &other), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(other, &snapshot), SALTS_OK);
    check_equal(json_get_string(snapshot, "goal"), "goal");
  }

  it("returns the original lost-ack error without an implicit readback or redispatch") {
    int reads = store.reads;
    store.fail_claim = SALTS_EBUSY;
    store.after_claim = 1;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_EBUSY);
    check_equal(store.reads - reads, TURBO_AGENT_JOB_MAX_REVISIONS + 2);
    check_equal(store.claims, 2);
    store.fail_claim = 0;
    check_equal(turbo_agent_job_open(runtime, "job-1", &other), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(other, &snapshot), SALTS_OK);
    check_equal(json_get_int(snapshot, "revision", -1), 1);
    check_equal(turbo_agent_job_propose(other, 0, "x", 1, "source"), SALTS_EBUSY);
  }

  it("uses conditional insertion rather than overwriting a competing proposal") {
    store.collide = 1;
    check_equal(turbo_agent_job_propose(job, 0, "loser", 5, "source"), SALTS_EALREADY);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_OK);
    check_equal(json_get_string(json_object_get(snapshot, "proposal"), "source_json"), "winner");
    check_equal(store.puts, 0);
  }

  it("rejects revision gaps instead of returning a partial head") {
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_001", &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(record, "revision", 3), 0);
    check_equal(turbo_runtime_json_object_set_int64(record, "previous_revision", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_003", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
    check_null(snapshot);
    check_equal(turbo_agent_job_open(runtime, "job-1", &other), SALTS_EPROTO);
    check_null(other);
  }

  it("validates every frozen binding field and rejects changes after binding") {
    const char *keys[] = {"job_id", "thread_id", "attempt_id"};
    size_t i;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "thread", "attempt"), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_002", &original), SALTS_OK);
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      record = json_clone(original);
      check_not_null(record);
      check_equal(turbo_runtime_json_object_set_string(record, keys[i], "../bad"), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", record), SALTS_OK);
      check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
      check_null(snapshot);
      json_free(record); record = NULL;
    }
    record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(record, "proposal_version", 0), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
  }

  it("rejects future schemas and unknown persisted fields") {
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_jobs", "job-1", &original), SALTS_OK);
    record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_jobs", "job-1", record), SALTS_OK);
    check_equal(turbo_agent_job_open(runtime, "job-1", &other), SALTS_ENOTSUP);
    json_free(record); record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_string(record, "result", "forbidden extra payload"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_jobs", "job-1", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
  }

  it("refuses a surviving orphan history instead of attaching a replacement goal") {
    int claims;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_001", &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(record, "job_id", "orphan"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "orphan_001", record), SALTS_OK);
    claims = store.claims;
    check_equal(turbo_agent_job_create(runtime, "orphan", "replacement", "criteria", &other), SALTS_EALREADY);
    check_null(other);
    check_equal(store.claims, claims);
    check_equal(turbo_agent_job_open(runtime, "orphan", &other), SALTS_ENOENT);
    check_null(other);
  }

  it("fails closed on an actual backend with no conditional-insert callback") {
    turbo_agent_runtime_store_t callbacks = turbo_agent_runtime_store_memory_create();
    turbo_agent_runtime_t *unsupported;
    callbacks.create_if_absent = NULL;
    unsupported = turbo_agent_runtime_create(&callbacks);
    check_not_null(unsupported);
    check_equal(turbo_agent_job_create(unsupported, "unsupported", "goal", "criteria", &other), SALTS_ENOTSUP);
    check_null(other);
    turbo_agent_runtime_destroy(unsupported);
  }

  it("enforces byte bounds while retaining the largest admitted source exactly") {
    char goal[4098], source[TURBO_AGENT_JOB_MAX_SOURCE_BYTES + 1];
    char label[130], id[66], thread[82], attempt[66];
    int claims = store.claims;
    memset(goal, 'g', sizeof(goal)); goal[sizeof(goal) - 1] = 0;
    memset(source, 's', sizeof(source)); source[sizeof(source) - 1] = 0;
    memset(label, 'l', sizeof(label)); label[sizeof(label) - 1] = 0;
    memset(id, 'j', sizeof(id)); id[sizeof(id) - 1] = 0;
    memset(thread, 't', sizeof(thread)); thread[sizeof(thread) - 1] = 0;
    memset(attempt, 'a', sizeof(attempt)); attempt[sizeof(attempt) - 1] = 0;
    check_equal(turbo_agent_job_create(runtime, "large", goal, "criteria", &other), SALTS_EINVAL);
    check_equal(turbo_agent_job_create(runtime, id, "goal", "criteria", &other), SALTS_EINVAL);
    check_equal(turbo_agent_job_propose(job, 0, source, sizeof(source) - 1, label), SALTS_EINVAL);
    check_equal(store.claims, claims);
    goal[4096] = 0; label[128] = 0; id[64] = 0; thread[80] = 0; attempt[64] = 0;
    check_equal(turbo_agent_job_create(runtime, id, goal, goal, &other), SALTS_OK);
    check_equal(turbo_agent_job_propose(other, 0, source, sizeof(source) - 1, label), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(other, 1, thread, attempt), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(other, &snapshot), SALTS_OK);
    check_equal(json_string_len(json_object_get(json_object_get(snapshot, "proposal"), "source_json")),
                TURBO_AGENT_JOB_MAX_SOURCE_BYTES);
    turbo_agent_job_destroy(other); other = NULL;
    check_equal(json_get_string(snapshot, "goal"), goal);
  }

  it("rejects mismatched predecessor and proposal joins, extra fields and future events") {
    const char *integer_keys[] = {"revision", "previous_revision", "proposal_version"};
    size_t i;
    check_equal(turbo_agent_job_propose(job, 0, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, 1, "thread", "attempt"), SALTS_OK);
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_002", &original), SALTS_OK);
    for (i = 0; i < sizeof(integer_keys) / sizeof(integer_keys[0]); ++i) {
      record = json_clone(original);
      check_equal(turbo_runtime_json_object_set_int64(record, integer_keys[i], 9), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", record), SALTS_OK);
      check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
      check_null(snapshot);
      json_free(record); record = NULL;
    }
    record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_ENOTSUP);
    json_free(record); record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_string(record, "result", "not a result authority"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_002", original), SALTS_OK);
    json_free(record); record = NULL;
    check_equal(turbo_agent_runtime_store_get_json_status(runtime, "agent_job_revisions", "job-1_001", &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_int64(record, "revision", 3), 0);
    check_equal(turbo_runtime_json_object_set_int64(record, "previous_revision", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_revisions", "job-1_003", record), SALTS_OK);
    check_equal(turbo_agent_job_get_snapshot(job, &snapshot), SALTS_EPROTO);
  }

  it("bounds history and never reuses an existing revision") {
    uint32_t i;
    for (i = 0; i < TURBO_AGENT_JOB_MAX_REVISIONS; ++i)
      check_equal(turbo_agent_job_propose(job, i, "x", 1, "source"), SALTS_OK);
    check_equal(turbo_agent_job_bind_compiled_inspect(job, TURBO_AGENT_JOB_MAX_REVISIONS,
                "thread", "attempt"), SALTS_ENOSPC);
    check_equal(turbo_agent_job_propose(job, TURBO_AGENT_JOB_MAX_REVISIONS, "x", 1, "source"), SALTS_ENOSPC);
    check_equal(store.claims, TURBO_AGENT_JOB_MAX_REVISIONS + 1);
    check_equal(turbo_agent_job_request_cancel(job, TURBO_AGENT_JOB_MAX_REVISIONS,
        "request-1", "reason"), SALTS_OK);
    check_equal(turbo_agent_job_propose(job, TURBO_AGENT_JOB_MAX_REVISIONS,
        "x", 1, "source"), SALTS_ECANCELED);
  }

  it("durably fences created, proposed and bound Jobs without rewriting their snapshots") {
    for (uint32_t revision = 0; revision <= 2; ++revision) {
      const char *id = revision == 0 ? "cancel-created" : revision == 1 ? "cancel-proposed" : "cancel-bound";
      check_equal(turbo_agent_job_create(runtime, id, "goal", "criteria", &other), SALTS_OK);
      if (revision) check_equal(turbo_agent_job_propose(other, 0, "{}", 2, "source"), SALTS_OK);
      if (revision == 2) check_equal(turbo_agent_job_bind_compiled_inspect(other, 1, "thread", "attempt"), SALTS_OK);
      check_equal(turbo_agent_job_get_cancellation(other, &record), SALTS_ENOENT);
      check_null(record);
      check_equal(turbo_agent_job_request_cancel(other, revision, "request-1", "host-reason"), SALTS_OK);
      check_equal(turbo_agent_job_request_cancel(other, revision, "request-1", "host-reason"), SALTS_EALREADY);
      check_equal(turbo_agent_job_request_cancel(other, revision, "different", "different"), SALTS_EALREADY);
      check_equal(turbo_agent_job_get_cancellation(other, &record), SALTS_OK);
      check_equal(json_object_size(record), (size_t)6);
      check_equal(json_get_string(record, "kind"), "job_cancellation");
      check_equal(json_get_string(record, "job_id"), id);
      check_equal(json_get_int(record, "job_revision", -1), revision);
      check_equal(json_get_string(record, "request_id"), "request-1");
      check_equal(json_get_string(record, "reason_ref"), "host-reason");
      json_free(record); record = NULL;
      check_equal(turbo_agent_job_propose(other, revision, "{}", 2, "new-source"), SALTS_ECANCELED);
      check_equal(turbo_agent_job_bind_compiled_inspect(other, revision, "thread", "attempt"), SALTS_ECANCELED);
      check_equal(turbo_agent_job_get_snapshot(other, &snapshot), SALTS_OK);
      check_equal(json_get_int(snapshot, "revision", -1), revision);
      check_equal(json_get_string(snapshot, "state"), revision == 0 ? "created" : revision == 1 ? "proposed" : "attempt_bound");
      check_null(json_object_get(snapshot, "result"));
      json_free(snapshot); snapshot = NULL;
      turbo_agent_job_destroy(other); other = NULL;
      check_equal(turbo_agent_job_open(runtime, id, &other), SALTS_OK);
      check_equal(turbo_agent_job_get_cancellation(other, &record), SALTS_OK);
      json_free(record); record = NULL;
      turbo_agent_job_destroy(other); other = NULL;
    }
    check_equal(store.puts, 0); check_equal(store.lists, 0);
  }

  it("preserves cancellation publication failures without readback or overwrite fallback") {
    const int errors[] = {SALTS_EIO, SALTS_ENOTSUP, SALTS_ENOSPC};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      int reads = store.reads, claims = store.claims;
      store.fail_claim = errors[i];
      check_equal(turbo_agent_job_request_cancel(job, 0, "request-1", "reason"), errors[i]);
      check_equal(store.reads - reads, TURBO_AGENT_JOB_MAX_REVISIONS + 1);
      check_equal(store.claims, claims + 1);
    }
    store.fail_claim = SALTS_EIO; store.after_claim = 1;
    int reads = store.reads;
    check_equal(turbo_agent_job_request_cancel(job, 0, "lost-ack", "reason"), SALTS_EIO);
    check_equal(store.reads - reads, TURBO_AGENT_JOB_MAX_REVISIONS + 1);
    store.fail_claim = 0;
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), "lost-ack");
    check_equal(turbo_agent_job_propose(job, 0, "{}", 2, "source"), SALTS_ECANCELED);
    check_equal(store.puts, 0); check_equal(store.lists, 0);
  }

  it("rejects stale cancellation requests and propagates read errors without publishing") {
    int claims = store.claims;
    check_equal(turbo_agent_job_request_cancel(job, 1, "request-1", "reason"), SALTS_EBUSY);
    store.fail_read = SALTS_EIO;
    check_equal(turbo_agent_job_request_cancel(job, 0, "request-1", "reason"), SALTS_EIO);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_EIO);
    check_null(record); check_equal(store.claims, claims);
    store.fail_read = 0;
  }

  it("validates cancellation shape, exact head join and all bounded identity fields") {
    const char *keys[] = {"kind", "job_id", "request_id", "reason_ref", "extra"};
    check_equal(turbo_agent_job_request_cancel(job, 0, "request-1", "reason"), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &original), SALTS_OK);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
      record = json_clone(original);
      check_equal(turbo_runtime_json_object_set_string(record, keys[i], i == 2 ? "../bad" : ""), 0);
      check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_cancellations", "job-1", record), SALTS_OK);
      check_equal(turbo_agent_job_get_cancellation(job, &snapshot), SALTS_EPROTO);
      check_null(snapshot);
      check_equal(turbo_agent_job_propose(job, 0, "{}", 2, "source"), SALTS_EPROTO);
      json_free(record); record = NULL;
    }
    record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(record, "job_revision", 1), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_cancellations", "job-1", record), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &snapshot), SALTS_EPROTO);
    json_free(record); record = json_clone(original);
    check_equal(turbo_runtime_json_object_set_int64(record, "schema_version", 2), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_cancellations", "job-1", record), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &snapshot), SALTS_ENOTSUP);
    check_equal(turbo_agent_job_propose(job, 0, "{}", 2, "source"), SALTS_ENOTSUP);
    json_free(record); record = json_clone(original);
    const char encoded[] = "\"x\\u0000y\"";
    check_equal(turbo_runtime_json_object_set(record, "reason_ref", json_parse(encoded, sizeof(encoded) - 1)), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_cancellations", "job-1", record), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &snapshot), SALTS_EPROTO);
  }

  it("refuses identity reuse when only a cancellation survives") {
    check_equal(turbo_agent_job_request_cancel(job, 0, "request-1", "reason"), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(record, "job_id", "orphan-cancel"), 0);
    check_equal(turbo_agent_runtime_store_put_json(runtime, "agent_job_cancellations", "orphan-cancel", record), SALTS_OK);
    int claims = store.claims;
    check_equal(turbo_agent_job_create(runtime, "orphan-cancel", "new-goal", "criteria", &other), SALTS_EALREADY);
    check_null(other); check_equal(store.claims, claims);
  }

  it("enforces cancellation API and byte limits before store access") {
    char request[66], reason[1026];
    memset(request, 'r', sizeof(request)); request[65] = 0;
    memset(reason, 'r', sizeof(reason)); reason[1025] = 0;
    int reads = store.reads, claims = store.claims;
    check_equal(turbo_agent_job_request_cancel(NULL, 0, "request", "reason"), SALTS_EINVAL);
    check_equal(turbo_agent_job_get_cancellation(NULL, &record), SALTS_EINVAL);
    check_null(record);
    check_equal(turbo_agent_job_get_cancellation(job, NULL), SALTS_EINVAL);
    check_equal(turbo_agent_job_request_cancel(job, 0, "../bad", "reason"), SALTS_EINVAL);
    check_equal(turbo_agent_job_request_cancel(job, 0, request, "reason"), SALTS_EINVAL);
    check_equal(turbo_agent_job_request_cancel(job, 0, "request", reason), SALTS_EINVAL);
    check_equal(turbo_agent_job_request_cancel(job, 0, "request", ""), SALTS_EINVAL);
    check_equal(store.reads, reads); check_equal(store.claims, claims);
    request[64] = 0; reason[1024] = 0;
    check_equal(turbo_agent_job_request_cancel(job, 0, request, reason), SALTS_OK);
    check_equal(turbo_agent_job_get_cancellation(job, &record), SALTS_OK);
    check_equal(json_get_string(record, "request_id"), request);
    check_equal(json_get_string(record, "reason_ref"), reason);
  }
}
