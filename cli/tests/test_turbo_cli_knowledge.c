#include "tinytest.h"
#include "turbo_cli.h"
#include "turbo_cli_knowledge.h"
#include <cmeta_fs.h>
#include <fmt.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

typedef struct knowledge_model_s {
  int calls, coding, writes, saw_alpha, saw_beta, stale_alpha, saw_custom, saw_project;
} knowledge_model_t;

static int knowledge_model(const char *request, char **out, void *data) {
  knowledge_model_t *model = data;
  const char response[] = "{\"output\":[{\"type\":\"message\",\"role\":\"assistant\","
      "\"content\":[{\"type\":\"output_text\",\"text\":\"answer_saved\"}]}]}";
  model->coding = strstr(request, "fs_read") != NULL;
  model->writes = strstr(request, "fs_write") != NULL || strstr(request, "fs_edit") != NULL ||
      strstr(request, "shell_exec") != NULL;
  model->saw_custom = strstr(request, "custom_instruction_marker") != NULL;
  model->saw_project = strstr(request, "project_instruction_marker") != NULL;
  if (strstr(request, "alpha_evidence_marker")) {
    model->saw_alpha = 1;
    if (model->calls) model->stale_alpha = 1;
  }
  if (strstr(request, "beta_evidence_marker")) model->saw_beta = 1;
  ++model->calls;
  *out = malloc(sizeof(response));
  if (!*out) return -1;
  memcpy(*out, response, sizeof(response));
  return 0;
}

static int write_source(const char *root, const char *name, const char *text) {
  tstr path = tstr_format("{}/{}", root, name);
  cmeta_fs_buf_t bytes = {(char *)text, strlen(text)};
  int rc = path ? cmeta_fs_write_file(path, &bytes) : -1;
  tstr_free(path);
  return rc;
}

