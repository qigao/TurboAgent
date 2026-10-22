#include "turbo_agent_session.h"
#include "turbo_agent_session_execution.h"
#include "turbo_agent_inbox.h"
#include "turbo_agent_state.h"
#include "turbo_runtime_store_sqlite.h"
#include <fmt.h>
#include <tstr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIXTURE_FAILURE = 2, FIXTURE_CRASH = 86 };
#define THREAD_ID "inbox-crash-thread"
#define PAYLOADS "agent_inbox_payloads"
#define TRANSITIONS "agent_inbox_transitions"
#define INPUT_TEXT "durable inbox work"
typedef struct cut_s {
  const char *name, *raw_status;
  int models, owner_event, head_event;
} cut_t;
static const cut_t cuts[] = {
  {"after_claim", "claimed", 0, 0, 0},
  {"in_model", "claimed", 1, 0, 0},
  {"before_applied", "claimed", 1, 1, 1},
  {"after_applied", "applied", 1, 1, 1},
  {"before_delivery", "applied", 1, 1, 1},
  {"after_following_run", "claimed", 1, 1, 0}
};
typedef struct fixture_s {
  const cut_t *cut;
  turbo_agent_runtime_store_t inner;
  turbo_agent_runtime_t *runtime, *oracle;
  turbo_agent_session_t *session;
  int inject, block_applied, blocked, model_calls, read_fault, owner_reads, run_lists;
} fixture_t;
static int failure(const char *what, int line) {
  fprintf(stderr, "Inbox process fixture line %d: %s\n", line, what);
  return FIXTURE_FAILURE;
}
#define REQUIRE(expression) do { if (!(expression)) return failure(#expression, __LINE__); } while (0)
static int text_is(const json_value_t *value, const char *key, const char *expected) {
  const json_value_t *field = json_object_get(value, key);
  return field && json_type(field) == JSON_STRING && expected &&
      json_string_len(field) == strlen(expected) && !memcmp(json_string(field), expected, strlen(expected));
}
static int count_put(fixture_t *f, const char *id, int n) {
  json_value_t *record = json_create_object();
  int rc = SALTS_ENOMEM;
  if (record && !turbo_runtime_json_object_set_int64(record, "count", n))
    rc = turbo_agent_runtime_store_put_json(f->oracle, "observations", id, record);
  json_free(record);
  return rc;
}
static int count_get(fixture_t *f, const char *id, int *n) {
  json_value_t *record = NULL;
  int rc = turbo_agent_runtime_store_get_json_status(f->oracle, "observations", id, &record);
  int64_t value = turbo_runtime_json_value_as_int64(json_object_get(record, "count"), -1);
  if (!rc && (value < 0 || value > 100)) rc = SALTS_EPROTO;
  if (!rc) *n = (int)value;
  json_free(record);
  return rc;
}
static int store_put(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  json_value_t *record = NULL;
  int claimed = 0, applied = 0, rc;
  if (!strcmp(collection, TRANSITIONS)) {
    record = json_parse(text, strlen(text));
    if (!record) return SALTS_EPROTO;
    claimed = text_is(record, "status", "claimed");
    applied = text_is(record, "status", "applied");
  }
  if (f->inject && applied && !strcmp(f->cut->name, "before_applied")) _Exit(FIXTURE_CRASH);
  if (applied && f->block_applied) { ++f->blocked; json_free(record); return SALTS_EIO; }
  rc = f->inner.put(f->inner.user_data, collection, id, text);
  if (!rc && f->inject && ((claimed && !strcmp(f->cut->name, "after_claim")) ||
      (applied && !strcmp(f->cut->name, "after_applied")))) _Exit(FIXTURE_CRASH);
  json_free(record);
  return rc;
}
static int store_get(void *user, const char *collection, const char *id, char **out) {
  fixture_t *f = user;
  if (f->read_fault && !strcmp(collection, "runs")) {
    ++f->owner_reads;
    if (f->read_fault == 1) return SALTS_EIO;
    if (f->read_fault == 2 || f->read_fault == 3) {
      json_value_t *record = NULL;
      char *serialized;
      int rc = f->inner.get(f->inner.user_data, collection, id, out);
      if (rc != SALTS_OK && rc != SALTS_ENOENT) return rc;
      if (rc == SALTS_ENOENT) {
        record = json_create_object();
        if (!record || turbo_runtime_json_object_set_string(record, "id", id) ||
            turbo_runtime_json_object_set_string(record, "thread_id", THREAD_ID)) {
          json_free(record); return SALTS_ENOMEM;
        }
      } else record = json_parse(*out, strlen(*out));
      if (!record || (f->read_fault == 2 &&
          turbo_runtime_json_object_set_string(record, "thread_id", "foreign-thread")) ||
          (f->read_fault == 3 && turbo_runtime_json_object_set_string(record, "state_snapshot", "invalid JSON"))) {
        json_free(record); return SALTS_EPROTO;
      }
      serialized = json_serialize(record, NULL); json_free(record);
      if (!serialized) return SALTS_ENOMEM;
      free(*out); *out = malloc(strlen(serialized) + 1);
      if (*out) memcpy(*out, serialized, strlen(serialized) + 1);
      json_serialize_free(serialized);
      return *out ? SALTS_OK : SALTS_ENOMEM;
    }
  }
  return f->inner.get(f->inner.user_data, collection, id, out);
}
static int store_list(void *user, const char *collection, const char *key, const char *value, char **out) {
  fixture_t *f = user;
  if (!strcmp(collection, "runs")) ++f->run_lists;
  if (f->read_fault == 4 && !strcmp(collection, TRANSITIONS)) return SALTS_EIO;
  return f->inner.list(f->inner.user_data, collection, key, value, out);
}
static int store_create(void *user, const char *collection, const char *id, const char *text) {
  fixture_t *f = user;
  return f->inner.create_if_absent(f->inner.user_data, collection, id, text);
}
static void store_free(void *user) {
  fixture_t *f = user;
  if (f->inner.user_data_free) f->inner.user_data_free(f->inner.user_data);
  f->inner = (turbo_agent_runtime_store_t){0};
}
static int model_transport(const char *request, char **response, void *user) {
  static const char reply[] = "{\"id\":\"inbox_model\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"received\"}]}]}";
  fixture_t *f = user;
  int n;
  *response = NULL; ++f->model_calls;
  if (!request || !strstr(request, INPUT_TEXT) || count_get(f, "model", &n) || n >= 100 ||
      count_put(f, "model", n + 1)) return -1;
  if (f->inject && !strcmp(f->cut->name, "in_model")) _Exit(FIXTURE_CRASH);
  *response = malloc(sizeof(reply));
  if (!*response) return -1;
  memcpy(*response, reply, sizeof(reply));
  return 0;
}
static int fixture_open(fixture_t *f, const char *directory, int create) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t oracle = {0}, callbacks = {0};
  turbo_agent_session_config_t session = {0};
  tstr path = tstr_format("{}/session.db", directory);
  tstr external = tstr_format("{}/external.db", directory);
  int rc = SALTS_ENOMEM;
  if (!path || !external) goto done;
  turbo_runtime_sqlite_config_init(&config);
  config.path = external; config.create_if_missing = create;
  config.max_record_bytes = 65536; config.max_list_bytes = 1048576; config.max_list_records = 128;
  rc = turbo_runtime_store_sqlite_open(&config, &oracle);
  if (rc) goto done;
  f->oracle = turbo_agent_runtime_create(&oracle);
  if (!f->oracle) { oracle.user_data_free(oracle.user_data); rc = SALTS_ENOMEM; goto done; }
  config.path = path;
  rc = turbo_runtime_store_sqlite_open(&config, &f->inner);
  if (rc) goto done;
  callbacks.put = store_put; callbacks.get = store_get; callbacks.list = store_list;
  callbacks.create_if_absent = store_create; callbacks.user_data = f; callbacks.user_data_free = store_free;
  session.thread_id = THREAD_ID; session.runtime_store = callbacks;
  session.agent_config.model = "offline-inbox";
  session.agent_config.transport_fn = model_transport; session.agent_config.transport_user_data = f;
  f->session = turbo_agent_session_create(&session);
  if (!f->session) { rc = SALTS_ENOMEM; goto done; }
  f->runtime = turbo_agent_session_runtime(f->session);
  rc = f->runtime ? SALTS_OK : SALTS_EIO;
