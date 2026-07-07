#include "tinytest.h"
#include "turbo_action_tool.h"
#include "turbo_embedding.h"
#include "turbo_retriever.h"
#include "turbo_tool_registry.h"
#include "turbo_vector_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define VECTOR_STORE_TEST_MKDIR(path) _mkdir(path)
#define VECTOR_STORE_TEST_RMDIR(path) _rmdir(path)
#define VECTOR_STORE_TEST_SEP "\\"
#else
#include <sys/stat.h>
#include <unistd.h>
#define VECTOR_STORE_TEST_MKDIR(path) mkdir(path, 0700)
#define VECTOR_STORE_TEST_RMDIR(path) rmdir(path)
#define VECTOR_STORE_TEST_SEP "/"
#endif

static char *vector_store_test_strdup(const char *text) {
  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);

  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static char *vector_store_test_temp_dir(void) {
  char path[512];
#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  snprintf(path, sizeof(path), "%sturbonet_vector_store_%llu", temp_dir,
           (unsigned long long)GetTickCount64());
#else
  snprintf(path, sizeof(path), "/tmp/turbonet_vector_store_%lu",
           (unsigned long)getpid());
#endif
  check_int_eq(VECTOR_STORE_TEST_MKDIR(path), 0);
  return vector_store_test_strdup(path);
}

static char *vector_store_test_path(const char *dir, const char *name) {
  char path[512];

  snprintf(path, sizeof(path), "%s%s%s", dir, VECTOR_STORE_TEST_SEP, name);
  return vector_store_test_strdup(path);
}

static void vector_store_test_write_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");

  check_not_null(file);
  check_size_eq(fwrite(text, 1, strlen(text), file), strlen(text));
  check_int_eq(fclose(file), 0);
}

static void vector_store_test_write_binary_file(const char *path) {
  static const unsigned char bytes[] = {'v', '\0', 'x'};
  FILE *file = fopen(path, "wb");

  check_not_null(file);
  check_size_eq(fwrite(bytes, 1, sizeof(bytes), file), sizeof(bytes));
  check_int_eq(fclose(file), 0);
}

static json_value_t *vector_store_test_embedding2(double a, double b) {
  json_value_t *embedding = turbo_json_create_array();

  check_not_null(embedding);
  turbo_json_array_add(embedding, turbo_json_create_number(a));
  turbo_json_array_add(embedding, turbo_json_create_number(b));
  return embedding;
}

static int vector_store_test_upsert2(turbo_vector_store_t *store,
                                     const char *id, const char *uri,
                                     const char *kind, const char *title,
                                     const char *text, double a, double b) {
  turbo_vector_store_document_t document = {0};
  json_value_t *embedding = vector_store_test_embedding2(a, b);
  int rc;

  document.id = id;
  document.uri = uri;
  document.kind = kind;
  document.title = title;
  document.text = text;
  rc = turbo_vector_store_upsert(store, &document, embedding);
  turbo_free_json(&embedding);
  return rc;
}

