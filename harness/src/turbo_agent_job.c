#include "turbo_agent_job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOBS "agent_jobs"
#define REVISIONS "agent_job_revisions"

struct turbo_agent_job_s {
  turbo_agent_runtime_t *runtime;
  char id[65];
};

typedef struct job_head_s {
  json_value_t *descriptor;
  json_value_t *proposal;
  json_value_t *binding;
  uint32_t revision;
} job_head_t;

static int text_valid(const char *text, size_t max) {
  size_t i;
  if (!text || !text[0]) return 0;
  for (i = 0; i <= max; ++i) if (!text[i]) return 1;
  return 0;
}

static int id_valid(const char *text, size_t max) {
  size_t i;
  if (!text_valid(text, max)) return 0;
  for (i = 0; text[i]; ++i) {
    unsigned char ch = (unsigned char)text[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return 0;
  }
  return 1;
}

static const char *field_text(const json_value_t *record, const char *key, size_t max) {
  const json_value_t *value = json_object_get(record, key);
  const char *text;
  if (!value || json_type(value) != JSON_STRING || json_string_len(value) > max) return NULL;
  text = json_string(value);
  return text && json_string_len(value) == strlen(text) && text[0] ? text : NULL;
}

static int field_is(const json_value_t *record, const char *key, const char *text) {
  const char *field = field_text(record, key, strlen(text));
  return field && !strcmp(field, text);
}

static int64_t number(const json_value_t *record, const char *key) {
  return turbo_runtime_json_value_as_int64(json_object_get(record, key), -1);
}

static int schema_valid(const json_value_t *record) {
  int64_t schema;
  if (!record || json_type(record) != JSON_OBJECT) return SALTS_EPROTO;
  schema = number(record, "schema_version");
  return schema == TURBO_AGENT_JOB_SCHEMA_VERSION ? SALTS_OK :
      schema > 0 ? SALTS_ENOTSUP : SALTS_EPROTO;
}

static void head_destroy(job_head_t *head) {
  json_free(head->descriptor);
  json_free(head->proposal);
  json_free(head->binding);
  memset(head, 0, sizeof(*head));
}

static void revision_key(const turbo_agent_job_t *job, uint32_t revision, char key[80]) {
  /* Fixed-width ordinal suffix is unambiguous even for IDs containing '_'. */
  (void)snprintf(key, 80, "%s_%03u", job->id, (unsigned)revision);
}

static int head_read(const turbo_agent_job_t *job, job_head_t *head) {
  json_value_t *record = NULL;
  char key[80];
  uint32_t i;
  int gap = 0;
  int rc;
  memset(head, 0, sizeof(*head));
  rc = turbo_agent_runtime_store_get_json_status(job->runtime, JOBS, job->id,
                                                 &head->descriptor);
  if (rc != SALTS_OK) goto fail;
  rc = schema_valid(head->descriptor);
  if (rc != SALTS_OK) goto fail;
  if (json_object_size(head->descriptor) != 5 ||
      !field_is(head->descriptor, "id", job->id) ||
      !field_text(head->descriptor, "goal", 4096) ||
      !field_text(head->descriptor, "acceptance_criteria", 4096) ||
      number(head->descriptor, "max_revisions") != TURBO_AGENT_JOB_MAX_REVISIONS) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  for (i = 1; i <= TURBO_AGENT_JOB_MAX_REVISIONS; ++i) {
    revision_key(job, i, key);
    rc = turbo_agent_runtime_store_get_json_status(job->runtime, REVISIONS, key, &record);
    if (rc == SALTS_ENOENT) { gap = 1; continue; }
    if (rc != SALTS_OK) goto fail;
    rc = schema_valid(record);
    if (rc != SALTS_OK) goto fail;
    if (gap || head->binding || !field_is(record, "job_id", job->id) ||
        number(record, "revision") != i || number(record, "previous_revision") != i - 1) {
      rc = SALTS_EPROTO;
      goto fail;
    }
    if (field_is(record, "kind", "proposal")) {
      if (json_object_size(record) != 7 ||
          !field_text(record, "source_json", TURBO_AGENT_JOB_MAX_SOURCE_BYTES) ||
          !field_text(record, "source_revision", 128)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      json_free(head->proposal);
      head->proposal = record;
    } else if (field_is(record, "kind", "attempt_bound")) {
      const char *thread = field_text(record, "thread_id", 80);
      const char *attempt = field_text(record, "attempt_id", 64);
      if (json_object_size(record) != 8 || !head->proposal ||
          number(record, "proposal_version") != number(head->proposal, "revision") ||
          !id_valid(thread, 80) || !id_valid(attempt, 64)) {
        rc = SALTS_EPROTO;
        goto fail;
      }
      head->binding = record;
    } else {
      rc = SALTS_EPROTO;
      goto fail;
    }
    record = NULL;
    head->revision = i;
  }
  return SALTS_OK;
fail:
  json_free(record);
  head_destroy(head);
  return rc;
}

/* Refuse reuse when a descriptor has been externally lost but its history
 * survives. This is a preflight, never a substitute for the atomic claim. */
static int history_absent(const turbo_agent_job_t *job) {
  uint32_t i;
  char key[80];
  for (i = 1; i <= TURBO_AGENT_JOB_MAX_REVISIONS; ++i) {
    json_value_t *record = NULL;
    int rc;
    revision_key(job, i, key);
    rc = turbo_agent_runtime_store_get_json_status(job->runtime, REVISIONS, key, &record);
    json_free(record);
    if (rc != SALTS_ENOENT) return rc == SALTS_OK ? SALTS_EALREADY : rc;
  }
  return SALTS_OK;
}

static turbo_agent_job_t *job_allocate(turbo_agent_runtime_t *runtime, const char *id) {
  turbo_agent_job_t *job = malloc(sizeof(*job));
  if (job) {
    job->runtime = runtime;
    memcpy(job->id, id, strlen(id) + 1);
  }
  return job;
}

int turbo_agent_job_create(turbo_agent_runtime_t *runtime, const char *id,
    const char *goal, const char *criteria, turbo_agent_job_t **out_job) {
  turbo_agent_job_t *job;
  json_value_t *record;
  int rc = SALTS_ENOMEM;
  if (!out_job) return SALTS_EINVAL;
  *out_job = NULL;
  if (!runtime || !id_valid(id, 64) || !text_valid(goal, 4096) ||
      !text_valid(criteria, 4096)) return SALTS_EINVAL;
  job = job_allocate(runtime, id);
  if (!job) return SALTS_ENOMEM;
  rc = history_absent(job);
  if (rc != SALTS_OK) { free(job); return rc; }
  rc = SALTS_ENOMEM;
  record = json_create_object();
  if (record && !turbo_runtime_json_object_set_int64(record, "schema_version", TURBO_AGENT_JOB_SCHEMA_VERSION) &&
      !turbo_runtime_json_object_set_string(record, "id", id) &&
      !turbo_runtime_json_object_set_string(record, "goal", goal) &&
      !turbo_runtime_json_object_set_string(record, "acceptance_criteria", criteria) &&
      !turbo_runtime_json_object_set_int64(record, "max_revisions", TURBO_AGENT_JOB_MAX_REVISIONS))
    rc = turbo_agent_runtime_store_create_json_if_absent(runtime, JOBS, id, record);
  json_free(record);
  if (rc != SALTS_OK) { free(job); return rc; }
  *out_job = job;
  return SALTS_OK;
}

int turbo_agent_job_open(turbo_agent_runtime_t *runtime, const char *id,
    turbo_agent_job_t **out_job) {
  turbo_agent_job_t *job;
  job_head_t head;
  int rc;
  if (!out_job) return SALTS_EINVAL;
  *out_job = NULL;
  if (!runtime || !id_valid(id, 64)) return SALTS_EINVAL;
  job = job_allocate(runtime, id);
  if (!job) return SALTS_ENOMEM;
  rc = head_read(job, &head);
  if (rc != SALTS_OK) { free(job); return rc; }
  head_destroy(&head);
  *out_job = job;
  return SALTS_OK;
}

void turbo_agent_job_destroy(turbo_agent_job_t *job) { free(job); }

int turbo_agent_job_get_snapshot(const turbo_agent_job_t *job, json_value_t **out) {
  job_head_t head;
  json_value_t *snapshot;
  int rc;
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!job) return SALTS_EINVAL;
  rc = head_read(job, &head);
  if (rc != SALTS_OK) return rc;
  snapshot = head.descriptor;
  head.descriptor = NULL;
  if (turbo_runtime_json_object_set_int64(snapshot, "revision", head.revision) ||
      turbo_runtime_json_object_set_string(snapshot, "state", head.binding ?
          "attempt_bound" : head.proposal ? "proposed" : "created") ||
      (head.proposal && turbo_runtime_json_object_set_clone(snapshot, "proposal", head.proposal)) ||
      (head.binding && turbo_runtime_json_object_set_clone(snapshot, "attempt", head.binding))) {
    json_free(snapshot);
    head_destroy(&head);
    return SALTS_ENOMEM;
  }
  head_destroy(&head);
  *out = snapshot;
  return SALTS_OK;
}

static int event_create(turbo_agent_job_t *job, uint32_t revision, const char *kind,
    json_value_t **out) {
  json_value_t *event = json_create_object();
  if (!event || turbo_runtime_json_object_set_int64(event, "schema_version", TURBO_AGENT_JOB_SCHEMA_VERSION) ||
      turbo_runtime_json_object_set_string(event, "job_id", job->id) ||
      turbo_runtime_json_object_set_int64(event, "revision", revision) ||
      turbo_runtime_json_object_set_int64(event, "previous_revision", revision - 1) ||
      turbo_runtime_json_object_set_string(event, "kind", kind)) {
    json_free(event);
    return SALTS_ENOMEM;
  }
  *out = event;
  return SALTS_OK;
}

static int append_admit(turbo_agent_job_t *job, uint32_t expected, int binding,
    job_head_t *head) {
  int rc = head_read(job, head);
  if (rc != SALTS_OK) return rc;
  if (head->revision != expected || head->binding || (binding && !head->proposal))
    rc = SALTS_EBUSY;
  else if (head->revision == TURBO_AGENT_JOB_MAX_REVISIONS) rc = SALTS_ENOSPC;
  if (rc != SALTS_OK) head_destroy(head);
  return rc;
}

int turbo_agent_job_propose(turbo_agent_job_t *job, uint32_t expected,
    const char *source, size_t size, const char *source_revision) {
  job_head_t head;
  json_value_t *event = NULL;
  char key[80];
  char *copy;
  int rc;
  if (!job || !source || !size || size > TURBO_AGENT_JOB_MAX_SOURCE_BYTES ||
      memchr(source, '\0', size) || !text_valid(source_revision, 128)) return SALTS_EINVAL;
  rc = append_admit(job, expected, 0, &head);
  if (rc != SALTS_OK) return rc;
  head_destroy(&head);
  copy = malloc(size + 1);
  if (!copy) return SALTS_ENOMEM;
  memcpy(copy, source, size);
  copy[size] = '\0';
  rc = event_create(job, expected + 1, "proposal", &event);
  if (rc == SALTS_OK && (turbo_runtime_json_object_set_string(event, "source_json", copy) ||
      turbo_runtime_json_object_set_string(event, "source_revision", source_revision))) rc = SALTS_ENOMEM;
  free(copy);
  revision_key(job, expected + 1, key);
  if (rc == SALTS_OK)
    rc = turbo_agent_runtime_store_create_json_if_absent(job->runtime, REVISIONS, key, event);
  json_free(event);
  return rc;
}

int turbo_agent_job_bind_compiled_inspect(turbo_agent_job_t *job, uint32_t expected,
    const char *thread, const char *attempt) {
  job_head_t head;
  json_value_t *event = NULL;
  char key[80];
  int rc;
  if (!job || !id_valid(thread, 80) || !id_valid(attempt, 64)) return SALTS_EINVAL;
  rc = append_admit(job, expected, 1, &head);
  if (rc != SALTS_OK) return rc;
  rc = event_create(job, expected + 1, "attempt_bound", &event);
  if (rc == SALTS_OK &&
      (turbo_runtime_json_object_set_int64(event, "proposal_version", number(head.proposal, "revision")) ||
       turbo_runtime_json_object_set_string(event, "thread_id", thread) ||
       turbo_runtime_json_object_set_string(event, "attempt_id", attempt))) rc = SALTS_ENOMEM;
  head_destroy(&head);
  revision_key(job, expected + 1, key);
  if (rc == SALTS_OK)
    rc = turbo_agent_runtime_store_create_json_if_absent(job->runtime, REVISIONS, key, event);
  json_free(event);
  return rc;
}
