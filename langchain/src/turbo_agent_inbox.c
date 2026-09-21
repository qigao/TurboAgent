#include "turbo_agent_inbox_internal.h"
#include "turbo_agent_runtime_v1_internal.h"

#include <turbo_deque.h>
#include <tstr.h>
#include <salts/thread.h>
#include <salts_uuid.h>

#include <stdlib.h>
#include <string.h>

#define TURBO_AGENT_INBOX_PAYLOADS_COLLECTION "agent_inbox_payloads"
#define TURBO_AGENT_INBOX_TRANSITIONS_COLLECTION "agent_inbox_transitions"
#define TURBO_AGENT_INBOX_SCHEMA_VERSION 1
#define TURBO_AGENT_INBOX_MS_TO_NS UINT64_C(1000000)
#define TURBO_AGENT_INBOX_WAIT_SLICE_MS (UINT32_MAX - UINT64_C(1))
#define TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER UINT64_C(9007199254740991)

typedef struct turbo_agent_inbox_ready_s {
  tstr_t inbox_id;
  turbo_agent_inbox_kind_t kind;
  size_t payload_bytes;
  uint64_t sequence;
  uint64_t transition_seq;
} turbo_agent_inbox_ready_t;

struct turbo_agent_inbox_s {
  turbo_agent_runtime_t *runtime;
  tstr_t thread_id;
  turbo_agent_inbox_config_t config;
  salts_mutex_t mutex;
  salts_cond_t changed;
  turbo_deque_t ready;
  size_t used_items;
  size_t used_bytes;
  size_t reserved_items;
  size_t reserved_bytes;
  uint64_t next_sequence;
  int closed;
  int has_active_claim;
  turbo_agent_inbox_ready_t active_claim;
  tstr_t active_event_id;
};

static int turbo_agent_inbox_kind_valid(turbo_agent_inbox_kind_t kind) {
  return kind == TURBO_AGENT_INBOX_STEER || kind == TURBO_AGENT_INBOX_FOLLOW_UP;
}

static const char *turbo_agent_inbox_kind_text(turbo_agent_inbox_kind_t kind) {
  return kind == TURBO_AGENT_INBOX_STEER ? "steer" : "follow_up";
}

static turbo_agent_inbox_kind_t turbo_agent_inbox_kind_from_text(const char *kind) {
  if (kind && strcmp(kind, "steer") == 0) return TURBO_AGENT_INBOX_STEER;
  if (kind && strcmp(kind, "follow_up") == 0) return TURBO_AGENT_INBOX_FOLLOW_UP;
  return 0;
}

static const char *turbo_agent_inbox_status_text(turbo_agent_inbox_status_t status) {
  switch (status) {
  case TURBO_AGENT_INBOX_QUEUED:
    return "queued";
  case TURBO_AGENT_INBOX_CLAIMED:
    return "claimed";
  case TURBO_AGENT_INBOX_APPLIED:
    return "applied";
  default:
    return "unknown";
  }
}

static turbo_agent_inbox_status_t turbo_agent_inbox_status_from_text(const char *status) {
  if (status && strcmp(status, "queued") == 0) return TURBO_AGENT_INBOX_QUEUED;
  if (status && strcmp(status, "claimed") == 0) return TURBO_AGENT_INBOX_CLAIMED;
  if (status && strcmp(status, "applied") == 0) return TURBO_AGENT_INBOX_APPLIED;
  return 0;
}

static uint64_t turbo_agent_inbox_saturating_add(uint64_t left, uint64_t right) {
  return UINT64_MAX - left < right ? UINT64_MAX : left + right;
}

static int turbo_agent_inbox_make_uuid(char out_id[SALTS_UUID_STRING_SIZE]) {
  salts_uuid_t uuid;
  if (salts_uuid_v7_generate(&uuid) != SALTS_OK) return SALTS_EIO;
  return salts_uuid_format(&uuid, out_id, SALTS_UUID_STRING_SIZE) == SALTS_OK ? SALTS_OK
                                                                              : SALTS_EIO;
}

static void turbo_agent_inbox_ready_reset(turbo_agent_inbox_ready_t *entry) {
  if (!entry) return;
  tstr_free(entry->inbox_id);
  memset(entry, 0, sizeof(*entry));
}