spec("turbo vector store api") {
  it("should rank and filter in-memory vector documents") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_vector_store_query_options_t options = {0};
    json_value_t *query = vector_store_test_embedding2(1.0, 0.0);
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_int_eq(vector_store_test_upsert2(
                     store, "planner-doc", "memory://planner", "note",
                     "Planner note", "planner local docs", 1.0, 0.0),
                 0);
    check_int_eq(vector_store_test_upsert2(
                     store, "tool-doc", "memory://tool", "note", "Tool note",
                     "tool registry docs", 0.0, 1.0),
                 0);
    check_int_eq(vector_store_test_upsert2(
                     store, "log-doc", "log://planner", "log", "Planner log",
                     "planner trace docs", 1.0, 0.0),
                 0);
    check_size_eq(turbo_vector_store_count(store), 3);

    options.kind = "note";
    options.uri_prefix = "memory://";
    options.limit = 1;
    check_int_eq(turbo_vector_store_query(store, query, &options, &results), 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"), "planner-doc");
    check_str_eq(turbo_json_get_string(first, "text"), "planner local docs");
    check_int_eq((int)turbo_json_get_double(first, "score", 0.0), 1);

    turbo_free_json(&results);
    turbo_free_json(&query);
    turbo_vector_store_destroy(store);
  }

  it("should replace and delete documents by id") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    json_value_t *query = vector_store_test_embedding2(0.0, 1.0);
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_int_eq(vector_store_test_upsert2(
                     store, "doc", "memory://old", "note", "Old",
                     "old vector", 1.0, 0.0),
                 0);
    check_int_eq(vector_store_test_upsert2(
                     store, "doc", "memory://new", "note", "New",
                     "new vector", 0.0, 1.0),
                 0);
    check_size_eq(turbo_vector_store_count(store), 1);
    check_int_eq(turbo_vector_store_query(store, query, NULL, &results), 0);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "uri"), "memory://new");
    check_str_eq(turbo_json_get_string(first, "title"), "New");

    turbo_free_json(&results);
    check_int_eq(turbo_vector_store_delete_document(store, "doc"), 0);
    check_size_eq(turbo_vector_store_count(store), 0);
    check_int_eq(turbo_vector_store_query(store, query, NULL, &results), 0);
    check_size_eq(turbo_json_array_size(results), 0);

    turbo_free_json(&results);
    turbo_free_json(&query);
    turbo_vector_store_destroy(store);
  }

  it("should persist vector documents in SQLite stores") {
    char *dir = vector_store_test_temp_dir();
    char *db_path = vector_store_test_path(dir, "vectors.sqlite3");
    turbo_vector_store_t *store = turbo_vector_store_sqlite_open(db_path);
    turbo_vector_store_query_options_t options = {0};
    turbo_vector_store_document_t document = {0};
    json_value_t *query = vector_store_test_embedding2(1.0, 0.0);
    json_value_t *embedding = turbo_json_create_array();
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding);
    check_int_eq(vector_store_test_upsert2(
                     store, "planner-doc", "sqlite://planner", "note",
                     "Planner note", "planner persisted docs", 1.0, 0.0),
                 0);
    check_int_eq(vector_store_test_upsert2(
                     store, "tool-doc", "sqlite://tool", "note", "Tool note",
                     "tool persisted docs", 0.0, 1.0),
                 0);
    check_size_eq(turbo_vector_store_count(store), 2);
    turbo_vector_store_destroy(store);

    store = turbo_vector_store_sqlite_open(db_path);
    check_not_null(store);
    check_size_eq(turbo_vector_store_count(store), 2);
    options.kind = "note";
    options.uri_prefix = "sqlite://";
    options.limit = 1;
    check_int_eq(turbo_vector_store_query(store, query, &options, &results), 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"), "planner-doc");
    check_str_eq(turbo_json_get_string(first, "text"), "planner persisted docs");
    turbo_free_json(&results);

    document.id = "bad-dimension";
    document.text = "bad";
    turbo_json_array_add(embedding, turbo_json_create_number(1.0));
    turbo_json_array_add(embedding, turbo_json_create_number(0.0));
    turbo_json_array_add(embedding, turbo_json_create_number(0.0));
    check_int_eq(turbo_vector_store_upsert(store, &document, embedding), -1);

    check_int_eq(turbo_vector_store_delete_document(store, "planner-doc"), 0);
    check_size_eq(turbo_vector_store_count(store), 1);
    turbo_vector_store_destroy(store);

    store = turbo_vector_store_sqlite_open(db_path);
    check_not_null(store);
    check_size_eq(turbo_vector_store_count(store), 1);
    check_int_eq(turbo_vector_store_query(store, query, &options, &results), 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"), "tool-doc");

    turbo_free_json(&results);
    turbo_free_json(&embedding);
    turbo_free_json(&query);
    turbo_vector_store_destroy(store);
    remove(db_path);
    free(db_path);
    VECTOR_STORE_TEST_RMDIR(dir);
    free(dir);
  }

  it("should reject mixed embedding dimensions") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_vector_store_document_t document = {0};
    json_value_t *embedding = turbo_json_create_array();

    check_not_null(store);
    check_int_eq(vector_store_test_upsert2(
                     store, "doc-a", "memory://a", "note", "A", "alpha",
                     1.0, 0.0),
                 0);

    document.id = "doc-b";
    document.text = "beta";
    check_not_null(embedding);
    turbo_json_array_add(embedding, turbo_json_create_number(1.0));
    turbo_json_array_add(embedding, turbo_json_create_number(0.0));
    turbo_json_array_add(embedding, turbo_json_create_number(0.0));
    check_int_eq(turbo_vector_store_upsert(store, &document, embedding), -1);

    turbo_free_json(&embedding);
    turbo_vector_store_destroy(store);
  }

  it("should expose vector stores through retrievers") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_retriever_t *retriever;
    turbo_retriever_query_options_t options = {0};
    turbo_vector_store_document_t document = {0};
    json_value_t *document_embedding = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding_model);

    document.id = "planner-vector";
    document.uri = "memory://planner-vector";
    document.kind = "note";
    document.title = "Planner vector";
    document.text = "planner context local retrieval";
    check_int_eq(turbo_embedding_model_embed_text(embedding_model, document.text,
                                                  &document_embedding),
                 0);
    check_int_eq(turbo_vector_store_upsert(store, &document, document_embedding),
                 0);

    retriever = turbo_retriever_from_vector_store(store, embedding_model);
    check_not_null(retriever);
    options.kind = "note";
    options.limit = 1;
    check_int_eq(turbo_retriever_query(retriever, "planner context",
                                       &options, &results),
                 0);
    first = turbo_json_array_get(results, 0);
    check_not_null(first);
    check_str_eq(turbo_json_get_string(first, "document_id"),
                 "planner-vector");

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    turbo_free_json(&document_embedding);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
  }

  it("should index text chunks with an embedding model") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_vector_store_document_t document = {0};
    turbo_text_splitter_options_t splitter_options = {0};
    turbo_retriever_t *retriever;
    turbo_retriever_query_options_t query_options = {0};
    json_value_t *summary = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding_model);
    document.id = "local-doc";
    document.uri = "memory://local-doc";
    document.kind = "note";
    document.title = "Local doc";
    document.text = "planner context local retrieval\n"
                    "tool registry execution details\n";
    splitter_options.chunk_size = 32;

    check_int_eq(turbo_vector_store_index_text(store, embedding_model, &document,
                                               &splitter_options, &summary),
                 0);
    check_true(turbo_json_get_bool(summary, "ok", false));
    check_int_eq(turbo_json_get_int(summary, "chunk_count", 0), 2);
    check_size_eq(turbo_vector_store_count(store), 2);

    retriever = turbo_retriever_from_vector_store(store, embedding_model);
    check_not_null(retriever);
    query_options.kind = "note";
    query_options.limit = 1;
    check_int_eq(turbo_retriever_query(retriever, "tool registry",
                                       &query_options, &results),
                 0);
    first = turbo_json_array_get(results, 0);
    check_not_null(first);
    check_str_eq(turbo_json_get_string(first, "document_id"),
                 "local-doc#chunk-1");
    check_not_null(strstr(turbo_json_get_string(first, "text"),
                          "tool registry"));
    turbo_free_json(&results);
    turbo_free_json(&summary);
    results = NULL;
    summary = NULL;

    document.text = "replacement retrieval only";
    splitter_options.chunk_size = 128;
    check_int_eq(turbo_vector_store_index_text(store, embedding_model, &document,
                                               &splitter_options, &summary),
                 0);
    check_true(turbo_json_get_bool(summary, "ok", false));
    check_int_eq(turbo_json_get_int(summary, "chunk_count", 0), 1);
    check_size_eq(turbo_vector_store_count(store), 1);
    check_int_eq(turbo_retriever_query(retriever, "replacement retrieval",
                                       &query_options, &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"),
                 "local-doc#chunk-0");
    check_str_eq(turbo_json_get_string(first, "text"),
                 "replacement retrieval only");

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    turbo_free_json(&summary);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
  }

  it("should replace indexed chunks in SQLite stores") {
    char *dir = vector_store_test_temp_dir();
    char *db_path = vector_store_test_path(dir, "indexed.sqlite3");
    turbo_vector_store_t *store = turbo_vector_store_sqlite_open(db_path);
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_vector_store_document_t document = {0};
    turbo_text_splitter_options_t splitter_options = {0};
    turbo_vector_store_query_options_t query_options = {0};
    json_value_t *summary = NULL;
    json_value_t *query_embedding = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding_model);
    document.id = "sqlite-local-doc";
    document.uri = "sqlite://local-doc";
    document.kind = "note";
    document.title = "SQLite local doc";
    document.text = "planner context local retrieval\n"
                    "tool registry execution details\n";
    splitter_options.chunk_size = 32;
    check_int_eq(turbo_vector_store_index_text(store, embedding_model, &document,
                                               &splitter_options, &summary),
                 0);
    check_int_eq(turbo_json_get_int(summary, "chunk_count", 0), 2);
    check_size_eq(turbo_vector_store_count(store), 2);
    turbo_free_json(&summary);

    document.text = "sqlite replacement retrieval only";
    splitter_options.chunk_size = 128;
    check_int_eq(turbo_vector_store_index_text(store, embedding_model, &document,
                                               &splitter_options, &summary),
                 0);
    check_true(turbo_json_get_bool(summary, "ok", false));
    check_int_eq(turbo_json_get_int(summary, "chunk_count", 0), 1);
    check_size_eq(turbo_vector_store_count(store), 1);

    check_int_eq(turbo_embedding_model_embed_text(embedding_model,
                                                  "sqlite replacement",
                                                  &query_embedding),
                 0);
    query_options.kind = "note";
    query_options.limit = 8;
    check_int_eq(turbo_vector_store_query(store, query_embedding,
                                          &query_options, &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 1);
    first = turbo_json_array_get(results, 0);
    check_str_eq(turbo_json_get_string(first, "document_id"),
                 "sqlite-local-doc#chunk-0");
    check_str_eq(turbo_json_get_string(first, "text"),
                 "sqlite replacement retrieval only");

    turbo_free_json(&results);
    turbo_free_json(&query_embedding);
    turbo_free_json(&summary);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
    remove(db_path);
    free(db_path);
    VECTOR_STORE_TEST_RMDIR(dir);
    free(dir);
  }

  it("should index a local text file through the document loader") {
    char *dir = vector_store_test_temp_dir();
    char *path = vector_store_test_path(dir, "local.txt");
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_text_splitter_options_t splitter_options = {0};
    turbo_retriever_t *retriever;
    turbo_retriever_query_options_t query_options = {0};
    json_value_t *summary = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding_model);
    vector_store_test_write_file(path, "loader vector retrieval\n"
                                       "planner file context\n");
    splitter_options.chunk_size = 28;
    check_int_eq(turbo_vector_store_index_text_file(
                     store, embedding_model, path, "file", &splitter_options,
                     &summary),
                 0);
    check_true(turbo_json_get_bool(summary, "ok", false));
    check_int_eq(turbo_json_get_int(summary, "chunk_count", 0), 2);
    check_size_eq(turbo_vector_store_count(store), 2);

    retriever = turbo_retriever_from_vector_store(store, embedding_model);
    check_not_null(retriever);
    query_options.kind = "file";
    query_options.uri_prefix = dir;
    query_options.limit = 1;
    check_int_eq(turbo_retriever_query(retriever, "planner context",
                                       &query_options, &results),
                 0);
    first = turbo_json_array_get(results, 0);
    check_not_null(first);
    check_not_null(strstr(turbo_json_get_string(first, "text"),
                          "planner file context"));

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    turbo_free_json(&summary);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
    remove(path);
    free(path);
    VECTOR_STORE_TEST_RMDIR(dir);
    free(dir);
  }

  it("should index directories recursively with filters") {
    char *dir = vector_store_test_temp_dir();
    char *docs_dir = vector_store_test_path(dir, "docs");
    char *nested_dir = vector_store_test_path(docs_dir, "nested");
    char *build_dir = vector_store_test_path(docs_dir, "build");
    char *root_path = vector_store_test_path(docs_dir, "root.txt");
    char *nested_path = vector_store_test_path(nested_dir, "deep.md");
    char *binary_path = vector_store_test_path(docs_dir, "binary.txt");
    char *log_path = vector_store_test_path(docs_dir, "skip.log");
    char *large_path = vector_store_test_path(docs_dir, "large.md");
    char *ignored_path = vector_store_test_path(build_dir, "ignored.txt");
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_vector_store_directory_options_t options = {0};
    turbo_retriever_t *retriever;
    turbo_retriever_query_options_t query_options = {0};
    json_value_t *summary = NULL;
    json_value_t *results = NULL;
    const json_value_t *first;

    check_not_null(store);
    check_not_null(embedding_model);
    check_int_eq(VECTOR_STORE_TEST_MKDIR(docs_dir), 0);
    check_int_eq(VECTOR_STORE_TEST_MKDIR(nested_dir), 0);
    check_int_eq(VECTOR_STORE_TEST_MKDIR(build_dir), 0);
    vector_store_test_write_file(root_path, "directory vector root document\n");
    vector_store_test_write_file(nested_path, "nested vector planner document\n");
    vector_store_test_write_binary_file(binary_path);
    vector_store_test_write_file(log_path, "extension filtered document\n");
    vector_store_test_write_file(
        large_path,
        "large vector document should be skipped by size because it is too "
        "large for the configured bound\n");
    vector_store_test_write_file(ignored_path, "ignored build artifact document\n");

    options.kind = "docs";
    options.splitter_options.chunk_size = 128;
    options.recursive = 1;
    options.max_file_bytes = 80;
    options.include_extensions = ".txt,.md";
    check_int_eq(turbo_vector_store_index_directory_ex(
                     store, embedding_model, docs_dir, &options, &summary),
                 0);
    check_not_null(summary);
    check_int_eq(turbo_json_get_int(summary, "visited", -1), 5);
    check_int_eq(turbo_json_get_int(summary, "indexed", -1), 2);
    check_int_eq(turbo_json_get_int(summary, "indexed_chunks", -1), 2);
    check_int_eq(turbo_json_get_int(summary, "skipped", -1), 3);
    check_int_eq(turbo_json_get_int(summary, "skipped_by_extension", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "skipped_too_large", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "skipped_index_error", -1), 1);
    check_int_eq(turbo_json_get_int(summary, "failed", -1), 0);
    check_false(turbo_json_get_bool(summary, "truncated", true));
    check_size_eq(turbo_vector_store_count(store), 2);

    retriever = turbo_retriever_from_vector_store(store, embedding_model);
    check_not_null(retriever);
    query_options.kind = "docs";
    query_options.uri_prefix = docs_dir;
    query_options.limit = 1;
    check_int_eq(turbo_retriever_query(retriever, "nested planner document",
                                       &query_options, &results),
                 0);
    first = turbo_json_array_get(results, 0);
    check_not_null(first);
    check_str_eq(turbo_json_get_string(first, "uri"), nested_path);
    turbo_free_json(&results);

    check_int_eq(turbo_retriever_query(retriever, "ignored build artifact",
                                       &query_options, &results),
                 0);
    check_true(turbo_json_array_size(results) <= 1);
    if (turbo_json_array_size(results) == 1) {
      first = turbo_json_array_get(results, 0);
      check_null(strstr(turbo_json_get_string(first, "text"), "ignored build"));
    }

    turbo_free_json(&results);
    turbo_retriever_destroy(retriever);
    turbo_free_json(&summary);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
    remove(ignored_path);
    remove(large_path);
    remove(log_path);
    remove(binary_path);
    remove(nested_path);
    remove(root_path);
    VECTOR_STORE_TEST_RMDIR(build_dir);
    VECTOR_STORE_TEST_RMDIR(nested_dir);
    VECTOR_STORE_TEST_RMDIR(docs_dir);
    free(ignored_path);
    free(large_path);
    free(log_path);
    free(binary_path);
    free(nested_path);
    free(root_path);
    free(build_dir);
    free(nested_dir);
    free(docs_dir);
    VECTOR_STORE_TEST_RMDIR(dir);
    free(dir);
  }

  it("should expose observe-only vector tools through registries") {
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_vector_store_tool_binding_t *binding;
    turbo_tool_registry_t *registry = turbo_tool_registry_create();
    turbo_action_tool_registry_t *action_registry =
        turbo_action_tool_registry_create();
    turbo_vector_store_document_t document = {0};
    json_value_t *embedding = NULL;
    json_value_t *action_args = NULL;
    json_value_t *action_result = NULL;
    json_value_t *graph = NULL;
    char *output = NULL;

    check_not_null(store);
    check_not_null(embedding_model);
    check_not_null(registry);
    check_not_null(action_registry);

    document.id = "tool-vector";
    document.uri = "memory://tool-vector";
    document.kind = "note";
    document.title = "Tool vector";
    document.text = "planner context vector registry";
    check_int_eq(turbo_embedding_model_embed_text(embedding_model, document.text,
                                                  &embedding),
                 0);
    check_int_eq(turbo_vector_store_upsert(store, &document, embedding), 0);

    binding = turbo_vector_store_tool_binding_create(store, embedding_model);
    check_not_null(binding);
    check_int_eq(turbo_vector_store_add_tools(registry, binding), 0);
    check_size_eq(turbo_tool_registry_count(registry), 4);
    check_int_eq(turbo_tool_registry_execute(
                     registry, "agent.vector.search",
                     "{\"query\":\"planner context\",\"kind\":\"note\","
                     "\"limit\":1}",
                     &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_not_null(strstr(output, "\"tool-vector\""));
    free(output);
    output = NULL;

    check_int_eq(turbo_tool_registry_execute(
                     registry, "agent.vector.build_context",
                     "{\"query\":\"planner context\",\"kind\":\"note\","
                     "\"limit\":1}",
                     &output),
                 TURBO_TOOL_OK);
    check_not_null(output);
    check_not_null(strstr(output, "\"context_text\""));
    free(output);
    output = NULL;

    check_int_eq(turbo_vector_store_add_action_tools(action_registry, binding), 0);
    check_size_eq(turbo_action_tool_registry_count(action_registry), 4);
    action_args = turbo_json_create_object();
    check_not_null(action_args);
    turbo_json_object_set_string(action_args, "query", "planner context");
    turbo_json_object_set_string(action_args, "kind", "note");
    turbo_json_object_set_number(action_args, "limit", 1);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.vector.search", action_args,
                     &action_result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(action_result, "ok", false));
    check_not_null(turbo_json_object_get(action_result, "results"));

    check_int_eq(turbo_vector_store_tool_graph(&graph), 0);
    check_true(turbo_json_array_size(turbo_json_object_get(graph, "nodes")) >= 4);
    check_true(turbo_json_array_size(turbo_json_object_get(graph, "edges")) >= 2);

    turbo_free_json(&graph);
    turbo_free_json(&action_result);
    turbo_free_json(&action_args);
    turbo_vector_store_tool_binding_destroy(binding);
    turbo_action_tool_registry_destroy(action_registry);
    turbo_tool_registry_destroy(registry);
    turbo_free_json(&embedding);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
  }

  it("should expose opt-in vector indexing action tools") {
    char *dir = vector_store_test_temp_dir();
    char *text_path = vector_store_test_path(dir, "action-file.txt");
    char *docs_dir = vector_store_test_path(dir, "docs");
    char *docs_path = vector_store_test_path(docs_dir, "bulk.txt");
    turbo_vector_store_t *store = turbo_vector_store_create_memory();
    turbo_embedding_model_t *embedding_model =
        turbo_embedding_model_create_hashing(16);
    turbo_vector_store_tool_binding_t *binding;
    turbo_action_tool_registry_t *action_registry =
        turbo_action_tool_registry_create();
    const turbo_action_tool_definition_t *upsert_definition;
    const turbo_action_tool_definition_t *index_file_definition;
    const turbo_action_tool_definition_t *index_directory_definition;
    const turbo_action_tool_definition_t *delete_definition;
    json_value_t *args = NULL;
    json_value_t *result = NULL;
    json_value_t *query_embedding = NULL;
    json_value_t *results = NULL;
    turbo_vector_store_query_options_t query_options = {0};

    check_not_null(store);
    check_not_null(embedding_model);
    check_not_null(action_registry);
    check_int_eq(VECTOR_STORE_TEST_MKDIR(docs_dir), 0);
    vector_store_test_write_file(text_path, "vector action file context\n");
    vector_store_test_write_file(docs_path, "vector directory action context\n");

    binding = turbo_vector_store_tool_binding_create(store, embedding_model);
    check_not_null(binding);
    check_int_eq(turbo_vector_store_add_indexing_action_tools(action_registry,
                                                              binding),
                 0);
    check_size_eq(turbo_action_tool_registry_count(action_registry), 4);
    upsert_definition =
        turbo_action_tool_registry_find(action_registry, "agent.vector.upsert_text");
    index_file_definition =
        turbo_action_tool_registry_find(action_registry, "agent.vector.index_file");
    index_directory_definition =
        turbo_action_tool_registry_find(action_registry,
                                        "agent.vector.index_directory");
    delete_definition =
        turbo_action_tool_registry_find(action_registry,
                                        "agent.vector.delete_document");
    check_not_null(upsert_definition);
    check_not_null(index_file_definition);
    check_not_null(index_directory_definition);
    check_not_null(delete_definition);
    check_int_eq(upsert_definition->kind, TURBO_ACTION_MUTATE);
    check_int_eq(index_file_definition->kind, TURBO_ACTION_MUTATE);
    check_int_eq(index_directory_definition->kind, TURBO_ACTION_MUTATE);
    check_int_eq(delete_definition->kind, TURBO_ACTION_MUTATE);

    args = turbo_json_create_object();
    check_not_null(args);
    turbo_json_object_set_string(args, "id", "action-text");
    turbo_json_object_set_string(args, "uri", "memory://action-text");
    turbo_json_object_set_string(args, "kind", "note");
    turbo_json_object_set_string(args, "text",
                                 "vector action tools index text context");
    turbo_json_object_set_number(args, "chunk_size", 128);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.vector.upsert_text", args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_not_null(turbo_json_object_get(result, "index"));
    turbo_free_json(&result);
    turbo_free_json(&args);

    args = turbo_json_create_object();
    check_not_null(args);
    turbo_json_object_set_string(args, "path", text_path);
    turbo_json_object_set_string(args, "kind", "file");
    turbo_json_object_set_number(args, "chunk_size", 128);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.vector.index_file", args, &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    turbo_free_json(&result);
    turbo_free_json(&args);

    args = turbo_json_create_object();
    check_not_null(args);
    turbo_json_object_set_string(args, "root_dir", docs_dir);
    turbo_json_object_set_string(args, "kind", "docs");
    turbo_json_object_set_bool(args, "recursive", true);
    turbo_json_object_set_string(args, "include_extensions", ".txt");
    turbo_json_object_set_number(args, "chunk_size", 128);
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.vector.index_directory", args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_int_eq(turbo_json_get_int(turbo_json_object_get(result, "index"),
                                    "indexed", 0),
                 1);
    turbo_free_json(&result);
    turbo_free_json(&args);

    check_size_eq(turbo_vector_store_count(store), 3);
    check_int_eq(turbo_embedding_model_embed_text(embedding_model,
                                                  "directory action context",
                                                  &query_embedding),
                 0);
    query_options.kind = "docs";
    query_options.uri_prefix = docs_dir;
    query_options.limit = 1;
    check_int_eq(turbo_vector_store_query(store, query_embedding,
                                          &query_options, &results),
                 0);
    check_size_eq(turbo_json_array_size(results), 1);
    turbo_free_json(&results);
    turbo_free_json(&query_embedding);

    args = turbo_json_create_object();
    check_not_null(args);
    turbo_json_object_set_string(args, "document_id", "action-text#chunk-0");
    check_int_eq(turbo_action_tool_registry_execute(
                     action_registry, "agent.vector.delete_document", args,
                     &result),
                 TURBO_ACTION_TOOL_OK);
    check_true(turbo_json_get_bool(result, "ok", false));
    check_size_eq(turbo_vector_store_count(store), 2);

    turbo_free_json(&result);
    turbo_free_json(&args);
    turbo_vector_store_tool_binding_destroy(binding);
    turbo_action_tool_registry_destroy(action_registry);
    turbo_embedding_model_destroy(embedding_model);
    turbo_vector_store_destroy(store);
    remove(docs_path);
    remove(text_path);
    VECTOR_STORE_TEST_RMDIR(docs_dir);
    free(docs_path);
    free(docs_dir);
    free(text_path);
    VECTOR_STORE_TEST_RMDIR(dir);
    free(dir);
  }
}
