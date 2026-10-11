#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_runtime_store_sqlite.h"
#include <fmt.h>
#include <cmeta_process.h>
#include <cmeta_fs.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct storage_model_s { int calls, ordered, fail; } storage_model_t;

static const char journal_key[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char other_journal_key[] = "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static int put_diagram_journal(turbo_agent_runtime_store_t *store, const json_value_t *metadata,
    const char *id, const char *mismatch) {
  static const char fixture[] = "{\"schema_version\":1,\"call_id\":\"call\",\"turn_key\":\"turn\","
      "\"tool_name\":\"fs.read\",\"phase\":\"started\",\"status\":0,\"execution_mode\":0,\"idempotency\":0,"
      "\"updated_at\":\"2026-10-11T00:00:00Z\",\"output\":\"private-output\",\"detail\":\"private-detail\"}";
  json_value_t *record = json_parse(fixture, sizeof(fixture) - 1);
  char *text = NULL;
  int rc = SALTS_ENOMEM;
  if (!record || turbo_runtime_json_object_set_string(record, "journal_id", id) ||
      turbo_runtime_json_object_set_string(record, "arguments_hash", journal_key) ||
      turbo_runtime_json_object_set_string(record, "thread_id", json_get_string(metadata, "thread_id")) ||
      turbo_runtime_json_object_set_string(record, "run_id", json_get_string(metadata, "last_run_id")) ||
      (mismatch && turbo_runtime_json_object_set_string(record, mismatch, "wrong"))) goto done;
  text = json_serialize(record, NULL);
  if (text) rc = store->put(store->user_data, "agent_tool_journal", id, text);
done:
  json_serialize_free(text); json_free(record); return rc;
}

