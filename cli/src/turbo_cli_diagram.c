#include "turbo_cli.h"
#include "turbo_agent_evidence_diagram.h"
#include "turbo_runtime_store_sqlite.h"
#include <cmeta_crypto.h>
#include <stdlib.h>
#include <string.h>

static int journal_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* Bound the input before copying it; all IDs borrow this one local owner. */
static int diagram_journal_selection(const char *text, tstr *storage,
    const char **ids, size_t *count) {
  const size_t key_bytes = 2u * SALTS_SHA256_DIGEST_BYTES;
  const size_t max_bytes = TURBO_AGENT_DIAGRAM_MAX_JOURNALS * (key_bytes + 1) - 1;
  size_t length = 0, field_bytes = 0;
  *count = 0; *storage = NULL;
  if (!text) return SALTS_OK;
  for (; text[length]; ++length) {
    char c = text[length];
    if (length >= max_bytes) return SALTS_EINVAL;
    if (c == ',') {
      if (field_bytes != key_bytes) return SALTS_EINVAL;
      field_bytes = 0;
    } else {
      if (!journal_hex(c) || ++field_bytes > key_bytes) return SALTS_EINVAL;
    }
  }
  if (field_bytes != key_bytes) return SALTS_EINVAL;
  *storage = tstr_new_len(text, length);
  if (!*storage) return SALTS_ENOMEM;
  for (size_t offset = 0; offset < length; offset += key_bytes + 1) {
    char *key = *storage + offset;
    key[key_bytes] = 0;
    for (size_t i = 0; i < *count; ++i)
      if (!strcmp(ids[i], key)) return SALTS_EINVAL;
    ids[(*count)++] = key;
  }
  return SALTS_OK;
}

static int compare_journal_keys(const void *left, const void *right) {
  return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/* CLI-only discovery: the exclusive SQLite owner bounds and stabilizes this
 * list. The SDK's selected-record adapter continues to require only point reads. */
static int diagram_discover_journals(turbo_agent_runtime_t *runtime, const char *thread,
    json_value_t **records, const char **ids, size_t *count) {
  json_value_t *head = NULL;
  const json_value_t *run_value;
  const char *run_id;
  int rc;
  *records = NULL; *count = 0;
  rc = turbo_agent_runtime_get_published_thread_head(runtime, thread, &head);
  if (rc != SALTS_OK) goto done;
  run_value = json_object_get(head, "id"); run_id = json_string(run_value);
  if (json_type(run_value) != JSON_STRING || !run_id || !run_id[0] ||
      json_string_len(run_value) > TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES ||
      json_string_len(run_value) != strlen(run_id)) { rc = SALTS_EPROTO; goto done; }
  rc = turbo_agent_runtime_store_list_json(runtime, "agent_tool_journal", "run_id", run_id, records);
  if (rc != SALTS_OK) goto done;
  *count = json_array_size(*records);
  if (*count > TURBO_AGENT_DIAGRAM_MAX_JOURNALS) { rc = SALTS_ENOSPC; goto done; }
  for (size_t i = 0; i < *count; ++i) {
    const json_value_t *key = json_object_get(json_array_get(*records, i), "journal_id");
    ids[i] = json_string(key);
    if (json_type(key) != JSON_STRING || !ids[i] ||
        json_string_len(key) != 2u * SALTS_SHA256_DIGEST_BYTES) { rc = SALTS_EPROTO; goto done; }
    for (size_t j = 0; j < 2u * SALTS_SHA256_DIGEST_BYTES; ++j)
      if (!journal_hex(ids[i][j])) { rc = SALTS_EPROTO; goto done; }
  }
  qsort(ids, *count, sizeof(*ids), compare_journal_keys);
done:
  json_free(head);
  return rc;
}

static int diagram_node_id_valid(const char *id) {
  if (!id || !id[0] || strlen(id) > TURBO_DIAGRAM_MAX_TEXT_BYTES ||
      !vstr_utf8_valid(vstr_from_cstr(id))) return 0;
  for (size_t i = 0; id[i]; ++i) {
    unsigned char c = (unsigned char)id[i];
    if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127) return 0;
  }
  return 1;
}