static int turbo_agent_inbox_ready_insert(turbo_agent_inbox_t *inbox,
                                          turbo_agent_inbox_ready_t *entry) {
  size_t index;
  if (turbo_deque_push_back(&inbox->ready, entry) != SALTS_OK) return SALTS_ENOMEM;
  memset(entry, 0, sizeof(*entry));
  index = turbo_deque_size(&inbox->ready) - 1;
  while (index > 0) {
    turbo_agent_inbox_ready_t *current =
        (turbo_agent_inbox_ready_t *)turbo_deque_at(&inbox->ready, index);
    turbo_agent_inbox_ready_t *previous =
        (turbo_agent_inbox_ready_t *)turbo_deque_at(&inbox->ready, index - 1);
    turbo_agent_inbox_ready_t swap;
    if (!current || !previous || previous->sequence <= current->sequence) break;
    swap = *previous;
    *previous = *current;
    *current = swap;
    --index;
  }
  return SALTS_OK;
}

static int turbo_agent_inbox_ready_take_kind(turbo_agent_inbox_t *inbox,
                                             turbo_agent_inbox_kind_t kind,
                                             turbo_agent_inbox_ready_t *out_entry) {
  size_t count = turbo_deque_size(&inbox->ready);
  size_t index;
  memset(out_entry, 0, sizeof(*out_entry));
  for (index = 0; index < count; ++index) {
    turbo_agent_inbox_ready_t entry;
    if (turbo_deque_pop_front(&inbox->ready, &entry) != SALTS_OK) return SALTS_EIO;
    if (entry.kind == kind) {
      *out_entry = entry;
      return SALTS_OK;
    }
    if (turbo_deque_push_back(&inbox->ready, &entry) != SALTS_OK) {
      turbo_agent_inbox_ready_reset(&entry);
      return SALTS_EIO;
    }
  }
  return SALTS_ENOENT;
}

static int turbo_agent_inbox_has_capacity(const turbo_agent_inbox_t *inbox, size_t payload_bytes) {
  size_t items = inbox->used_items + inbox->reserved_items;
  size_t bytes = inbox->used_bytes + inbox->reserved_bytes;
  return items < inbox->config.max_items && payload_bytes <= inbox->config.max_total_bytes - bytes;
}

static int turbo_agent_inbox_wait_for_capacity(turbo_agent_inbox_t *inbox, size_t payload_bytes,
                                               uint64_t timeout_ms) {
  uint64_t deadline = UINT64_MAX;
  int timed = timeout_ms != TURBO_AGENT_INBOX_WAIT_INFINITE;
  if (timed) deadline = turbo_agent_inbox_saturating_add(turbo_monotonic_ms(), timeout_ms);
  while (!inbox->closed && !turbo_agent_inbox_has_capacity(inbox, payload_bytes)) {
    uint64_t now;
    uint64_t wait_ms;
    if (!timed) {
      salts_cond_wait(&inbox->changed, &inbox->mutex);
      continue;
    }
    now = turbo_monotonic_ms();
    if (now >= deadline) return SALTS_EBUSY;
    wait_ms = deadline - now;
    if (wait_ms > TURBO_AGENT_INBOX_WAIT_SLICE_MS) wait_ms = TURBO_AGENT_INBOX_WAIT_SLICE_MS;
    (void)salts_cond_timedwait(&inbox->changed, &inbox->mutex,
                               wait_ms * TURBO_AGENT_INBOX_MS_TO_NS);
  }
  return inbox->closed ? SALTS_ECANCELED : SALTS_OK;
}

static int turbo_agent_inbox_build_transition(const turbo_agent_inbox_t *inbox,
                                              const char *inbox_id,
                                              turbo_agent_inbox_status_t status, const char *action,
                                              uint64_t transition_seq, const char *run_id,
                                              const char *event_id, json_value_t **out_transition) {
  char transition_id[SALTS_UUID_STRING_SIZE];
  char timestamp[32];
  json_value_t *transition;
  *out_transition = NULL;
  if (turbo_agent_inbox_make_uuid(transition_id) != SALTS_OK ||
      turbo_agent_runtime_make_timestamp(timestamp, sizeof(timestamp)) != 0)
    return SALTS_EIO;
  transition = turbo_json_create_object();
  if (!transition) return SALTS_ENOMEM;
  turbo_json_object_set_number(transition, "schema_version", TURBO_AGENT_INBOX_SCHEMA_VERSION);
  turbo_json_object_set_string(transition, "transition_id", transition_id);
  turbo_json_object_set_string(transition, "inbox_id", inbox_id);
  turbo_json_object_set_string(transition, "thread_id", inbox->thread_id);
  turbo_json_object_set_string(transition, "status", turbo_agent_inbox_status_text(status));
  turbo_json_object_set_string(transition, "action", action);
  turbo_json_object_set_number(transition, "transition_seq", (double)transition_seq);
  turbo_json_object_set_string(transition, "created_at", timestamp);
  if (run_id && run_id[0] != '\0')
    turbo_json_object_set_string(transition, "claimed_by_run_id", run_id);
  else turbo_json_object_set_null(transition, "claimed_by_run_id");
  if (event_id && event_id[0] != '\0')
    turbo_json_object_set_string(transition, "applied_event_id", event_id);
  else turbo_json_object_set_null(transition, "applied_event_id");
  *out_transition = transition;
  return SALTS_OK;
}

