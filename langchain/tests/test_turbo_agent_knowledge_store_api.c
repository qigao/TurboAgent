#include "tinytest.h"
#include "turbo_agent_knowledge_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#define TEST_SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_MKDIR(path) mkdir(path, 0700)
#define TEST_RMDIR(path) rmdir(path)
#define TEST_SEP "/"
#endif

static int knowledge_test_dummy_transport(const char *request_json,
                                          char **out_response_json,
                                          void *user_data) {
  (void)request_json;
  (void)out_response_json;
  (void)user_data;
  return -1;
}

static turbo_agent_t *knowledge_test_agent_create(const char *model) {
  turbo_agent_config_t config = {0};

  config.model = model;
  config.transport_fn = knowledge_test_dummy_transport;
  return turbo_agent_create(&config);
}

static char *knowledge_test_strdup(const char *text) {
  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);
  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static char *knowledge_test_temp_dir(void) {
  char path[512];
#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  snprintf(path, sizeof(path), "%sturbonet_knowledge_%llu", temp_dir,
           (unsigned long long)GetTickCount64());
#else
  snprintf(path, sizeof(path), "/tmp/turbonet_knowledge_%lu", (unsigned long)getpid());
#endif
  check_int_eq(TEST_MKDIR(path), 0);
  return knowledge_test_strdup(path);
}

static char *knowledge_test_path(const char *dir, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s%s%s", dir, TEST_SEP, name);
  return knowledge_test_strdup(path);
}

static char *knowledge_test_json_path(const char *path) {
  char *copy = knowledge_test_strdup(path);
  size_t i;

  for (i = 0; copy[i] != '\0'; ++i) {
    if (copy[i] == '\\') {
      copy[i] = '/';
    }
  }
  return copy;
}

static void knowledge_test_write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  check_not_null(file);
  check_size_eq(fwrite(text, 1, strlen(text), file), strlen(text));
  check_int_eq(fclose(file), 0);
}

static void knowledge_test_write_binary_file(const char *path) {
  static const unsigned char bytes[] = {'a', '\0', 'b'};
  FILE *file = fopen(path, "wb");
  check_not_null(file);
  check_size_eq(fwrite(bytes, 1, sizeof(bytes), file), sizeof(bytes));
  check_int_eq(fclose(file), 0);
}

static void knowledge_test_json_value_set_string(json_value_t *object,
                                           const char *key, const char *value) {
  json_value_t *field =
      turbo_json_create_string(value);
  check_not_null(field);
  check_int_eq(turbo_runtime_json_object_set(object, key, field),
               TURBO_RUNTIME_JSON_OK);
}

static void knowledge_test_json_value_set_int64(json_value_t *object,
                                          const char *key, int64_t value) {
  json_value_t *field =
      turbo_json_create_int64(value);
  check_not_null(field);
  check_int_eq(turbo_runtime_json_object_set(object, key, field),
               TURBO_RUNTIME_JSON_OK);
}

static const json_value_t *knowledge_test_first_result(const json_value_t *results) {
  check_not_null(results);
  check_int_eq((int)turbo_json_type(results), TURBO_JSON_ARRAY);
  check_true(turbo_json_array_size(results) > 0);
  return turbo_json_array_get(results, 0);
}

static const json_value_t *knowledge_test_find_kind_stat(const json_value_t *kinds,
                                                         const char *kind) {
  size_t i;

  check_not_null(kinds);
  check_int_eq((int)turbo_json_type(kinds), TURBO_JSON_ARRAY);
  for (i = 0; i < turbo_json_array_size(kinds); ++i) {
    const json_value_t *item = turbo_json_array_get(kinds, i);
    if (item && strcmp(turbo_json_get_string(item, "kind"), kind) == 0) {
      return item;
    }
  }
  return NULL;
}