done:
  tstr_free(path); tstr_free(external);
  return rc;
}
static void fixture_close(fixture_t *f) {
  turbo_agent_session_destroy(f->session);
  store_free(f);
  turbo_agent_runtime_destroy(f->oracle);
}
static int configure(fixture_t *f) {
  turbo_agent_inbox_config_t config = {sizeof(config), TURBO_AGENT_INBOX_ABI_VERSION, 1, 4096, 1024, 1};
  return turbo_agent_session_inbox_configure(f->session, &config);
}
static json_value_t *message(void) {
  json_value_t *value = json_create_object();
  if (!value || turbo_runtime_json_object_set_string(value, "role", "user") ||
      turbo_runtime_json_object_set_string(value, "content", INPUT_TEXT)) {
    json_free(value); return NULL;
  }
  return value;
}
static const char *find_event(const json_value_t *state, const char *id) {
  const json_value_t *events = json_object_get(state, "events");
  for (size_t i = 0; i < json_array_size(events); ++i) {
    const json_value_t *event = json_array_get(events, i);
    if (text_is(event, "kind", "inbox_message") && text_is(event, "inbox_id", id))
      return json_get_string(event, "event_id");
  }
  return NULL;
}
static int evidence(fixture_t *f, const char *expected_status) {
  json_value_t *payloads = NULL, *transitions = NULL, *owner = NULL, *head = NULL, *record = NULL;
  const json_value_t *payload, *latest = NULL;
  const char *id, *run = NULL, *event;
  int n, valid;
  double seq = 0;
  REQUIRE(!count_get(f, "model", &n) && n == f->cut->models);
  REQUIRE(!count_get(f, "delivery", &n) && n == 0);
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, PAYLOADS, NULL, NULL, &payloads));
  REQUIRE(json_array_size(payloads) == 1);
  payload = json_array_get(payloads, 0); id = json_get_string(payload, "inbox_id");
  REQUIRE(id && text_is(payload, "thread_id", THREAD_ID) &&
      text_is(json_object_get(payload, "payload"), "content", INPUT_TEXT));
  REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, TRANSITIONS, "inbox_id", id, &transitions));
  for (size_t i = 0; i < json_array_size(transitions); ++i) {
    const json_value_t *transition = json_array_get(transitions, i);
    double current = json_get_double(transition, "transition_seq", -1);
    if (current == 2 && text_is(transition, "status", "claimed")) run = json_get_string(transition, "claimed_by_run_id");
    if (current > seq) { seq = current; latest = transition; }
  }
  REQUIRE(run && latest && text_is(latest, "status", expected_status));
  n = turbo_agent_runtime_store_get_json_status(f->runtime, "runs", run, &record);
  if (f->cut->owner_event) {
    REQUIRE(n == SALTS_OK && text_is(record, "id", run) && text_is(record, "thread_id", THREAD_ID));
    REQUIRE(!turbo_agent_runtime_get_run_state_json_value(f->runtime, run, &owner));
  } else REQUIRE(n == SALTS_ENOENT && !record);
  json_free(record); record = NULL;
  n = turbo_agent_runtime_get_published_thread_head(f->runtime, THREAD_ID, &record);
  if (f->cut->owner_event) {
    REQUIRE(n == SALTS_OK && record);
    REQUIRE(!turbo_agent_runtime_get_thread_head_state_json_value(f->runtime, THREAD_ID, &head));
  } else REQUIRE(n == SALTS_ENOENT && !record);
  json_free(record);
  event = find_event(owner, id);
  valid = !!event == f->cut->owner_event && !!find_event(head, id) == f->cut->head_event;
  if (text_is(latest, "status", "applied")) valid = valid && text_is(latest, "applied_event_id", event);
  if (!valid) fprintf(stderr, "%s: owner_event=%d head_event=%d status=%s\n", f->cut->name,
      !!event, !!find_event(head, id), json_get_string(latest, "status"));
  json_free(payloads); json_free(transitions); json_free(owner); json_free(head);
  REQUIRE(valid);
  return 0;
}
static int later_node(turbo_graph_exec_ctx_t *context, void *user) {
  (void)context; (void)user;
  return 0;
}
static int exercise(fixture_t *f, const char *mode) {
  json_value_t *input = NULL, *summary = NULL, *state = NULL, *payloads = NULL, *status = NULL;
  char *id = NULL;
  int rc = FIXTURE_FAILURE, operation;
  if (!strcmp(mode, "init")) return count_put(f, "model", 0) || count_put(f, "delivery", 0);
  if (!strcmp(mode, "inspect")) return evidence(f, f->cut->raw_status);
  if (!strcmp(mode, "owner_error") || !strcmp(mode, "owner_foreign") ||
      !strcmp(mode, "owner_state_error") || !strcmp(mode, "transition_error")) {
    f->read_fault = !strcmp(mode, "owner_error") ? 1 : !strcmp(mode, "owner_foreign") ? 2 :
        !strcmp(mode, "owner_state_error") ? 3 : 4;
    operation = configure(f);
    REQUIRE(operation == (f->read_fault == 2 ? SALTS_EPROTO : SALTS_EIO));
    REQUIRE(!f->run_lists && (f->read_fault == 4 || f->owner_reads > 0));
    f->read_fault = 0;
    REQUIRE(!f->model_calls);
    return evidence(f, f->cut->raw_status);
  }
  {
    json_value_t *before = NULL, *after = NULL;
    REQUIRE(!turbo_agent_runtime_list_runs(f->runtime, THREAD_ID, &before));
    f->run_lists = 0;
    operation = configure(f);
    REQUIRE(!operation && !f->run_lists);
    REQUIRE(!turbo_agent_runtime_list_runs(f->runtime, THREAD_ID, &after));
    REQUIRE(json_array_size(before) == json_array_size(after));
    json_free(before); json_free(after);
  }
  if (!strcmp(mode, "crash")) {
    input = message(); REQUIRE(input);
    REQUIRE(!turbo_agent_session_enqueue(f->session, TURBO_AGENT_INBOX_STEER, input, 0, &id));
    f->inject = 1; f->block_applied = !strcmp(f->cut->name, "after_following_run");
    operation = turbo_agent_session_start_text(f->session, "initial", NULL, &summary, &state);
    if (f->block_applied) {
      turbo_graph_t *graph = turbo_graph_create("following-inbox-run");
      json_value_t *next = turbo_agent_state_create();
      REQUIRE(operation == SALTS_EIO && f->blocked == 1 && graph && next);
      REQUIRE(turbo_graph_add_json_value_node_ex(graph, "later", "test.inbox.later.v1", later_node, NULL) == TURBO_GRAPH_EXEC_OK);
      REQUIRE(turbo_graph_set_entry(graph, "later") == TURBO_GRAPH_EXEC_OK);
      json_free(summary); summary = NULL; json_free(state); state = NULL;
      operation = turbo_agent_session_start_graph(f->session, graph, next, NULL, NULL, NULL, &summary, &state);
      REQUIRE(operation == SALTS_EIO && f->blocked == 2 && !evidence(f, "claimed"));
      _Exit(FIXTURE_CRASH);
    }
    REQUIRE(!operation && summary && state && f->model_calls == 1);
    if (!strcmp(f->cut->name, "before_delivery")) _Exit(FIXTURE_CRASH);
  } else if (!strcmp(mode, "recover") || !strcmp(mode, "sticky")) {
    const char *expected = f->cut->owner_event ? "applied" : "queued";
    REQUIRE(!evidence(f, expected));
    REQUIRE(!turbo_agent_runtime_store_list_json(f->runtime, PAYLOADS, NULL, NULL, &payloads));
    id = NULL;
    {
      const char *borrowed = json_get_string(json_array_get(payloads, 0), "inbox_id");
      REQUIRE(borrowed);
      id = malloc(strlen(borrowed) + 1); REQUIRE(id); memcpy(id, borrowed, strlen(borrowed) + 1);
    }
    REQUIRE(!turbo_agent_session_inbox_status(f->session, id, &status) && text_is(status, "status", expected));
    if (!strcmp(mode, "recover")) {
      json_value_t *claim = NULL;
      operation = turbo_agent_session_inbox_claim(f->session, TURBO_AGENT_INBOX_STEER, "recovery-probe", &claim);
      if (f->cut->owner_event) REQUIRE(operation == SALTS_ENOENT && !claim);
      else {
        REQUIRE(!operation && text_is(claim, "inbox_id", id));
        REQUIRE(!turbo_agent_session_inbox_requeue(f->session, id));
      }
      json_free(claim);
    }
    REQUIRE(!f->model_calls && !evidence(f, expected));
    rc = 0;
  }
  free(id); json_free(input); json_free(summary); json_free(state); json_free(payloads); json_free(status);
  return rc;
}
int main(int argc, char **argv) {
  fixture_t f = {0};
  int rc;
  if (argc != 4) return failure("mode/cut/private-directory required", __LINE__);
  for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i)
    if (!strcmp(cuts[i].name, argv[2])) f.cut = &cuts[i];
  if (!f.cut) return failure("unknown cut", __LINE__);
  rc = fixture_open(&f, argv[3], !strcmp(argv[1], "init"));
  if (!rc) rc = exercise(&f, argv[1]);
  fixture_close(&f);
  return rc ? FIXTURE_FAILURE : 0;
}