static int turbo_agent_inbox_persist_transition(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                                turbo_agent_inbox_status_t status,
                                                const char *action, uint64_t transition_seq,
                                                const char *run_id, const char *event_id) {
  json_value_t *transition = NULL;
  const char *transition_id;
  int rc = turbo_agent_inbox_build_transition(inbox, inbox_id, status, action, transition_seq,
                                              run_id, event_id, &transition);
  if (rc != SALTS_OK) return rc;
  transition_id = turbo_json_get_string(transition, "transition_id");
  rc = turbo_agent_runtime_store_put_json(inbox->runtime, TURBO_AGENT_INBOX_TRANSITIONS_COLLECTION,
                                          transition_id, transition) == 0
           ? SALTS_OK
           : SALTS_EIO;
  turbo_runtime_json_destroy(transition);
  return rc;
}

static int turbo_agent_inbox_latest_transition(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                               json_value_t **out_transition) {
  json_value_t *transitions = NULL;
  const json_value_t *latest = NULL;
  uint64_t latest_seq = 0;
  size_t index;
  *out_transition = NULL;
  if (turbo_agent_runtime_store_list_json(inbox->runtime, TURBO_AGENT_INBOX_TRANSITIONS_COLLECTION,
                                          "inbox_id", inbox_id, &transitions) != 0)
    return SALTS_ENOENT;
  for (index = 0; index < turbo_json_array_size(transitions); ++index) {
    const json_value_t *candidate = turbo_json_array_get(transitions, index);
    double value = turbo_json_get_double(candidate, "transition_seq", 0);
    uint64_t seq = value > 0 ? (uint64_t)value : 0;
    if (seq > latest_seq) {
      latest = candidate;
      latest_seq = seq;
    }
  }
  if (latest) *out_transition = turbo_json_clone(latest);
  turbo_runtime_json_destroy(transitions);
  return *out_transition ? SALTS_OK : SALTS_ENOENT;
}

static int turbo_agent_inbox_load_ready(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                        turbo_agent_inbox_ready_t *out_entry,
                                        turbo_agent_inbox_status_t *out_status) {
  json_value_t *payload = NULL;
  json_value_t *transition = NULL;
  const char *thread_id;
  double bytes_value, sequence_value, transition_value;
  int rc = SALTS_ENOENT;
  memset(out_entry, 0, sizeof(*out_entry));
  if (turbo_agent_runtime_store_get_json(inbox->runtime, TURBO_AGENT_INBOX_PAYLOADS_COLLECTION,
                                         inbox_id, &payload) != 0) {
    rc = SALTS_EIO;
    goto cleanup;
  }
  rc = turbo_agent_inbox_latest_transition(inbox, inbox_id, &transition);
  if (rc != SALTS_OK) goto cleanup;
  rc = SALTS_EIO;
  thread_id = turbo_json_get_string(payload, "thread_id");
  out_entry->kind = turbo_agent_inbox_kind_from_text(turbo_json_get_string(payload, "kind"));
  *out_status = turbo_agent_inbox_status_from_text(turbo_json_get_string(transition, "status"));
  bytes_value = turbo_json_get_double(payload, "payload_bytes", -1);
  sequence_value = turbo_json_get_double(payload, "sequence", -1);
  transition_value = turbo_json_get_double(transition, "transition_seq", -1);
  if (!thread_id || strcmp(thread_id, inbox->thread_id) != 0 ||
      !turbo_json_get_string(transition, "thread_id") ||
      strcmp(turbo_json_get_string(transition, "thread_id"), inbox->thread_id) != 0 ||
      !turbo_agent_inbox_kind_valid(out_entry->kind) || !*out_status || bytes_value < 0 ||
      bytes_value > (double)TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER ||
      bytes_value != (double)(size_t)bytes_value || sequence_value < 1 ||
      sequence_value > (double)TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER ||
      sequence_value != (double)(uint64_t)sequence_value || transition_value < 1 ||
      transition_value > (double)TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER ||
      transition_value != (double)(uint64_t)transition_value)
    goto cleanup;
  out_entry->inbox_id = tstr_dup(inbox_id);
  if (!out_entry->inbox_id) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  out_entry->payload_bytes = (size_t)bytes_value;
  out_entry->sequence = (uint64_t)sequence_value;
  out_entry->transition_seq = (uint64_t)transition_value;
  rc = SALTS_OK;
cleanup:
  turbo_runtime_json_destroy(transition);
  turbo_runtime_json_destroy(payload);
  if (rc != SALTS_OK) turbo_agent_inbox_ready_reset(out_entry);
  return rc;
}