spec("CLI general agent and persistent FTS retrieval") {
  static char *directory;
  static tstr knowledge_path, session_path, body, child;
  static FILE *input, *output, *diagnostics;
  static turbo_cli_options_t options;
  static turbo_agent_config_t config;
  static knowledge_model_t model;
  static turbo_agent_knowledge_store_t *store;
  static json_value_t *results, *state;

  before_each() {
    directory = tt_make_temp_dir("turbo-cli-knowledge"); check_not_null(directory);
    knowledge_path = tstr_format("{}/knowledge.db", directory);
    session_path = tstr_format("{}/session.db", directory);
    check_not_null(knowledge_path); check_not_null(session_path);
    input = tmpfile(); output = tmpfile(); diagnostics = tmpfile();
    check_not_null(input); check_not_null(output); check_not_null(diagnostics);
    options = (turbo_cli_options_t){0};
    options.workspace = directory; options.mode = "general"; options.prompt = "alpha";
    model = (knowledge_model_t){0}; config = (turbo_agent_config_t){0};
    config.model = "test-model"; config.transport_fn = knowledge_model; config.transport_user_data = &model;
  }
  after_each() {
    turbo_agent_knowledge_store_close(store); store = NULL;
    json_free(results); results = NULL; json_free(state); state = NULL;
    if (input) fclose(input);
    if (output) fclose(output);
    if (diagnostics) fclose(diagnostics);
    input = output = diagnostics = NULL;
    tstr_free(knowledge_path); knowledge_path = NULL; tstr_free(session_path); session_path = NULL;
    tstr_free(body); body = NULL; tstr_free(child); child = NULL;
    if (directory) tt_remove_tree(directory);
    free(directory); directory = NULL;
  }

  it("parses mode, instructions and knowledge options while preserving defaults") {
    char *arguments[] = {"turboagent", "--mode=general", "--instructions=custom text",
        "--knowledge-db=knowledge.db", "--index-dir=docs", NULL};
    turbo_cli_parse_options(5, arguments, &options);
    check_equal(options.mode, "general"); check_equal(options.instructions, "custom text");
    check_equal(options.knowledge_db, "knowledge.db"); check_equal(options.index_dir, "docs");
    turbo_cli_parse_options(1, arguments, &options);
    check_null(options.mode); check_null(options.instructions);
    check_null(options.knowledge_db); check_null(options.index_dir);
  }

  it("runs a general assistant with custom and project instructions without coding tools") {
    check_equal(write_source(directory, "AGENTS.md", "project_instruction_marker"), 0);
    options.instructions = "custom_instruction_marker";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 1); check_true(model.saw_custom);
    check_false(model.coding); check_false(model.writes); check_true(model.saw_project);
  }

  it("imports real files and retrieves independent context on every interactive turn") {
    check_equal(write_source(directory, "alpha.md", "alpha alpha_evidence_marker"), 0);
    check_equal(write_source(directory, "beta.cpp", "beta beta_evidence_marker"), 0);
    check_equal(write_source(directory, "__init__.py", ""), 0);
    options.knowledge_db = knowledge_path; options.index_dir = directory; options.prompt = NULL;
    fputs("alpha\nbeta\n/quit\n", input); rewind(input);
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 2); check_true(model.saw_alpha); check_true(model.saw_beta);
    check_false(model.stale_alpha); check_false(model.coding);
    store = turbo_agent_knowledge_store_sqlite_open(knowledge_path); check_not_null(store);
    check_equal(turbo_agent_knowledge_store_query(store, "alpha", 8, &results), 0);
    check_equal(json_array_size(results), 1u);
    check_contains(json_get_string(json_array_get(results, 0), "uri"), "alpha.md");
  }

  it("reopens the knowledge index and resumes completed conversation without reimporting") {
    check_equal(write_source(directory, "alpha.txt", "alpha alpha_evidence_marker"), 0);
    options.knowledge_db = knowledge_path; options.index_dir = directory; options.session_db = session_path;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_true(model.saw_alpha);
    options.index_dir = NULL; options.resume = 1;
    model = (knowledge_model_t){0};
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 1); check_true(model.saw_alpha);
  }

  it("adds retrieval to the existing coding mode without granting writes in read-only mode") {
    check_equal(write_source(directory, "alpha.c", "alpha alpha_evidence_marker"), 0);
    options.mode = "coding"; options.read_only = 1;
    options.knowledge_db = knowledge_path; options.index_dir = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_true(model.coding); check_true(model.saw_alpha); check_false(model.writes);
  }

  it("replaces an imported document on explicit reindex without keeping its old text") {
    check_equal(write_source(directory, "note.md", "alpha alpha_evidence_marker"), 0);
    check_equal(turbo_cli_knowledge_open(knowledge_path, directory, NULL, &store), SALTS_OK);
    turbo_agent_knowledge_store_close(store); store = NULL;
    check_equal(write_source(directory, "note.md", "beta beta_evidence_marker"), 0);
    check_equal(turbo_cli_knowledge_open(knowledge_path, directory, NULL, &store), SALTS_OK);
    check_equal(turbo_agent_knowledge_store_query(store, "alpha", 8, &results), 0);
    check_equal(json_array_size(results), 0u);
    json_free(results); results = NULL;
    check_equal(turbo_agent_knowledge_store_query(store, "beta", 8, &results), 0);
    check_equal(json_array_size(results), 1u);
    check_contains(json_get_string(json_array_get(results, 0), "text"), "beta_evidence_marker");
  }

  it("cancels startup import before submitting any model work") {
    atomic_int interrupted = 1;
    check_equal(write_source(directory, "alpha.md", "alpha alpha_evidence_marker"), 0);
    options.knowledge_db = knowledge_path; options.index_dir = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, &interrupted), 130);
    check_equal(model.calls, 0);
  }

  it("refuses an unrelated database without adding knowledge tables") {
    sqlite3 *database = NULL;
    sqlite3_stmt *statement = NULL;
    check_equal(sqlite3_open(knowledge_path, &database), SQLITE_OK);
    check_equal(sqlite3_exec(database, "CREATE TABLE unrelated(value TEXT)", NULL, NULL, NULL), SQLITE_OK);
    check_equal(sqlite3_close(database), SQLITE_OK);
    check_equal(turbo_cli_knowledge_open(knowledge_path, NULL, NULL, &store), SALTS_EPROTO);
    check_null(store);
    check_equal(sqlite3_open_v2(knowledge_path, &database, SQLITE_OPEN_READONLY, NULL), SQLITE_OK);
    check_equal(sqlite3_prepare_v2(database, "SELECT count(*) FROM sqlite_schema", -1, &statement, NULL), SQLITE_OK);
    check_equal(sqlite3_step(statement), SQLITE_ROW);
    check_equal(sqlite3_column_int(statement, 0), 1);
    check_equal(sqlite3_finalize(statement), SQLITE_OK);
    check_equal(sqlite3_close(database), SQLITE_OK);
  }

  it("refuses a completed session as a knowledge database and preserves its resumability") {
    options.session_db = session_path;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(turbo_cli_knowledge_open(session_path, NULL, NULL, &store), SALTS_EPROTO);
    check_null(store);
    options.resume = 1;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 2);
  }

  it("allows an empty index or no match without claiming retrieved evidence") {
    options.knowledge_db = knowledge_path;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 1); check_false(model.saw_alpha);
  }

  it("skips hidden files and generated directories during automatic import") {
    check_equal(write_source(directory, ".hidden.txt", "alpha alpha_evidence_marker"), 0);
    child = tstr_format("{}/build", directory); check_not_null(child);
    check_equal(cmeta_fs_mkdir(child, 0700), 0);
    check_equal(write_source(child, "generated.md", "alpha beta_evidence_marker"), 0);
    options.knowledge_db = knowledge_path; options.index_dir = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_false(model.saw_alpha); check_false(model.saw_beta);
  }

  it("rejects invalid options before opening either database or invoking the model") {
    options.session_db = session_path; options.knowledge_db = knowledge_path;
    options.mode = "unknown";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    options.mode = "general"; options.instructions = "";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    options.instructions = NULL; options.knowledge_db = NULL; options.index_dir = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    options.index_dir = NULL; options.diagram = "json";
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 2);
    check_equal(model.calls, 0);
    cmeta_fs_stat_t info;
    check_not_equal(cmeta_fs_stat(session_path, &info), 0);
    check_not_equal(cmeta_fs_stat(knowledge_path, &info), 0);
  }

  it("fails before a model call for oversized imported text or an unavailable directory") {
    body = tstr_new_len(NULL, 256 * 1024 + 1); check_not_null(body);
    memset(body, 'a', tstr_len(body));
    check_equal(write_source(directory, "large.txt", body), 0);
    options.knowledge_db = knowledge_path; options.index_dir = directory;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    child = tstr_format("{}/absent", directory); check_not_null(child);
    options.index_dir = child;
    check_not_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 0);
    check_equal(model.calls, 0);
  }

  it("fails on oversized stored context instead of sending a partial retrieval") {
    turbo_agent_knowledge_document_t document = {0};
    store = turbo_agent_knowledge_store_sqlite_open(knowledge_path); check_not_null(store);
    body = tstr_new_len(NULL, 70 * 1024); check_not_null(body);
    memset(body, ' ', tstr_len(body)); memcpy(body, "alpha", 5);
    document.id = "large"; document.uri = "large"; document.kind = "file";
    check_equal(turbo_agent_knowledge_store_upsert_text(store, &document, body, tstr_len(body)), 0);
    turbo_agent_knowledge_store_close(store); store = NULL;
    options.knowledge_db = knowledge_path;
    check_equal(turbo_cli_run(&options, input, output, diagnostics, &config, NULL), 1);
    check_equal(model.calls, 0);
  }

  it("preserves valid UTF-8 context when persisted byte chunks divide a codepoint") {
    turbo_agent_knowledge_document_t document = {0};
    store = turbo_agent_knowledge_store_sqlite_open(knowledge_path); check_not_null(store);
    body = tstr_new_len(NULL, 2047); check_not_null(body);
    memset(body, ' ', tstr_len(body)); memcpy(body, "alpha", 5);
    tstr next = tstr_cat(body, "中文 alpha"); check_not_null(next); body = next;
    document.id = "unicode"; document.uri = "unicode"; document.kind = "file";
    check_equal(turbo_agent_knowledge_store_upsert_text(store, &document, body, 2048), 0);
    state = turbo_agent_state_create(); check_not_null(state);
    check_equal(turbo_cli_knowledge_prepare(store, "alpha", state), SALTS_OK);
    const json_value_t *layers = turbo_agent_state_memory_layers(state);
    check_greater_equal(json_array_size(layers), 2u);
    for (size_t i = 0; i < json_array_size(layers); ++i)
      check_true(vstr_utf8_valid(vstr_from_cstr(json_get_string(json_array_get(layers, i), "text"))));
  }
}