spec("turbo agent knowledge store") {
  it("should index, query, update, and delete text documents") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t document = {0};
    json_value_t *results = NULL;
    json_value_t *documents = NULL;
    json_value_t *loaded_document = NULL;
    json_value_t *stats = NULL;
    const json_value_t *first;
    const json_value_t *chunks;
    const json_value_t *kinds;

    check_not_null(store);
    document.id = "doc-1";
    document.uri = "memory://doc-1";
    document.kind = "note";
    document.title = "Planning note";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document,
                     "planner uses graph state and retrieval context", 256),
                 0);
    check_int_eq(turbo_agent_knowledge_store_query(store, "retrieval context", 5,
                                                   &results),
                 0);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "document_id"), "doc-1");
    check_str_eq(turbo_json_get_string(first, "uri"), "memory://doc-1");
    check_not_null(strstr(turbo_json_get_string(first, "text"), "retrieval"));
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_query(
                     store, "please use local docs for retrieval context", 5,
                     &results),
                 0);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "document_id"), "doc-1");
    check_not_null(strstr(turbo_json_get_string(first, "text"), "retrieval"));
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_stats(store, &stats), 0);
    check_not_null(turbo_json_object_get(stats, "fts5_enabled"));
    check_int_eq(turbo_json_get_int(stats, "document_count", 0), 1);
    check_int_eq(turbo_json_get_int(stats, "chunk_count", 0), 1);
    kinds = turbo_json_object_get(stats, "kinds");
    check_not_null(kinds);
    check_size_eq(turbo_json_array_size(kinds), 1);
    first = turbo_json_array_get(kinds, 0);
    check_str_eq(turbo_json_get_string(first, "kind"), "note");
    check_int_eq(turbo_json_get_int(first, "document_count", 0), 1);
    check_int_eq(turbo_json_get_int(first, "chunk_count", 0), 1);
    turbo_free_json(&stats);

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document, "executor writes final answer", 256),
                 0);
    check_int_eq(turbo_agent_knowledge_store_query(store, "retrieval context", 5,
                                                   &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 0);
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_query(store, "final answer", 5,
                                                   &results),
                 0);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "kind"), "note");
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_list_documents(
                     store, "note", "memory://", 10, &documents),
                 0);
    first = knowledge_test_first_result(documents);
    check_str_eq(turbo_json_get_string(first, "id"), "doc-1");
    check_str_eq(turbo_json_get_string(first, "uri"), "memory://doc-1");
    check_str_eq(turbo_json_get_string(first, "kind"), "note");
    check_int_eq(turbo_json_get_int(first, "chunk_count", 0), 1);
    turbo_free_json(&documents);

    check_int_eq(turbo_agent_knowledge_store_get_document(store, "doc-1",
                                                          &loaded_document),
                 0);
    check_str_eq(turbo_json_get_string(loaded_document, "id"), "doc-1");
    chunks = turbo_json_object_get(loaded_document, "chunks");
    check_not_null(chunks);
    check_size_eq(turbo_json_array_size(chunks), 1);
    first = turbo_json_array_get(chunks, 0);
    check_not_null(strstr(turbo_json_get_string(first, "text"), "final answer"));
    turbo_free_json(&loaded_document);

    check_int_eq(turbo_agent_knowledge_store_delete_document(store, "doc-1"), 0);
    check_int_eq(turbo_agent_knowledge_store_query(store, "final answer", 5,
                                                   &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 0);
    turbo_free_json(&results);

    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should filter search results by kind and uri prefix") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t note = {0};
    turbo_agent_knowledge_document_t project = {0};
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    note.id = "note-1";
    note.uri = "memory://note-1";
    note.kind = "note";
    note.title = "Note";
    project.id = "project-1";
    project.uri = "project://docs/project-1";
    project.kind = "project";
    project.title = "Project";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &note, "shared planner context from notes", 128),
                 0);
    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &project, "shared planner context from project docs", 128),
                 0);

    check_int_eq(turbo_agent_knowledge_store_query_ex(
                     store, "planner context", "project", "project://docs/", 5,
                     &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "document_id"), "project-1");
    check_str_eq(turbo_json_get_string(first, "kind"), "project");
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_query_ex(
                     store, "planner context", "project", "memory://", 5,
                     &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 0);
    turbo_free_json(&results);

    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should describe recommended knowledge tool dependencies") {
    json_value_t *graph = NULL;
    const json_value_t *nodes;
    const json_value_t *edges;

    check_int_eq(turbo_agent_knowledge_store_tool_graph(&graph), 0);
    nodes = turbo_json_object_get(graph, "nodes");
    edges = turbo_json_object_get(graph, "edges");
    check_not_null(nodes);
    check_not_null(edges);
    check_true(turbo_json_array_size(nodes) >= 9);
    check_true(turbo_json_array_size(edges) >= 7);
    check_str_eq(turbo_json_get_string(turbo_json_array_get(nodes, 0), "name"),
                 "agent.knowledge.stats");
    check_str_eq(turbo_json_get_string(turbo_json_array_get(edges, 0), "from"),
                 "agent.knowledge.stats");

    turbo_free_json(&graph);
  }

  it("should index text files and reject binary files") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    char *text_path = knowledge_test_path(dir, "notes.txt");
    char *binary_path = knowledge_test_path(dir, "binary.bin");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    knowledge_test_write_file(text_path, "local rag stores citations in sqlite fts5\n");
    knowledge_test_write_binary_file(binary_path);

    check_int_eq(turbo_agent_knowledge_store_index_file(store, text_path, "file", 64), 0);
    check_int_eq(turbo_agent_knowledge_store_index_file(store, binary_path, "file", 64), -1);
    check_int_eq(turbo_agent_knowledge_store_query(store, "sqlite fts5", 3, &results), 0);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "uri"), text_path);
    turbo_free_json(&results);

    turbo_agent_knowledge_store_close(store);
    remove(binary_path);
    remove(text_path);
    remove(db_path);
    free(binary_path);
    free(text_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should index directories recursively with a summary") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    char *docs_dir = knowledge_test_path(dir, "docs");
    char *nested_dir = knowledge_test_path(docs_dir, "nested");
    char *build_dir = knowledge_test_path(docs_dir, "build");
    char *root_path = knowledge_test_path(docs_dir, "root.txt");
    char *nested_path = knowledge_test_path(nested_dir, "deep.md");
    char *binary_path = knowledge_test_path(docs_dir, "binary.txt");
    char *log_path = knowledge_test_path(docs_dir, "skip.log");
    char *large_path = knowledge_test_path(docs_dir, "large.md");
    char *ignored_path = knowledge_test_path(build_dir, "ignored.txt");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_directory_options_t options = {0};
    json_value_t *summary = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_int_eq(TEST_MKDIR(docs_dir), 0);
    check_int_eq(TEST_MKDIR(nested_dir), 0);
    check_int_eq(TEST_MKDIR(build_dir), 0);
    knowledge_test_write_file(root_path, "directory rag root document\n");
    knowledge_test_write_file(nested_path, "nested graph planner document\n");
    knowledge_test_write_binary_file(binary_path);
    knowledge_test_write_file(log_path, "extension filtered document\n");
    knowledge_test_write_file(large_path,
                              "large document should be skipped by size limit "
                              "because it is larger than the configured bound\n");
    knowledge_test_write_file(ignored_path, "ignored build artifact document\n");

    options.kind = "docs";
    options.chunk_target_bytes = 64;
    options.recursive = 1;
    options.max_file_bytes = 80;
    options.include_extensions = ".txt,.md";
    check_int_eq(turbo_agent_knowledge_store_index_directory_ex(
                     store, docs_dir, &options, &summary),
                 0);
    check_not_null(summary);
    check_int_eq(turbo_json_get_int(summary, "visited", -1), 5);
    check_int_eq(turbo_json_get_int(summary, "indexed", -1), 2);
    check_int_eq(turbo_json_get_int(summary, "skipped", -1), 3);
    check_int_eq(turbo_json_get_int(summary, "skipped_by_extension", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "skipped_too_large", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "skipped_index_error", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "failed", -1), 0);
    turbo_free_json(&summary);

    check_int_eq(turbo_agent_knowledge_store_query(store, "nested graph planner", 3,
                                                   &results),
                 0);
    first = knowledge_test_first_result(results);
    check_str_eq(turbo_json_get_string(first, "uri"), nested_path);
    turbo_free_json(&results);

    check_int_eq(turbo_agent_knowledge_store_query(store, "ignored build artifact", 3,
                                                   &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 0);
    turbo_free_json(&results);

    turbo_agent_knowledge_store_close(store);
    remove(ignored_path);
    remove(large_path);
    remove(log_path);
    remove(binary_path);
    remove(nested_path);
    remove(root_path);
    remove(db_path);
    TEST_RMDIR(build_dir);
    TEST_RMDIR(nested_dir);
    TEST_RMDIR(docs_dir);
    free(ignored_path);
    free(large_path);
    free(log_path);
    free(binary_path);
    free(nested_path);
    free(root_path);
    free(build_dir);
    free(nested_dir);
    free(docs_dir);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should expose knowledge indexing and search as registry tools") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    char *text_path = knowledge_test_path(dir, "tool-notes.txt");
    char *json_dir = knowledge_test_json_path(dir);
    char *json_text_path = knowledge_test_json_path(text_path);
    char *tool_docs_dir = knowledge_test_path(dir, "tool-docs");
    char *tool_docs_path = knowledge_test_path(tool_docs_dir, "bulk.txt");
    char *json_tool_docs_dir = knowledge_test_json_path(tool_docs_dir);
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    char arguments_json[1024];
    char *output = NULL;
    json_value_t *output_json = NULL;
    json_value_t *post_delete_results = NULL;
    json_value_t *search_args =
        turbo_json_create_object();
    json_value_t *search_result = NULL;
    const json_value_t *results_value;
    const json_value_t *context_value;

    check_not_null(store);
    check_not_null(registry);
    check_not_null(search_args);
    knowledge_test_write_file(text_path, "planner graph retrieves local context\n");
    check_int_eq(TEST_MKDIR(tool_docs_dir), 0);
    knowledge_test_write_file(tool_docs_path, "directory tool indexes graph notes\n");

    check_int_eq(turbo_agent_knowledge_store_add_tools(registry, store), 0);
    check_size_eq(turbo_tool_registry_count(registry), 10);

    snprintf(arguments_json, sizeof(arguments_json),
             "{\"path\":\"%s\",\"kind\":\"note\",\"chunk_target_bytes\":128}",
             json_text_path);
    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.index_file",
                                             arguments_json, &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output), &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_str_eq(turbo_json_get_string(output_json, "uri"), json_text_path);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    snprintf(arguments_json, sizeof(arguments_json),
             "{\"root_dir\":\"%s\",\"kind\":\"note\",\"recursive\":true,"
             "\"max_files\":5,\"chunk_target_bytes\":128}",
             json_tool_docs_dir);
    check_int_eq(turbo_tool_registry_execute(
                     registry, "agent.knowledge.index_directory",
                     arguments_json, &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_int_eq(turbo_json_get_int(turbo_json_object_get(output_json, "stats"),
                                    "indexed", -1),
                 1);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    check_int_eq(turbo_tool_registry_execute(
                     registry, "agent.knowledge.upsert_text",
                     "{\"id\":\"memory://tool-memory\",\"kind\":\"memo\","
                     "\"title\":\"Tool memory\","
                     "\"text\":\"ephemeral tool memory for planner recall\","
                     "\"chunk_target_bytes\":128}",
                     &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_str_eq(turbo_json_get_string(output_json, "document_id"),
                 "memory://tool-memory");
    check_str_eq(turbo_json_get_string(output_json, "kind"), "memo");
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.stats",
                                             "{}", &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    {
      const json_value_t *stats_json = turbo_json_object_get(output_json, "stats");
      const json_value_t *kind_stats;

      check_true(turbo_json_get_int(stats_json, "document_count", 0) >= 3);
      check_true(turbo_json_get_int(stats_json, "chunk_count", 0) >= 3);
      kind_stats = knowledge_test_find_kind_stat(
          turbo_json_object_get(stats_json, "kinds"), "memo");
      check_not_null(kind_stats);
      check_int_eq(turbo_json_get_int(kind_stats, "document_count", 0), 1);
      kind_stats = knowledge_test_find_kind_stat(
          turbo_json_object_get(stats_json, "kinds"), "note");
      check_not_null(kind_stats);
      check_true(turbo_json_get_int(kind_stats, "document_count", 0) >= 2);
    }
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.tool_graph",
                                             "{}", &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_true(turbo_json_array_size(turbo_json_object_get(
                   turbo_json_object_get(output_json, "graph"), "edges")) >= 7);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.list_documents",
                                             "{\"kind\":\"note\",\"limit\":10}",
                                             &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_int_eq((int)turbo_json_array_size(
                     turbo_json_object_get(output_json, "documents")),
                 2);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    snprintf(arguments_json, sizeof(arguments_json),
             "{\"document_id\":\"%s\"}", json_text_path);
    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.get_document",
                                             arguments_json, &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_str_eq(turbo_json_get_string(turbo_json_object_get(output_json, "document"),
                                       "id"),
                 json_text_path);
    check_true(turbo_json_array_size(turbo_json_object_get(
                   turbo_json_object_get(output_json, "document"), "chunks")) > 0);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;

    knowledge_test_json_value_set_string(search_args, "query", "local context");
    knowledge_test_json_value_set_string(search_args, "kind", "note");
    knowledge_test_json_value_set_string(search_args, "uri_prefix", json_dir);
    knowledge_test_json_value_set_int64(search_args, "limit", 3);
    check_int_eq(turbo_tool_registry_execute_json_value(registry, "agent.knowledge.search",
                                                  search_args, &search_result),
                 TURBO_TOOL_OK);
    check_not_null(search_result);
    check_true(turbo_runtime_json_value_as_bool(
        turbo_json_object_get(search_result, "ok"), 0));
    results_value = turbo_json_object_get(search_result, "results");
    check_not_null(results_value);
    check_int_eq((int)turbo_json_type(results_value),
                 TURBO_JSON_ARRAY);
    check_true(turbo_runtime_json_value_size(results_value) > 0);

    turbo_runtime_json_destroy(search_result);
    search_result = NULL;
    check_int_eq(turbo_tool_registry_execute_json_value(
                     registry, "agent.knowledge.build_context", search_args,
                     &search_result),
                 TURBO_TOOL_OK);
    check_not_null(search_result);
    context_value = turbo_json_object_get(search_result, "context");
    check_not_null(context_value);
    check_int_eq((int)turbo_json_type(context_value),
                 TURBO_JSON_OBJECT);
    check_true(turbo_runtime_json_value_as_string(
                   turbo_json_object_get(context_value, "context_text")) != NULL);

    turbo_runtime_json_destroy(search_result);
    search_result = NULL;
    snprintf(arguments_json, sizeof(arguments_json),
             "{\"document_id\":\"%s\"}", json_text_path);
    check_int_eq(turbo_tool_registry_execute(registry, "agent.knowledge.delete_document",
                                             arguments_json, &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_int_eq(turbo_parse_json((const uint8_t *)output, strlen(output),
                                  &output_json),
                 0);
    check_true(turbo_json_get_bool(output_json, "ok", false));
    check_str_eq(turbo_json_get_string(output_json, "document_id"), json_text_path);
    turbo_free_json(&output_json);
    free(output);
    output = NULL;
    check_int_eq(turbo_agent_knowledge_store_query(store, "local context", 3,
                                                   &post_delete_results),
                 0);
    check_size_eq(turbo_json_array_size(post_delete_results), 0);
    turbo_free_json(&post_delete_results);
    turbo_runtime_json_destroy(search_args);
    turbo_tool_registry_destroy(registry);
    turbo_agent_knowledge_store_close(store);
    remove(tool_docs_path);
    remove(text_path);
    remove(db_path);
    TEST_RMDIR(tool_docs_dir);
    free(json_tool_docs_dir);
    free(tool_docs_path);
    free(tool_docs_dir);
    free(json_text_path);
    free(json_dir);
    free(text_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should expose knowledge tools through the action registry") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    char *text_path = knowledge_test_path(dir, "action-notes.txt");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_action_tool_registry_t *action_registry =
        turbo_action_tool_registry_create();
    const turbo_action_tool_definition_t *index_definition;
    const turbo_action_tool_definition_t *search_definition;
    const turbo_action_tool_definition_t *build_context_definition;
    const turbo_action_tool_definition_t *stats_definition;
    const turbo_action_tool_definition_t *tool_graph_definition;
    const turbo_action_tool_definition_t *list_definition;
    const turbo_action_tool_definition_t *get_definition;
    const turbo_action_tool_definition_t *upsert_definition;
    const turbo_action_tool_definition_t *delete_definition;
    json_value_t *upsert_args = turbo_json_create_object();
    json_value_t *index_args = turbo_json_create_object();
    json_value_t *search_args = turbo_json_create_object();
    json_value_t *get_args = turbo_json_create_object();
    json_value_t *result = NULL;
    const json_value_t *results;
    const json_value_t *document;

    check_not_null(store);
    check_not_null(action_registry);
    check_not_null(upsert_args);
    check_not_null(index_args);
    check_not_null(search_args);
    check_not_null(get_args);
    knowledge_test_write_file(text_path, "action registry indexes local graph context\n");

    check_int_eq(turbo_agent_knowledge_store_add_action_tools(action_registry, store),
                 0);
    check_size_eq(turbo_action_tool_registry_count(action_registry), 10);
    index_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.index_file");
    search_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.search");
    build_context_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.build_context");
    stats_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.stats");
    tool_graph_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.tool_graph");
    list_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.list_documents");
    get_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.get_document");
    upsert_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.upsert_text");
    delete_definition =
        turbo_action_tool_registry_find(action_registry, "agent.knowledge.delete_document");
    check_not_null(index_definition);
    check_not_null(search_definition);
    check_not_null(build_context_definition);
    check_not_null(stats_definition);
    check_not_null(tool_graph_definition);
    check_not_null(list_definition);
    check_not_null(get_definition);
    check_not_null(upsert_definition);
    check_not_null(delete_definition);
    check_int_eq(index_definition->kind, TURBO_ACTION_MUTATE);
    check_int_eq(search_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(build_context_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(stats_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(tool_graph_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(list_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(get_definition->kind, TURBO_ACTION_OBSERVE);
    check_int_eq(upsert_definition->kind, TURBO_ACTION_MUTATE);
    check_int_eq(delete_definition->kind, TURBO_ACTION_MUTATE);

    turbo_json_object_set_string(upsert_args, "id", "memory://action-memory");
    turbo_json_object_set_string(upsert_args, "kind", "memo");
    turbo_json_object_set_string(upsert_args, "text",
                                 "action registry remembers planner memory");
    turbo_json_object_set_number(upsert_args, "chunk_target_bytes", 128);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.upsert_text", upsert_args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_str_eq(turbo_json_get_string(result, "document_id"),
                 "memory://action-memory");
    check_str_eq(turbo_json_get_string(result, "kind"), "memo");
    turbo_free_json(&result);

    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.stats", NULL, &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_true(turbo_json_get_int(turbo_json_object_get(result, "stats"),
                                  "document_count", 0) >= 1);
    check_true(turbo_json_get_int(turbo_json_object_get(result, "stats"),
                                  "chunk_count", 0) >= 1);
    turbo_free_json(&result);

    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.tool_graph", NULL, &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_true(turbo_json_array_size(turbo_json_object_get(
                   turbo_json_object_get(result, "graph"), "nodes")) >= 9);
    turbo_free_json(&result);

    turbo_json_object_set_string(index_args, "path", text_path);
    turbo_json_object_set_string(index_args, "kind", "note");
    turbo_json_object_set_number(index_args, "chunk_target_bytes", 128);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.index_file", index_args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_str_eq(turbo_json_get_string(result, "uri"), text_path);
    turbo_free_json(&result);

    turbo_json_object_set_string(search_args, "query", "local graph context");
    turbo_json_object_set_string(search_args, "kind", "note");
    turbo_json_object_set_string(search_args, "uri_prefix", dir);
    turbo_json_object_set_number(search_args, "limit", 3);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.search", search_args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    results = turbo_json_object_get(result, "results");
    check_not_null(results);
    check_true(turbo_json_array_size(results) > 0);

    turbo_free_json(&result);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.build_context", search_args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_not_null(strstr(turbo_json_get_string(
                              turbo_json_object_get(result, "context"), "context_text"),
                          "local graph context"));

    turbo_free_json(&result);
    turbo_json_object_set_string(get_args, "document_id", text_path);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.knowledge.get_document", get_args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    document = turbo_json_object_get(result, "document");
    check_not_null(document);
    check_str_eq(turbo_json_get_string(document, "id"), text_path);
    check_true(turbo_json_array_size(turbo_json_object_get(document, "chunks")) > 0);

    turbo_free_json(&result);
    turbo_free_json(&get_args);
    turbo_free_json(&search_args);
    turbo_free_json(&index_args);
    turbo_free_json(&upsert_args);
    turbo_action_tool_registry_destroy(action_registry);
    turbo_agent_knowledge_store_close(store);
    remove(text_path);
    remove(db_path);
    free(text_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should load retrieved chunks into agent memory context") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t document = {0};
    json_value_t *state = turbo_agent_state_create();
    char *memory_text = NULL;

    check_not_null(store);
    check_not_null(state);
    document.id = "doc-context";
    document.uri = "memory://doc-context";
    document.kind = "note";
    document.title = "Context note";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document,
                     "planner should read local sqlite fts5 retrieval context", 256),
                 0);
    check_int_eq(turbo_agent_knowledge_store_load_context(store, state,
                                                          "sqlite fts5 retrieval", 2),
                 0);
    check_size_eq(turbo_agent_state_memory_layer_count(state), 1);
    check_str_eq(turbo_json_get_string(turbo_agent_state_memory_layer_at(state, 0),
                                       "scope"),
                 "knowledge");
    check_str_eq(turbo_json_get_string(turbo_agent_state_memory_layer_at(state, 0),
                                       "path"),
                 "memory://doc-context");
    memory_text = turbo_agent_state_memory_context_text(state);
    check_not_null(memory_text);
    check_not_null(strstr(memory_text, "sqlite fts5 retrieval"));

    free(memory_text);
    turbo_free_json(&state);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should load filtered knowledge context into agent memory") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t note = {0};
    turbo_agent_knowledge_document_t project = {0};
    json_value_t *state = turbo_agent_state_create();
    json_value_t *context = NULL;
    const json_value_t *first;
    char *memory_text = NULL;

    check_not_null(store);
    check_not_null(state);
    note.id = "ctx-note";
    note.uri = "memory://ctx-note";
    note.kind = "note";
    note.title = "Context note";
    project.id = "ctx-project";
    project.uri = "project://docs/ctx-project";
    project.kind = "project";
    project.title = "Project context";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &note, "shared filtered context from notes", 128),
                 0);
    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &project, "shared filtered context from project docs", 128),
                 0);

    check_int_eq(turbo_agent_knowledge_store_load_context_ex(
                     store, state, "filtered context", "project",
                     "project://docs/", 5),
                 0);
    check_size_eq(turbo_agent_state_memory_layer_count(state), 1);
    check_str_eq(turbo_json_get_string(turbo_agent_state_memory_layer_at(state, 0),
                                       "path"),
                 "project://docs/ctx-project");
    memory_text = turbo_agent_state_memory_context_text(state);
    check_not_null(memory_text);
    check_not_null(strstr(memory_text, "project docs"));
    check_null(strstr(memory_text, "from notes"));

    check_int_eq(turbo_agent_knowledge_store_build_context(
                     store, "filtered context", "project", "project://docs/", 5,
                     &context),
                 0);
    check_int_eq(turbo_json_get_int(context, "layer_count", 0), 1);
    check_not_null(strstr(turbo_json_get_string(context, "context_text"),
                          "project docs"));
    check_null(strstr(turbo_json_get_string(context, "context_text"), "from notes"));
    check_size_eq(turbo_json_array_size(turbo_json_object_get(context, "evidence")), 1);
    first = turbo_json_array_get(turbo_json_object_get(context, "evidence"), 0);
    check_str_eq(turbo_json_get_string(first, "document_id"), "ctx-project");
    check_str_eq(turbo_json_get_string(first, "uri"), "project://docs/ctx-project");

    turbo_free_json(&context);
    free(memory_text);
    turbo_free_json(&state);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should expose a graph node that loads knowledge context from user input") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t document = {0};
    turbo_agent_knowledge_context_config_t config = {0};
    turbo_graph_exec_ctx_t ctx = {0};
    json_value_t *state = turbo_agent_state_create();

    check_not_null(store);
    check_not_null(state);
    document.id = "doc-node";
    document.uri = "memory://doc-node";
    document.kind = "note";
    document.title = "Node note";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &document,
                     "agent graph can hydrate planner context from local docs", 256),
                 0);
    check_int_eq(turbo_agent_state_add_user_message(
                     state, "hydrate planner context from local docs"),
                 0);
    config.store = store;
    config.limit = 3;
    ctx.state = state;
    check_int_eq(turbo_agent_knowledge_context_node(&ctx, &config), 0);
    check_size_eq(turbo_agent_state_memory_layer_count(state), 1);
    check_str_eq(turbo_json_get_string(turbo_agent_state_memory_layer_at(state, 0),
                                       "path"),
                 "memory://doc-node");

    turbo_free_json(&state);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should expose a graph node that loads filtered knowledge context") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_document_t note = {0};
    turbo_agent_knowledge_document_t project = {0};
    turbo_agent_knowledge_context_config_t config = {0};
    turbo_graph_exec_ctx_t ctx = {0};
    json_value_t *state = turbo_agent_state_create();

    check_not_null(store);
    check_not_null(state);
    note.id = "node-note";
    note.uri = "memory://node-note";
    note.kind = "note";
    note.title = "Node note";
    project.id = "node-project";
    project.uri = "project://docs/node-project";
    project.kind = "project";
    project.title = "Node project";

    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &note, "graph filtered planner context from notes", 128),
                 0);
    check_int_eq(turbo_agent_knowledge_store_upsert_text(
                     store, &project, "graph filtered planner context from project docs", 128),
                 0);
    check_int_eq(turbo_agent_state_add_user_message(
                     state, "filtered planner context"),
                 0);
    config.store = store;
    config.kind = "project";
    config.uri_prefix = "project://docs/";
    config.limit = 3;
    ctx.state = state;

    check_int_eq(turbo_agent_knowledge_context_node(&ctx, &config), 0);
    check_size_eq(turbo_agent_state_memory_layer_count(state), 1);
    check_str_eq(turbo_json_get_string(turbo_agent_state_memory_layer_at(state, 0),
                                       "path"),
                 "project://docs/node-project");

    turbo_free_json(&state);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should install a knowledge-enhanced engineering loop") {
    char *dir = knowledge_test_temp_dir();
    char *db_path = knowledge_test_path(dir, "knowledge.sqlite3");
    turbo_agent_knowledge_store_t *store =
        turbo_agent_knowledge_store_sqlite_open(db_path);
    turbo_agent_knowledge_context_config_t config = {0};
    turbo_graph_t *graph = turbo_graph_create("knowledge-engineering-loop");
    turbo_agent_t *planner = knowledge_test_agent_create("planner-test");
    turbo_agent_t *executor = knowledge_test_agent_create("executor-test");

    check_not_null(store);
    check_not_null(graph);
    check_not_null(planner);
    check_not_null(executor);
    config.store = store;
    config.kind = "note";
    config.limit = 3;

    check_int_eq(turbo_agent_install_knowledge_engineering_loop(
                     graph, &config, planner, executor, "knowledge", "planner",
                     "plan_commit", "plan_step", "review", "executor", "tools",
                     "detect_failure", "replan_route", "replan_prepare",
                     "plan_advance", "end", 1),
                 TURBO_GRAPH_EXEC_OK);
    check_str_eq(turbo_graph_get_entry(graph), "knowledge");
    check_size_eq(turbo_graph_node_count(graph), 13);
    check_size_eq(turbo_graph_edge_count(graph), 17);
    check_not_null(turbo_graph_topology_id(graph));

    turbo_agent_destroy(executor);
    turbo_agent_destroy(planner);
    turbo_graph_destroy(graph);
    turbo_agent_knowledge_store_close(store);
    remove(db_path);
    free(db_path);
    TEST_RMDIR(dir);
    free(dir);
  }
}