static int turbo_agent_inbox_recover(turbo_agent_inbox_t *inbox) {
  json_value_t *payloads = NULL;
  json_value_t *runs = NULL;
  json_value_t *events = NULL;
  size_t index;
  if (turbo_agent_runtime_store_list_json(inbox->runtime, TURBO_AGENT_INBOX_PAYLOADS_COLLECTION,
                                          "thread_id", inbox->thread_id, &payloads) != 0)
    return SALTS_EIO;
  for (index = 0; index < turbo_json_array_size(payloads); ++index) {
    const json_value_t *payload = turbo_json_array_get(payloads, index);
    const char *inbox_id = turbo_json_get_string(payload, "inbox_id");
    turbo_agent_inbox_ready_t entry;
    turbo_agent_inbox_status_t status = 0;
    int rc;
    if (!inbox_id) {
      turbo_runtime_json_destroy(events);
      turbo_runtime_json_destroy(runs);
      turbo_runtime_json_destroy(payloads);
      return SALTS_EIO;
    }
    rc = turbo_agent_inbox_load_ready(inbox, inbox_id, &entry, &status);
    /* A payload without a transition is an uncommitted enqueue left by a
     * failed store write. It never became queue-visible and is safe to ignore. */
    if (rc == SALTS_ENOENT) continue;
    if (rc != SALTS_OK) {
      turbo_runtime_json_destroy(events);
      turbo_runtime_json_destroy(runs);
      turbo_runtime_json_destroy(payloads);
      return rc;
    }
    if (entry.sequence >= inbox->next_sequence) inbox->next_sequence = entry.sequence + 1;
    if (status == TURBO_AGENT_INBOX_CLAIMED) {
      const char *event_id = NULL;
      const json_value_t *state_events = NULL;
      size_t event_index;
      if (entry.transition_seq >= TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return SALTS_ERANGE;
      }
      if (!runs && turbo_agent_runtime_list_runs(inbox->runtime, inbox->thread_id, &runs) != 0) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return SALTS_EIO;
      }
      if (turbo_json_array_size(runs) > 0 && !events &&
          turbo_agent_runtime_get_thread_head_state_json_value(inbox->runtime, inbox->thread_id,
                                                               &events) != 0) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return SALTS_EIO;
      }
      state_events = events ? turbo_json_object_get(events, "events") : NULL;
      for (event_index = 0; state_events && event_index < turbo_json_array_size(state_events);
           ++event_index) {
        const json_value_t *event = turbo_json_array_get(state_events, event_index);
        if (event &&
            strcmp(turbo_json_get_string(event, "kind") ? turbo_json_get_string(event, "kind") : "",
                   "inbox_message") == 0 &&
            strcmp(turbo_json_get_string(event, "inbox_id")
                       ? turbo_json_get_string(event, "inbox_id")
                       : "",
                   inbox_id) == 0) {
          event_id = turbo_json_get_string(event, "event_id");
          break;
        }
      }
      if (event_id && event_id[0] != '\0') {
        rc = turbo_agent_inbox_persist_transition(inbox, inbox_id, TURBO_AGENT_INBOX_APPLIED,
                                                  "recovered_applied", entry.transition_seq + 1,
                                                  NULL, event_id);
        turbo_agent_inbox_ready_reset(&entry);
        if (rc != SALTS_OK) {
          turbo_runtime_json_destroy(events);
          turbo_runtime_json_destroy(runs);
          turbo_runtime_json_destroy(payloads);
          return rc;
        }
        continue;
      }
      rc = turbo_agent_inbox_persist_transition(inbox, inbox_id, TURBO_AGENT_INBOX_QUEUED,
                                                "recovered_requeue", entry.transition_seq + 1, NULL,
                                                NULL);
      if (rc != SALTS_OK) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return rc;
      }
      ++entry.transition_seq;
      status = TURBO_AGENT_INBOX_QUEUED;
    }
    if (status != TURBO_AGENT_INBOX_APPLIED) {
      if (inbox->used_items == inbox->config.max_items ||
          entry.payload_bytes > inbox->config.max_total_bytes - inbox->used_bytes) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return SALTS_EBUSY;
      }
      ++inbox->used_items;
      inbox->used_bytes += entry.payload_bytes;
    }
    if (status == TURBO_AGENT_INBOX_QUEUED) {
      rc = turbo_agent_inbox_ready_insert(inbox, &entry);
      if (rc != SALTS_OK) {
        turbo_agent_inbox_ready_reset(&entry);
        turbo_runtime_json_destroy(events);
        turbo_runtime_json_destroy(runs);
        turbo_runtime_json_destroy(payloads);
        return rc;
      }
    } else turbo_agent_inbox_ready_reset(&entry);
  }
  turbo_runtime_json_destroy(events);
  turbo_runtime_json_destroy(runs);
  turbo_runtime_json_destroy(payloads);
  return SALTS_OK;
}