static int diagram_selection_options(const turbo_cli_options_t *options,
    turbo_diagram_direction_t *direction, size_t *hops) {
  *direction = TURBO_DIAGRAM_BOTH; *hops = 1;
  if (!options->diagram_focus) return !options->diagram_direction && !options->diagram_depth && !options->diagram_to;
  if (!diagram_node_id_valid(options->diagram_focus) ||
      (options->diagram_to && !diagram_node_id_valid(options->diagram_to))) return 0;
  if (options->diagram_direction) {
    if (!strcmp(options->diagram_direction, "incoming")) *direction = TURBO_DIAGRAM_INCOMING;
    else if (!strcmp(options->diagram_direction, "outgoing")) *direction = TURBO_DIAGRAM_OUTGOING;
    else if (strcmp(options->diagram_direction, "both")) return 0;
  }
  if (options->diagram_depth) {
    const char *text = options->diagram_depth;
    if (!text[0] || strlen(text) > 3) return 0;
    *hops = 0;
    for (size_t i = 0; text[i]; ++i) {
      if (text[i] < '0' || text[i] > '9') return 0;
      *hops = *hops * 10 + (size_t)(text[i] - '0');
      if (*hops >= TURBO_DIAGRAM_MAX_NODES) return 0;
    }
  }
  return 1;
}