static int storage_model(const char *request, char **out, void *data) {
  storage_model_t *model = data;
  const char response[] = "{\"id\":\"storage_response\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"saved_answer\"}]}]}";
  const char *first = strstr(request, "initial_task"), *answer = strstr(request, "saved_answer"),
             *next = strstr(request, "followup_task");
  ++model->calls;
  model->ordered = first && answer && next && first < answer && answer < next;
  *out = NULL;
  if (model->fail) return -1;
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

spec("CLI persistent conversation admission") {
  static char *directory, *text;
  static tstr path, focus, destination;
  static FILE *input, *output, *diagnostics;
  static turbo_cli_options_t options;
  static turbo_agent_config_t model_config;
  static storage_model_t model;
  static turbo_runtime_sqlite_config_t store_config;
  static turbo_agent_runtime_store_t store;
  static json_value_t *metadata, *run_record;
  static cmeta_process_t *process;

  before_each() {
    directory = tt_make_temp_dir("turbo-cli-storage");
    check_not_null(directory);
    path = tstr_format("{}/session.db", directory);
    check_not_null(path);
    input = tmpfile(); output = tmpfile(); diagnostics = tmpfile();
    check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    options = (turbo_cli_options_t){directory, "initial_task", NULL, 1, path, 0};
    model = (storage_model_t){0};
    model_config = (turbo_agent_config_t){0};
    model_config.model = "test-model";
    model_config.transport_fn = storage_model;
    model_config.transport_user_data = &model;
    turbo_runtime_sqlite_config_init(&store_config);
    store_config.path = path;
  }
  after_each() {
    cmeta_process_destroy(process); process = NULL;
    if (store.user_data_free) store.user_data_free(store.user_data);
    store = (turbo_agent_runtime_store_t){0};
    free(text); text = NULL;
    json_free(metadata); metadata = NULL;
    json_free(run_record); run_record = NULL;
    if (input) fclose(input);
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    tstr_free(path); path = NULL;
    tstr_free(focus); focus = NULL;
    tstr_free(destination); destination = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  for (int as_json = 0; as_json < 2; ++as_json) {
    it("exports a saved session as %s without invoking a model or changing metadata", as_json ? "JSON" : "Mermaid") {
      char buffer[8192];
      size_t size;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(model.calls, 1);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL;
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      fclose(output); output = tmpfile(); check_not_null(output);
      options.prompt = NULL; options.diagram = as_json ? "json" : "mermaid";
      options.workspace = "missing-workspace-is-not-needed-for-export";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(model.calls, 1);
      rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      check_greater(size, 0); check_less(size, sizeof(buffer) - 1);
      if (as_json) {
        run_record = json_parse(buffer, size); check_not_null(run_record);
        check_equal(json_get_string(run_record, "kind"), "execution");
        check_equal(json_get_string(run_record, "session_phase"), "ready");
      } else {
        check_contains(buffer, "flowchart TD"); check_contains(buffer, "published snapshot");
      }
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      if (run_record) { json_free(run_record); run_record = NULL; }
      run_record = json_parse(text, strlen(text)); check_not_null(run_record);
      check_equal(json_get_string(run_record, "last_run_id"), json_get_string(metadata, "last_run_id"));
      check_equal(json_get_int(run_record, "turn_count", -1), 1);
      check_equal(json_get_string(run_record, "phase"), "ready");
    }
  }

  it("rejects invalid diagram options and missing saved sessions without invoking a model") {
    options.diagram = "mermaid";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.prompt = NULL;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    options.diagram = "unsupported";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    check_equal(model.calls, 0);
  }

  for (int as_json = 0; as_json < 2; ++as_json) {
    it("exports selected current journals as %s and preserves active session records", as_json ? "JSON" : "Mermaid") {
      char buffer[16384];
      size_t size;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL;
      check_not_null(metadata);
      check_equal(turbo_runtime_json_object_set_string(metadata, "phase", "active"), 0);
      text = json_serialize(metadata, NULL); check_not_null(text);
      check_equal(store.put(store.user_data, "cli_sessions", "current", text), SALTS_OK);
      check_equal(put_diagram_journal(&store, metadata, journal_key, NULL), SALTS_OK);
      check_equal(put_diagram_journal(&store, metadata, other_journal_key, NULL), SALTS_OK);
      free(text); text = NULL;
      check_equal(store.get(store.user_data, "agent_tool_journal", journal_key, &text), SALTS_OK);
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      fclose(output); output = tmpfile(); check_not_null(output);
      focus = tstr_format("{},{}", other_journal_key, journal_key); check_not_null(focus);
      options.prompt = NULL; options.diagram = as_json ? "json" : "mermaid";
      options.diagram_journals = focus;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(model.calls, 1);
      rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      check_greater(size, 0); check_less(size, sizeof(buffer) - 1);
      check_null(strstr(buffer, "private-output")); check_null(strstr(buffer, "private-detail"));
      if (as_json) {
        const json_value_t *nodes;
        run_record = json_parse(buffer, size); check_not_null(run_record);
        check_equal(json_get_string(run_record, "record_scope"), "published_and_current");
        check_equal(json_get_string(run_record, "session_phase"), "active");
        nodes = json_object_get(run_record, "nodes"); check_equal(json_array_size(nodes), 3u);
        check_equal(json_get_string(json_array_get(nodes, 1), "record_id"), other_journal_key);
        check_equal(json_get_string(json_array_get(nodes, 2), "record_id"), journal_key);
      } else {
        check_contains(buffer, "Session active: published lineage plus selected current journals");
        check_contains(buffer, "side effects unknown");
        check_contains(buffer, "not a step trace");
      }
      json_free(run_record); run_record = NULL;
      fclose(output); output = tmpfile(); check_not_null(output);
      options.diagram_journals = "all";
      options.diagram_origins = 1;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      if (as_json) {
        const json_value_t *nodes;
        run_record = json_parse(buffer, size); check_not_null(run_record);
        check_equal(json_get_string(run_record, "journal_scope"), "published_run_records");
        check_equal(json_get_int(run_record, "journal_count", -1), 2);
        check_equal(json_get_string(run_record, "session_phase"), "active");
        nodes = json_object_get(run_record, "nodes"); check_equal(json_array_size(nodes), 3u);
        check_equal(json_get_string(json_array_get(nodes, 1), "record_id"), journal_key);
        check_equal(json_get_string(json_array_get(nodes, 2), "record_id"), other_journal_key);
        check_equal(json_get_string(json_array_get(nodes, 1), "origin_status"), "unresolved_identity");
      } else {
        check_contains(buffer, "Session active: published lineage plus selected current journals");
        check_contains(buffer, "side effects unknown");
      }
      json_free(run_record); run_record = NULL;
      fclose(output); output = tmpfile(); check_not_null(output);
      tstr_free(focus); focus = tstr_format("journal:{}", journal_key); check_not_null(focus);
      options.diagram_focus = focus; options.diagram_direction = "incoming";
      options.diagram = "json";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      run_record = json_parse(buffer, size); check_not_null(run_record);
      check_equal(json_array_size(json_object_get(run_record, "nodes")), 2u);
      check_equal(json_get_string(json_object_get(run_record, "selection"), "focus_id"), focus);
      check_equal(json_get_string(run_record, "session_phase"), "active");
      check_equal(model.calls, 1);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      {
        char *saved = NULL;
        check_equal(store.get(store.user_data, "agent_tool_journal", journal_key, &saved), SALTS_OK);
        check_equal(saved, text); free(saved);
        check_equal(store.get(store.user_data, "cli_sessions", "current", &saved), SALTS_OK);
        json_free(run_record); run_record = json_parse(saved, strlen(saved)); free(saved);
        check_equal(json_get_string(run_record, "phase"), "active");
        check_equal(json_get_string(run_record, "last_run_id"), json_get_string(metadata, "last_run_id"));
        check_equal(json_get_int(run_record, "turn_count", -1), 1);
      }
    }
  }

  it("discovers only the published run despite a stale CLI completion marker") {
    char buffer[16384];
    size_t size;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(metadata);
    focus = tstr_format("run:{}", json_get_string(metadata, "last_run_id")); check_not_null(focus);
    check_equal(put_diagram_journal(&store, metadata, journal_key, NULL), SALTS_OK);
    check_equal(put_diagram_journal(&store, metadata, other_journal_key, "run_id"), SALTS_OK);
    check_equal(turbo_runtime_json_object_set_string(metadata, "last_run_id", "wrong"), 0);
    check_equal(turbo_runtime_json_object_set_string(metadata, "phase", "active"), 0);
    text = json_serialize(metadata, NULL); check_not_null(text);
    check_equal(store.put(store.user_data, "cli_sessions", "current", text), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    fclose(output); output = tmpfile(); check_not_null(output);
    options.prompt = NULL; options.diagram = "json"; options.diagram_journals = "all";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
    run_record = json_parse(buffer, size); check_not_null(run_record);
    check_equal(json_get_int(run_record, "journal_count", -1), 1);
    check_equal(json_get_string(json_array_get(json_object_get(run_record, "nodes"), 0), "id"), focus);
    check_equal(json_get_string(json_array_get(json_object_get(run_record, "nodes"), 1), "record_id"), journal_key);
    check_null(strstr(buffer, other_journal_key)); check_equal(model.calls, 1);
  }

  it("exports empty journal discovery without claiming tools ran") {
    char buffer[8192];
    size_t size;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    fclose(output); output = tmpfile(); check_not_null(output);
    options.prompt = NULL; options.diagram = "json"; options.diagram_journals = "all";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
    run_record = json_parse(buffer, size); check_not_null(run_record);
    check_equal(json_get_int(run_record, "journal_count", -1), 0);
    check_equal(json_get_string(run_record, "journal_scope"), "published_run_records");
    check_equal(json_array_size(json_object_get(run_record, "nodes")), 1u);
    check_equal(json_array_size(json_object_get(run_record, "edges")), 0u);
    check_equal(model.calls, 1);
  }

  it("accepts 32 discovered journals and rejects the 33rd before focus or output") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text)); check_not_null(metadata);
    for (size_t i = 0; i < 33; ++i) {
      const char hex[] = "0123456789abcdef";
      tstr_free(focus); focus = tstr_format("{}{}{}", hex[i / 16], hex[i % 16], journal_key + 2);
      check_not_null(focus);
      check_equal(put_diagram_journal(&store, metadata, focus, NULL), SALTS_OK);
      if (i == 31) {
        store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
        fclose(output); output = tmpfile(); check_not_null(output);
        options.prompt = NULL; options.diagram = "json"; options.diagram_journals = "all";
        check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
        check_greater(ftell(output), 0);
        check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      }
    }
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    fclose(output); output = tmpfile(); check_not_null(output);
    tstr_free(focus); focus = tstr_format("run:{}", json_get_string(metadata, "last_run_id")); check_not_null(focus);
    options.diagram_focus = focus; options.diagram_depth = "0";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(ftell(output), 0); check_equal(model.calls, 1);
  }

  const char *discovery_errors[] = {"malformed key", "duplicate key", "missing key", "cross-thread record"};
  for (size_t i = 0; i < sizeof(discovery_errors) / sizeof(discovery_errors[0]); ++i) {
    it("refuses discovered %s without exporting a partial view", discovery_errors[i]) {
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(metadata);
      check_equal(put_diagram_journal(&store, metadata, journal_key,
          i == 0 ? "journal_id" : i == 3 ? "thread_id" : NULL), SALTS_OK);
      if (i == 1) {
        check_equal(store.get(store.user_data, "agent_tool_journal", journal_key, &text), SALTS_OK);
        check_equal(store.put(store.user_data, "agent_tool_journal", other_journal_key, text), SALTS_OK);
      } else if (i == 2) {
        check_equal(store.get(store.user_data, "agent_tool_journal", journal_key, &text), SALTS_OK);
        run_record = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(run_record);
        check_equal(turbo_runtime_json_object_set_string(run_record, "journal_id", other_journal_key), 0);
        text = json_serialize(run_record, NULL); check_not_null(text);
        check_equal(store.put(store.user_data, "agent_tool_journal", journal_key, text), SALTS_OK);
      }
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      fclose(output); output = tmpfile(); check_not_null(output);
      focus = tstr_format("run:{}", json_get_string(metadata, "last_run_id")); check_not_null(focus);
      options.prompt = NULL; options.diagram = "json"; options.diagram_journals = "all";
      options.diagram_focus = focus; options.diagram_depth = "0";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0); check_equal(model.calls, 1);
    }
  }

  it("rejects malformed journal selections before database creation or model invocation") {
    cmeta_fs_stat_t file_stat;
    const char *invalid[] = {"", "abc", ",", " xyz", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeF"};
    options.prompt = NULL; options.diagram_journals = journal_key;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram = "json";
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      options.diagram_journals = invalid[i];
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    }
    const char *formats[] = {"{},{}", "{},", ",{}", "{} {}", "{},,{}"};
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
      tstr_free(focus); focus = tstr_format(formats[i], journal_key, journal_key); check_not_null(focus);
      options.diagram_journals = focus;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    }
    tstr_free(focus); focus = tstr_new(); check_not_null(focus);
    for (size_t i = 0; i < 33; ++i) {
      const char hex[] = "0123456789abcdef";
      focus = tstr_append_format(focus, "{}{}{}{}", i ? "," : "", hex[i / 16], hex[i % 16], journal_key + 2);
      check_not_null(focus);
      if (i >= 31) {
        options.diagram_journals = focus;
        /* 32 valid keys reach the missing-database error; 33 fail admission. */
        check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), i == 31 ? 1 : 2);
      }
    }
    check_equal(model.calls, 0); check_equal(ftell(output), 0);
    check_equal(cmeta_fs_stat(path, &file_stat), -ENOENT);
  }

  const char *mismatches[] = {NULL, "run_id", "thread_id", "journal_id"};
  for (size_t i = 0; i < sizeof(mismatches) / sizeof(mismatches[0]); ++i) {
    it("rejects a %s journal even when focus excludes it", mismatches[i] ? mismatches[i] : "missing") {
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); check_not_null(metadata);
      if (mismatches[i]) check_equal(put_diagram_journal(&store, metadata, journal_key, mismatches[i]), SALTS_OK);
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      focus = tstr_format("run:{}", json_get_string(metadata, "last_run_id")); check_not_null(focus);
      fclose(output); output = tmpfile(); check_not_null(output);
      options.prompt = NULL; options.diagram = "json"; options.diagram_journals = journal_key;
      options.diagram_focus = focus; options.diagram_depth = "0";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0); check_equal(model.calls, 1);
    }
  }

  for (int as_json = 0; as_json < 2; ++as_json) {
    it("finds a two-hop journal relationship with explicit depth in %s", as_json ? "JSON" : "Mermaid") {
      char buffer[16384];
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(metadata);
      check_equal(put_diagram_journal(&store, metadata, journal_key, NULL), SALTS_OK);
      check_equal(put_diagram_journal(&store, metadata, other_journal_key, NULL), SALTS_OK);
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      focus = tstr_format("journal:{}", journal_key); check_not_null(focus);
      destination = tstr_format("journal:{}", other_journal_key); check_not_null(destination);
      options.prompt = NULL; options.diagram = as_json ? "json" : "mermaid";
      options.diagram_journals = "all"; options.diagram_focus = focus; options.diagram_to = destination;
      fclose(output); output = tmpfile(); check_not_null(output);
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0L); /* Default depth remains one. */
      options.diagram_depth = "2";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      rewind(output); size_t size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      check_null(strstr(buffer, "private-output")); check_null(strstr(buffer, "private-detail"));
      if (as_json) {
        run_record = json_parse(buffer, size); check_not_null(run_record);
        const json_value_t *selection = json_object_get(run_record, "selection");
        check_equal(json_get_string(selection, "mode"), "shortest_path");
        check_equal(json_get_string(selection, "target_id"), destination);
        check_equal(json_get_int(selection, "hop_count", -1), 2);
        check_equal(json_get_string(run_record, "session_phase"), "ready");
        check_equal(json_array_size(json_object_get(run_record, "nodes")), 3u);
        check_equal(json_array_size(json_object_get(run_record, "edges")), 2u);
      } else {
        check_contains(buffer, "One minimum-hop path"); check_contains(buffer, "not a causality claim");
      }
      fclose(output); output = tmpfile(); check_not_null(output);
      options.diagram_direction = "outgoing";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0L);
      options.diagram_direction = "both"; options.diagram_to = "unknown-node";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0L); check_equal(model.calls, 1);
    }
  }
  for (int origin_case = 0; origin_case < 8; ++origin_case) {
    it("exports or rejects verified request origins before output case %d", origin_case) {
      const int workflow = origin_case / 4, malformed = (origin_case % 4) / 2, as_json = origin_case % 2;
      const char *state_text = workflow
          ? "{\"plan_versions\":[{\"steps\":[{\"text\":\"private-step\"}]}],\"executor_state_versions\":[{"
            "\"plan_step_origin\":{\"schema_version\":1,\"plan_version\":0,\"step_index\":0},"
            "\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]}]}"
          : "{\"events\":[{\"kind\":\"model\",\"tool_calls\":[{\"call_id\":\"call\",\"name\":\"fs.read\"}]}]}";
      const char *call_pointer = workflow ? "/run_snapshot/state_snapshot/executor_state_versions/0/events/0/tool_calls/0"
          : "/run_snapshot/state_snapshot/events/0/tool_calls/0";
      char buffer[16384];
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(metadata);
      check_equal(put_diagram_journal(&store, metadata, journal_key, NULL), SALTS_OK);
      check_equal(store.get(store.user_data, "thread_heads", json_get_string(metadata, "thread_id"), &text), SALTS_OK);
      run_record = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(run_record);
      focus = tstr_format("run:{}", json_get_string(json_object_get(run_record, "run_snapshot"), "id")); check_not_null(focus);
      json_free(metadata); metadata = json_parse(state_text, strlen(state_text)); check_not_null(metadata);
      if (workflow && malformed) check_equal(turbo_runtime_json_object_set_int64(json_object_get(
          json_array_get(json_object_get(metadata, "executor_state_versions"), 0), "plan_step_origin"), "schema_version", 2), 0);
      check_equal(turbo_runtime_json_object_set(json_object_get(run_record, "run_snapshot"), "state_snapshot", metadata), 0); metadata = NULL;
      text = json_serialize(run_record, NULL); check_not_null(text);
      check_equal(store.put(store.user_data, "thread_heads", json_get_string(run_record, "thread_id"), text), SALTS_OK);
      free(text); text = NULL;
      check_equal(store.get(store.user_data, "agent_tool_journal", journal_key, &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL; check_not_null(metadata);
      check_equal(turbo_runtime_json_object_set_string(metadata, "turn_key", workflow ? "executor:0:0" : "planner:root:0"), 0);
      if (malformed && !workflow) check_equal(turbo_runtime_json_object_set_string(metadata, "tool_name", "fs.write"), 0);
      text = json_serialize(metadata, NULL); check_not_null(text);
      check_equal(store.put(store.user_data, "agent_tool_journal", journal_key, text), SALTS_OK);
      free(text); text = NULL;
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      fclose(output); output = tmpfile(); check_not_null(output);
      options.prompt = NULL; options.diagram = as_json ? "json" : "mermaid";
      options.diagram_journals = "all"; options.diagram_origins = !workflow; options.diagram_plan_steps = workflow;
      if (malformed) { options.diagram_focus = focus; options.diagram_to = focus; options.diagram_depth = "0"; }
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), malformed ? 1 : 0);
      check_equal(model.calls, 1);
      rewind(output); size_t size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      if (malformed) check_equal(size, 0u);
      else {
        check_contains(buffer, "published request");
        check_contains(buffer, "same call identity");
        if (as_json) {
          check_contains(buffer, "matched");
          check_contains(buffer, call_pointer);
        }
        destination = tstr_format("journal:{}", journal_key); check_not_null(destination);
        options.diagram_focus = workflow ? "workflow-step:0:0" : "event:planner:root:0"; options.diagram_to = destination;
        if (workflow) options.diagram_depth = "2";
        options.diagram_direction = "outgoing";
        fclose(output); output = tmpfile(); check_not_null(output);
        check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
        rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
        check_contains(buffer, "same call identity");
        check_null(strstr(buffer, "private-step"));
        if (workflow) check_contains(buffer, "prepared for workflow step");
        if (as_json) {
          json_free(run_record); run_record = json_parse(buffer, size); check_not_null(run_record);
          check_equal(json_array_size(json_object_get(run_record, "nodes")), workflow ? 3u : 2u);
          check_equal(json_array_size(json_object_get(run_record, "edges")), workflow ? 2u : 1u);
          check_equal(json_get_string(json_object_get(run_record, "selection"), "mode"), "shortest_path");
          check_contains(buffer, call_pointer);
        } else check_contains(buffer, "One minimum-hop path");
        check_equal(model.calls, 1);
      }
    }
  }
  it("rejects orphaned or malformed path options without opening a session or model") {
    options.prompt = NULL; options.diagram_to = "run:r";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram = "json";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_focus = "run:r";
    const char *invalid[] = {"", "\xc0\xaf", "\x01"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      options.diagram_to = invalid[i];
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    }
    check_equal(model.calls, 0); check_equal(ftell(output), 0L);
  }
  it("rejects origins without a journal selection before opening a session or model") {
    options.prompt = NULL; options.diagram_origins = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram = "json";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_origins = 0; options.diagram_plan_steps = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    check_equal(model.calls, 0);
    check_equal(ftell(output), 0L);
  }
  for (int as_json = 0; as_json < 2; ++as_json) {
    it("focuses a published run in %s while preserving provenance and session phase", as_json ? "JSON" : "Mermaid") {
      char buffer[8192];
      size_t size;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); check_not_null(metadata);
      focus = tstr_format("run:{}", json_get_string(metadata, "last_run_id")); check_not_null(focus);
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      fclose(output); output = tmpfile(); check_not_null(output);
      options.prompt = NULL; options.diagram = as_json ? "json" : "mermaid"; options.diagram_focus = focus;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(model.calls, 1);
      rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
      if (as_json) {
        const json_value_t *selection, *node;
        run_record = json_parse(buffer, size); check_not_null(run_record);
        selection = json_object_get(run_record, "selection");
        check_equal(json_get_string(selection, "focus_id"), focus);
        check_equal(json_get_string(selection, "direction"), "both");
        check_equal(json_get_int(selection, "max_hops", -1), 1);
        check_equal(json_get_string(run_record, "session_phase"), "ready");
        check_equal(json_array_size(json_object_get(run_record, "nodes")), 1u);
        node = json_array_get(json_object_get(run_record, "nodes"), 0);
        check_equal(json_get_string(node, "collection"), "thread_heads");
        check_equal(json_get_string(node, "record_id"), json_get_string(metadata, "thread_id"));
      } else {
        check_contains(buffer, "Focused view"); check_contains(buffer, "published snapshot");
      }
      fclose(output); output = tmpfile(); check_not_null(output);
      options.diagram_focus = "n0"; /* Renderer IDs are not source identities. */
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(ftell(output), 0); check_equal(model.calls, 1);
    }
  }

  it("rejects orphaned and malformed focus options before opening a session or invoking the model") {
    options.diagram_focus = "run:r";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.prompt = NULL; options.diagram = "json"; options.diagram_focus = NULL;
    options.diagram_depth = "1";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_depth = NULL; options.diagram_direction = "incoming";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_focus = "";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_focus = "\x01";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_focus = "run:r"; options.diagram_direction = "sideways";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    options.diagram_direction = "outgoing";
    {
      const char *depths[] = {"", "-1", "128", "9999999999999999999", "1x", "+1", " 1"};
      for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
        options.diagram_depth = depths[i];
        check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
      }
    }
    check_equal(model.calls, 0); check_equal(ftell(output), 0);
  }

  it("labels active sessions as incomplete and refuses corrupt heads without output") {
    char buffer[8192];
    size_t size;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text)); free(text); text = NULL;
    check_equal(turbo_runtime_json_object_set_string(metadata, "phase", "active"), 0);
    text = json_serialize(metadata, NULL); check_not_null(text);
    check_equal(store.put(store.user_data, "cli_sessions", "current", text), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    fclose(output); output = tmpfile(); check_not_null(output);
    options.prompt = NULL; options.diagram = "mermaid";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    rewind(output); size = fread(buffer, 1, sizeof(buffer) - 1, output); buffer[size] = 0;
    check_contains(buffer, "Session active: unpublished work is not shown.");
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "thread_heads", json_get_string(metadata, "thread_id"), "{}"), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    fclose(output); output = tmpfile(); check_not_null(output);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(ftell(output), 0);
    check_equal(model.calls, 1);
  }

  it("reopens a completed session and projects its exact prior run into the next turn") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(model.calls, 1);
    options.prompt = "followup_task"; options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(model.calls, 2);
    check_true(model.ordered);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text));
    check_equal(json_get_string(metadata, "phase"), "ready");
    check_equal(turbo_runtime_json_value_as_int64(json_object_get(metadata, "turn_count"), -1), 2);
  }

  it("refuses an existing session without resume and preserves it for explicit continuation") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
    options.resume = 1; options.prompt = "followup_task";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_true(model.ordered);
  }

  it("retains unfinished work and prevents model or tool replay on resume") {
    model.fail = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
    check_equal(ftell(output), 0);
    model.fail = 0; options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text));
    check_equal(json_get_string(metadata, "phase"), "active");
  }

  it("rejects missing sessions and invalid resume options before model invocation") {
    options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    options.session_db = NULL;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 2);
    check_equal(model.calls, 0);
  }

  it("rejects workspace changes and malformed metadata instead of starting fresh") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    options.resume = 1; options.workspace = ".";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.put(store.user_data, "cli_sessions", "current", "{}"), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    options.workspace = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
  }

  it("rejects a missing published run instead of silently losing history") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text)); free(text); text = NULL;
    check_equal(turbo_runtime_json_object_set_string(metadata, "last_run_id", "missing"), 0);
    text = json_serialize(metadata, NULL);
    check_not_null(text);
    check_equal(store.put(store.user_data, "cli_sessions", "current", text), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 1);
  }

  const char *run_fields[] = {"id", "thread_id", "status"};
  for (size_t i = 0; i < sizeof(run_fields) / sizeof(run_fields[0]); ++i) {
    it("rejects a published run with mismatching %s before model invocation", run_fields[i]) {
      const char *thread_id;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
      check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
      check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
      metadata = json_parse(text, strlen(text)); free(text); text = NULL;
      thread_id = json_get_string(metadata, "thread_id"); check_not_null(thread_id);
      check_equal(store.get(store.user_data, "thread_heads", thread_id, &text), SALTS_OK);
      run_record = json_parse(text, strlen(text)); free(text); text = NULL;
      check_equal(turbo_runtime_json_object_set_string(json_object_get(run_record, "run_snapshot"), run_fields[i], "wrong"), 0);
      text = json_serialize(run_record, NULL); check_not_null(text);
      check_equal(store.put(store.user_data, "thread_heads", thread_id, text), SALTS_OK);
      store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
      options.resume = 1;
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
      check_equal(model.calls, 1);
    }
  }

  it("enforces the persisted turn limit across launches") {
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(turbo_runtime_store_sqlite_open(&store_config, &store), SALTS_OK);
    check_equal(store.get(store.user_data, "cli_sessions", "current", &text), SALTS_OK);
    metadata = json_parse(text, strlen(text)); free(text); text = NULL;
    check_equal(turbo_runtime_json_object_set_int64(metadata, "turn_count", 63), 0);
    text = json_serialize(metadata, NULL); check_not_null(text);
    check_equal(store.put(store.user_data, "cli_sessions", "current", text), SALTS_OK);
    store.user_data_free(store.user_data); store = (turbo_agent_runtime_store_t){0};
    options.resume = 1; options.prompt = "followup_task";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(model.calls, 2);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 1);
    check_equal(model.calls, 2);
  }

  it("continues an empty saved session after EOF without fabricating history") {
    options.prompt = NULL;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(model.calls, 0);
    options.resume = 1; options.prompt = "initial_task";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), 0);
    check_equal(model.calls, 1);
  }

  for (int crash = 0; crash < 2; ++crash) {
    it("handles a previous process that %s", crash ? "exits abruptly during its turn" : "completes its turn") {
      const char *args[] = {directory, path, crash ? "crash" : "complete", NULL};
      cmeta_process_options_t child_options;
      cmeta_process_result_t result;
      cmeta_process_options_init(&child_options);
      child_options.program = CLI_STORAGE_FIXTURE; child_options.args = args;
      child_options.timeout_ms = 5000; child_options.max_output_bytes = 4096;
      check_equal(cmeta_process_spawn(&child_options, &process), SALTS_OK);
      check_equal(cmeta_process_wait(process, &result), SALTS_OK);
      check_equal(result.state, SALTS_PROCESS_EXITED);
      check_equal(result.exit_code, crash ? 23 : 0);
      options.resume = 1; options.prompt = "followup_task";
      check_equal(turbo_cli_run(&options, input, output, diagnostics, &model_config, NULL), crash ? 1 : 0);
      check_equal(model.calls, crash ? 0 : 1);
      if (!crash) check_true(model.ordered);
    }
  }
}