int turbo_agent_inbox_create(turbo_agent_runtime_t *runtime, const char *thread_id,
                             const turbo_agent_inbox_config_t *config,
                             turbo_agent_inbox_t **out_inbox) {
  turbo_agent_inbox_t *inbox;
  int rc;
  if (!out_inbox) return SALTS_EINVAL;
  *out_inbox = NULL;
  if (!runtime || !thread_id || thread_id[0] == '\0' || !config ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != TURBO_AGENT_INBOX_ABI_VERSION || config->max_items == 0 ||
      config->max_total_bytes == 0 || config->max_item_bytes == 0 ||
      config->max_item_bytes > config->max_total_bytes ||
      config->max_follow_ups_per_execution == 0 ||
      config->max_items > TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER ||
      config->max_total_bytes > TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER)
    return SALTS_EINVAL;
  inbox = (turbo_agent_inbox_t *)calloc(1, sizeof(*inbox));
  if (!inbox) return SALTS_ENOMEM;
  inbox->runtime = runtime;
  inbox->thread_id = tstr_dup(thread_id);
  inbox->config = *config;
  inbox->next_sequence = 1;
  salts_mutex_init(&inbox->mutex);
  salts_cond_init(&inbox->changed);
  if (!inbox->thread_id || !inbox->mutex || !inbox->changed) {
    turbo_agent_inbox_destroy(inbox);
    return SALTS_ENOMEM;
  }
  if (turbo_deque_init(&inbox->ready, sizeof(turbo_agent_inbox_ready_t)) != SALTS_OK ||
      turbo_deque_reserve(&inbox->ready, config->max_items) != SALTS_OK) {
    turbo_agent_inbox_destroy(inbox);
    return SALTS_ENOMEM;
  }
  rc = turbo_agent_inbox_recover(inbox);
  if (rc != SALTS_OK) {
    turbo_agent_inbox_destroy(inbox);
    return rc;
  }
  *out_inbox = inbox;
  return SALTS_OK;
}

void turbo_agent_inbox_destroy(turbo_agent_inbox_t *inbox) {
  turbo_agent_inbox_ready_t entry;
  if (!inbox) return;
  while (turbo_deque_pop_front(&inbox->ready, &entry) == SALTS_OK)
    turbo_agent_inbox_ready_reset(&entry);
  turbo_agent_inbox_ready_reset(&inbox->active_claim);
  tstr_free(inbox->active_event_id);
  turbo_deque_destroy(&inbox->ready);
  salts_cond_destroy(&inbox->changed);
  salts_mutex_destroy(&inbox->mutex);
  tstr_free(inbox->thread_id);
  free(inbox);
}