int turbo_cli_export_diagram(const turbo_cli_options_t *options, FILE *output, FILE *diagnostics) {
  turbo_runtime_sqlite_config_t config;
  turbo_agent_runtime_store_t store = {0};
  turbo_agent_runtime_t *runtime = NULL;
  json_value_t *metadata = NULL, *view = NULL, *selected = NULL, *journals = NULL;
  tstr mermaid = NULL, journal_storage = NULL;
  const char *journal_ids[TURBO_AGENT_DIAGRAM_MAX_JOURNALS];
  size_t journal_count = 0;
  char *serialized = NULL;
  const char *thread, *phase, *text;
  int rc = 1;
  int discover_journals;
  turbo_diagram_direction_t direction;
  size_t hops;
  if (!options || !output || !diagnostics || !options->diagram ||
      (strcmp(options->diagram, "mermaid") && strcmp(options->diagram, "json")) ||
      !options->session_db || !options->session_db[0] || options->resume || options->prompt || options->model) {
    if (diagnostics) fputs("Diagram export requires --session-db and --diagram mermaid|json, without --resume, --prompt or --model.\n", diagnostics);
    return 2;
  }
  if (options->mode || options->instructions || options->knowledge_db || options->index_dir ||
      options->vector_db || options->embedding_url || options->embedding_model) {
    fputs("Diagram export cannot be combined with agent mode, instructions or knowledge options.\n", diagnostics);
    return 2;
  }
  if ((options->diagram_origins || options->diagram_plan_steps) && !options->diagram_journals) {
    fputs("--diagram-origins and --diagram-plan-steps require --diagram-journals.\n", diagnostics);
    return 2;
  }
  if (!diagram_selection_options(options, &direction, &hops)) {
    fputs("Diagram selection requires --diagram-focus ID, direction incoming|outgoing|both and depth 0..127.\n", diagnostics);
    return 2;
  }
  discover_journals = options->diagram_journals && !strcmp(options->diagram_journals, "all");
  if (!discover_journals) {
    int status = diagram_journal_selection(options->diagram_journals,
        &journal_storage, journal_ids, &journal_count);
    if (status == SALTS_EINVAL) {
      fputs("--diagram-journals requires all or 1..32 unique comma-separated lowercase SHA-256 keys, without spaces.\n", diagnostics);
      tstr_free(journal_storage);
      return 2;
    }
    if (status != SALTS_OK) goto done;
  }
  turbo_runtime_sqlite_config_init(&config);
  config.path = options->session_db; config.create_if_missing = 0;
  if (turbo_runtime_store_sqlite_open(&config, &store) != SALTS_OK) goto done;
  runtime = turbo_agent_runtime_create(&store);
  if (!runtime) goto done;
  store = (turbo_agent_runtime_store_t){0};
  if (turbo_agent_runtime_store_get_json_status(runtime, "cli_sessions", "current", &metadata) != 0) goto done;
  thread = json_get_string(metadata, "thread_id"); phase = json_get_string(metadata, "phase");
  if (turbo_runtime_json_value_as_int64(json_object_get(metadata, "format_version"), -1) != 1 ||
      !thread || !thread[0] || strlen(thread) != json_string_len(json_object_get(metadata, "thread_id")) ||
      !phase || (strcmp(phase, "ready") && strcmp(phase, "active")) ||
      strlen(phase) != json_string_len(json_object_get(metadata, "phase"))) goto done;
  if (discover_journals && diagram_discover_journals(runtime, thread,
      &journals, journal_ids, &journal_count) != SALTS_OK) {
    fputs("Cannot discover journals for the published run: check record identities and store limits; at most 32 matches are supported.\n", diagnostics);
    goto done;
  }
  if ((journal_count
        ? turbo_agent_thread_tool_diagram_ex(runtime, thread, journal_ids, journal_count,
            options->diagram_plan_steps ? TURBO_AGENT_DIAGRAM_CALL_ORIGINS | TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS :
              (options->diagram_origins ? TURBO_AGENT_DIAGRAM_CALL_ORIGINS : 0), &view)
        : turbo_agent_runtime_thread_diagram(runtime, thread, &view)) != 0 ||
      turbo_runtime_json_object_set_string(view, "session_phase", phase) != 0) goto done;
  json_free(journals); journals = NULL; /* The view now owns every copied key. */
  if (discover_journals &&
      (turbo_runtime_json_object_set_string(view, "journal_scope", "published_run_records") != 0 ||
       turbo_runtime_json_object_set_int64(view, "journal_count", (int64_t)journal_count) != 0)) goto done;
  if (options->diagram_focus) {
    int status = options->diagram_to
        ? turbo_diagram_shortest_path(view, options->diagram_focus, options->diagram_to, direction, hops, &selected)
        : turbo_diagram_neighborhood(view, options->diagram_focus, direction, hops, &selected);
    if (status == SALTS_ENOENT) {
      fputs(options->diagram_to
          ? "Diagram endpoint missing or no path within the direction/depth limit; use exact nodes[].id values from --diagram json.\n"
          : "Diagram focus was not found; use an exact nodes[].id from --diagram json.\n", diagnostics);
      goto done;
    }
    if (status == SALTS_EINVAL) { rc = 2; goto done; }
    if (status != SALTS_OK) goto done;
    json_free(view); view = selected; selected = NULL;
  }
  if (!strcmp(options->diagram, "mermaid")) {
    if (turbo_diagram_mermaid(view, &mermaid) != 0) goto done;
    const char *prelude = journal_count
        ? (!strcmp(phase, "active")
            ? "%% Session active: published lineage plus selected current journals; incomplete execution evidence.\n"
            : "%% Session ready: published lineage plus selected current journals; journals are not frozen in the snapshot.\n")
        : (!strcmp(phase, "active")
            ? "%% Session active: unpublished work is not shown.\n"
            : "%% Session ready: published records only.\n");
    if (fputs(prelude, output) == EOF) goto done;
    if (discover_journals && fputs("%% Journal discovery: published run only; key order is not execution order.\n", output) == EOF) goto done;
    text = mermaid;
  } else {
    serialized = json_serialize(view, NULL);
    if (!serialized) goto done;
    text = serialized;
  }
  if (fputs(text, output) == EOF || fflush(output) != 0) goto done;
  rc = 0;
done:
  if (rc) fputs("Cannot export diagram: check the saved session, published head, record links and diagram limits.\n", diagnostics);
  json_serialize_free(serialized); tstr_free(mermaid); json_free(metadata); json_free(view); json_free(selected);
  tstr_free(journal_storage);
  json_free(journals);
  turbo_agent_runtime_destroy(runtime);
  if (store.user_data_free) store.user_data_free(store.user_data);
  return rc;
}