int turbo_agent_inbox_enqueue(turbo_agent_inbox_t *inbox, turbo_agent_inbox_kind_t kind,
                              const json_value_t *message, uint64_t timeout_ms,
                              char **out_inbox_id) {
  char inbox_id[SALTS_UUID_STRING_SIZE];
  char timestamp[32];
  char *payload_text = NULL, *caller_id = NULL;
  json_value_t *payload_copy = NULL, *record = NULL;
  turbo_agent_inbox_ready_t entry = {0};
  size_t payload_bytes = 0;
  uint64_t sequence = 0;
  int reserved = 0, rc = SALTS_EIO;
  if (!out_inbox_id) return SALTS_EINVAL;
  *out_inbox_id = NULL;
  if (!inbox || !message || !turbo_agent_inbox_kind_valid(kind)) return SALTS_EINVAL;
  if (turbo_json_type(message) != TURBO_JSON_OBJECT || !turbo_json_get_string(message, "role") ||
      strcmp(turbo_json_get_string(message, "role"), "user") != 0 ||
      !turbo_json_get_string(message, "content") ||
      turbo_json_get_string(message, "content")[0] == '\0') {
    return SALTS_EINVAL;
  }
  payload_text = turbo_json_serialize(message, &payload_bytes);
  if (!payload_text) return SALTS_EINVAL;
  if (payload_bytes == 0 || payload_bytes > inbox->config.max_item_bytes ||
      payload_bytes > inbox->config.max_total_bytes) {
    rc = SALTS_ERANGE;
    goto cleanup;
  }
  if (turbo_agent_runtime_parse_json_string(payload_text, &payload_copy) != 0 ||
      turbo_agent_inbox_make_uuid(inbox_id) != SALTS_OK ||
      turbo_agent_runtime_make_timestamp(timestamp, sizeof(timestamp)) != 0)
    goto cleanup;
  caller_id = (char *)malloc(strlen(inbox_id) + 1);
  entry.inbox_id = tstr_dup(inbox_id);
  if (!caller_id || !entry.inbox_id) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  memcpy(caller_id, inbox_id, strlen(inbox_id) + 1);
  salts_mutex_lock(&inbox->mutex);
  rc = turbo_agent_inbox_wait_for_capacity(inbox, payload_bytes, timeout_ms);
  if (rc == SALTS_OK && inbox->next_sequence > TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER) {
    rc = SALTS_ERANGE;
  }
  if (rc == SALTS_OK) {
    ++inbox->reserved_items;
    inbox->reserved_bytes += payload_bytes;
    sequence = inbox->next_sequence++;
    reserved = 1;
  }
  salts_mutex_unlock(&inbox->mutex);
  if (rc != SALTS_OK) goto cleanup;
  record = turbo_json_create_object();
  if (!record) {
    rc = SALTS_ENOMEM;
    goto cleanup;
  }
  turbo_json_object_set_number(record, "schema_version", TURBO_AGENT_INBOX_SCHEMA_VERSION);
  turbo_json_object_set_string(record, "inbox_id", inbox_id);
  turbo_json_object_set_string(record, "thread_id", inbox->thread_id);
  turbo_json_object_set_null(record, "execution_id");
  turbo_json_object_set_string(record, "kind", turbo_agent_inbox_kind_text(kind));
  turbo_json_object_set_number(record, "payload_bytes", (double)payload_bytes);
  turbo_json_object_set_number(record, "sequence", (double)sequence);
  turbo_json_object_set_string(record, "created_at", timestamp);
  if (turbo_agent_runtime_json_value_object_set_clone(record, "payload", payload_copy) != 0 ||
      turbo_agent_runtime_store_put_json(inbox->runtime, TURBO_AGENT_INBOX_PAYLOADS_COLLECTION,
                                         inbox_id, record) != 0 ||
      turbo_agent_inbox_persist_transition(inbox, inbox_id, TURBO_AGENT_INBOX_QUEUED, "enqueued", 1,
                                           NULL, NULL) != SALTS_OK) {
    rc = SALTS_EIO;
    goto cleanup;
  }
  entry.kind = kind;
  entry.payload_bytes = payload_bytes;
  entry.sequence = sequence;
  entry.transition_seq = 1;
  salts_mutex_lock(&inbox->mutex);
  --inbox->reserved_items;
  inbox->reserved_bytes -= payload_bytes;
  reserved = 0;
  rc = turbo_agent_inbox_ready_insert(inbox, &entry);
  if (rc == SALTS_OK) {
    ++inbox->used_items;
    inbox->used_bytes += payload_bytes;
    salts_cond_broadcast(&inbox->changed);
  }
  salts_mutex_unlock(&inbox->mutex);
  if (rc != SALTS_OK) goto cleanup;
  *out_inbox_id = caller_id;
  caller_id = NULL;
cleanup:
  if (reserved) {
    salts_mutex_lock(&inbox->mutex);
    --inbox->reserved_items;
    inbox->reserved_bytes -= payload_bytes;
    salts_cond_broadcast(&inbox->changed);
    salts_mutex_unlock(&inbox->mutex);
  }
  turbo_agent_inbox_ready_reset(&entry);
  free(caller_id);
  turbo_runtime_json_destroy(record);
  turbo_runtime_json_destroy(payload_copy);
  turbo_json_serialize_free(payload_text);
  return rc;
}

int turbo_agent_inbox_status(turbo_agent_inbox_t *inbox, const char *inbox_id,
                             json_value_t **out_status) {
  json_value_t *payload = NULL, *transition = NULL;
  const char *status;
  if (!out_status) return SALTS_EINVAL;
  *out_status = NULL;
  if (!inbox || !inbox_id || inbox_id[0] == '\0' ||
      turbo_agent_runtime_store_get_json(inbox->runtime, TURBO_AGENT_INBOX_PAYLOADS_COLLECTION,
                                         inbox_id, &payload) != 0 ||
      turbo_agent_inbox_latest_transition(inbox, inbox_id, &transition) != SALTS_OK) {
    turbo_runtime_json_destroy(payload);
    turbo_runtime_json_destroy(transition);
    return SALTS_ENOENT;
  }
  status = turbo_json_get_string(transition, "status");
  if (!status || !turbo_json_get_string(payload, "thread_id") ||
      strcmp(turbo_json_get_string(payload, "thread_id"), inbox->thread_id) != 0 ||
      !turbo_json_get_string(transition, "thread_id") ||
      strcmp(turbo_json_get_string(transition, "thread_id"), inbox->thread_id) != 0 ||
      turbo_agent_runtime_json_value_object_set_clone(payload, "latest_transition", transition) !=
          0) {
    turbo_runtime_json_destroy(payload);
    turbo_runtime_json_destroy(transition);
    return SALTS_EIO;
  }
  turbo_json_object_set_string(payload, "status", status);
  turbo_runtime_json_destroy(transition);
  *out_status = payload;
  return SALTS_OK;
}

int turbo_agent_inbox_claim(turbo_agent_inbox_t *inbox, turbo_agent_inbox_kind_t kind,
                            const char *run_id, json_value_t **out_record) {
  turbo_agent_inbox_ready_t entry = {0};
  json_value_t *record = NULL;
  int rc;
  if (!out_record) return SALTS_EINVAL;
  *out_record = NULL;
  if (!inbox || !run_id || run_id[0] == '\0' || !turbo_agent_inbox_kind_valid(kind))
    return SALTS_EINVAL;
  salts_mutex_lock(&inbox->mutex);
  if (inbox->closed) {
    salts_mutex_unlock(&inbox->mutex);
    return SALTS_ECANCELED;
  }
  if (inbox->has_active_claim) {
    salts_mutex_unlock(&inbox->mutex);
    return SALTS_EBUSY;
  }
  rc = turbo_agent_inbox_ready_take_kind(inbox, kind, &entry);
  if (rc != SALTS_OK) {
    salts_mutex_unlock(&inbox->mutex);
    return rc;
  }
  inbox->active_claim = entry;
  memset(&entry, 0, sizeof(entry));
  inbox->has_active_claim = 1;
  salts_mutex_unlock(&inbox->mutex);
  rc = turbo_agent_runtime_store_get_json(inbox->runtime, TURBO_AGENT_INBOX_PAYLOADS_COLLECTION,
                                          inbox->active_claim.inbox_id, &record) == 0
           ? SALTS_OK
           : SALTS_EIO;
  if (rc == SALTS_OK)
    rc = turbo_agent_inbox_persist_transition(inbox, inbox->active_claim.inbox_id,
                                              TURBO_AGENT_INBOX_CLAIMED, "claimed",
                                              inbox->active_claim.transition_seq + 1, run_id, NULL);
  salts_mutex_lock(&inbox->mutex);
  if (rc == SALTS_OK) ++inbox->active_claim.transition_seq;
  else {
    entry = inbox->active_claim;
    memset(&inbox->active_claim, 0, sizeof(inbox->active_claim));
    inbox->has_active_claim = 0;
    (void)turbo_agent_inbox_ready_insert(inbox, &entry);
    salts_cond_broadcast(&inbox->changed);
  }
  salts_mutex_unlock(&inbox->mutex);
  if (rc != SALTS_OK) {
    turbo_runtime_json_destroy(record);
    return rc;
  }
  turbo_json_object_set_string(record, "status", "claimed");
  *out_record = record;
  return SALTS_OK;
}

static int turbo_agent_inbox_resolve_entry(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                           turbo_agent_inbox_ready_t *out_entry) {
  turbo_agent_inbox_status_t status = 0;
  int rc;
  memset(out_entry, 0, sizeof(*out_entry));
  salts_mutex_lock(&inbox->mutex);
  if (inbox->has_active_claim && strcmp(inbox->active_claim.inbox_id, inbox_id) == 0) {
    *out_entry = inbox->active_claim;
    out_entry->inbox_id = tstr_clone(inbox->active_claim.inbox_id);
    salts_mutex_unlock(&inbox->mutex);
    return out_entry->inbox_id ? SALTS_OK : SALTS_ENOMEM;
  }
  salts_mutex_unlock(&inbox->mutex);
  rc = turbo_agent_inbox_load_ready(inbox, inbox_id, out_entry, &status);
  if (rc != SALTS_OK) return rc;
  if (status != TURBO_AGENT_INBOX_CLAIMED) {
    turbo_agent_inbox_ready_reset(out_entry);
    return SALTS_EINVAL;
  }
  return SALTS_OK;
}

int turbo_agent_inbox_mark_applied(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                   const char *applied_event_id) {
  turbo_agent_inbox_ready_t entry = {0};
  int rc;
  if (!inbox || !inbox_id || inbox_id[0] == '\0' || !applied_event_id ||
      applied_event_id[0] == '\0')
    return SALTS_EINVAL;
  rc = turbo_agent_inbox_resolve_entry(inbox, inbox_id, &entry);
  if (rc != SALTS_OK) return rc;
  if (entry.transition_seq >= TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER) {
    turbo_agent_inbox_ready_reset(&entry);
    return SALTS_ERANGE;
  }
  rc = turbo_agent_inbox_persist_transition(inbox, inbox_id, TURBO_AGENT_INBOX_APPLIED, "applied",
                                            entry.transition_seq + 1, NULL, applied_event_id);
  if (rc == SALTS_OK) {
    salts_mutex_lock(&inbox->mutex);
    if (inbox->has_active_claim && strcmp(inbox->active_claim.inbox_id, inbox_id) == 0) {
      turbo_agent_inbox_ready_reset(&inbox->active_claim);
      tstr_free(inbox->active_event_id);
      inbox->active_event_id = NULL;
      inbox->has_active_claim = 0;
    }
    if (inbox->used_items > 0 && inbox->used_bytes >= entry.payload_bytes) {
      --inbox->used_items;
      inbox->used_bytes -= entry.payload_bytes;
    }
    salts_cond_broadcast(&inbox->changed);
    salts_mutex_unlock(&inbox->mutex);
  }
  turbo_agent_inbox_ready_reset(&entry);
  return rc;
}

int turbo_agent_inbox_requeue(turbo_agent_inbox_t *inbox, const char *inbox_id) {
  turbo_agent_inbox_ready_t entry = {0};
  int rc;
  if (!inbox || !inbox_id || inbox_id[0] == '\0') return SALTS_EINVAL;
  rc = turbo_agent_inbox_resolve_entry(inbox, inbox_id, &entry);
  if (rc != SALTS_OK) return rc;
  if (entry.transition_seq >= TURBO_AGENT_INBOX_MAX_EXACT_JSON_INTEGER) {
    turbo_agent_inbox_ready_reset(&entry);
    return SALTS_ERANGE;
  }
  rc = turbo_agent_inbox_persist_transition(inbox, inbox_id, TURBO_AGENT_INBOX_QUEUED, "requeued",
                                            entry.transition_seq + 1, NULL, NULL);
  if (rc == SALTS_OK) {
    ++entry.transition_seq;
    salts_mutex_lock(&inbox->mutex);
    if (inbox->has_active_claim && strcmp(inbox->active_claim.inbox_id, inbox_id) == 0) {
      turbo_agent_inbox_ready_reset(&inbox->active_claim);
      tstr_free(inbox->active_event_id);
      inbox->active_event_id = NULL;
      inbox->has_active_claim = 0;
    }
    rc = turbo_agent_inbox_ready_insert(inbox, &entry);
    salts_cond_broadcast(&inbox->changed);
    salts_mutex_unlock(&inbox->mutex);
  }
  turbo_agent_inbox_ready_reset(&entry);
  return rc;
}

int turbo_agent_inbox_bind_applied_event(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                         const char *event_id) {
  tstr_t event_id_copy;
  if (!inbox || !inbox_id || inbox_id[0] == '\0' || !event_id || event_id[0] == '\0') {
    return SALTS_EINVAL;
  }
  event_id_copy = tstr_dup(event_id);
  if (!event_id_copy) return SALTS_ENOMEM;
  salts_mutex_lock(&inbox->mutex);
  if (!inbox->has_active_claim || strcmp(inbox->active_claim.inbox_id, inbox_id) != 0 ||
      inbox->active_event_id) {
    salts_mutex_unlock(&inbox->mutex);
    tstr_free(event_id_copy);
    return SALTS_EINVAL;
  }
  inbox->active_event_id = event_id_copy;
  salts_mutex_unlock(&inbox->mutex);
  return SALTS_OK;
}

int turbo_agent_inbox_commit_bound_claim(turbo_agent_inbox_t *inbox) {
  tstr_t inbox_id = NULL;
  tstr_t event_id = NULL;
  int rc;
  if (!inbox) return SALTS_EINVAL;
  salts_mutex_lock(&inbox->mutex);
  if (inbox->has_active_claim && inbox->active_event_id) {
    inbox_id = tstr_clone(inbox->active_claim.inbox_id);
    event_id = tstr_clone(inbox->active_event_id);
  }
  salts_mutex_unlock(&inbox->mutex);
  if (!inbox_id && !event_id) return SALTS_OK;
  if (!inbox_id || !event_id) {
    tstr_free(inbox_id);
    tstr_free(event_id);
    return SALTS_ENOMEM;
  }
  rc = turbo_agent_inbox_mark_applied(inbox, inbox_id, event_id);
  tstr_free(inbox_id);
  tstr_free(event_id);
  return rc;
}

int turbo_agent_inbox_close(turbo_agent_inbox_t *inbox) {
  if (!inbox) return SALTS_EINVAL;
  salts_mutex_lock(&inbox->mutex);
  if (inbox->closed) {
    salts_mutex_unlock(&inbox->mutex);
    return SALTS_EALREADY;
  }
  inbox->closed = 1;
  salts_cond_broadcast(&inbox->changed);
  salts_mutex_unlock(&inbox->mutex);
  return SALTS_OK;
}
