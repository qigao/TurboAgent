#include "tinytest.h"
#include "turbo_document_loader.h"

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

static char *document_loader_test_strdup(const char *text) {
  size_t len = strlen(text);
  char *copy = (char *)malloc(len + 1);

  check_not_null(copy);
  memcpy(copy, text, len + 1);
  return copy;
}

static char *document_loader_test_temp_dir(void) {
  char path[512];
#ifdef _WIN32
  char temp_dir[MAX_PATH];
  DWORD len = GetTempPathA(MAX_PATH, temp_dir);
  check_true(len > 0);
  snprintf(path, sizeof(path), "%sturbonet_doc_loader_%llu", temp_dir,
           (unsigned long long)GetTickCount64());
#else
  snprintf(path, sizeof(path), "/tmp/turbonet_doc_loader_%lu",
           (unsigned long)getpid());
#endif
  check_int_eq(TEST_MKDIR(path), 0);
  return document_loader_test_strdup(path);
}

static char *document_loader_test_path(const char *dir, const char *name) {
  char path[512];

  snprintf(path, sizeof(path), "%s%s%s", dir, TEST_SEP, name);
  return document_loader_test_strdup(path);
}

static void document_loader_test_write_file(const char *path,
                                            const char *text) {
  FILE *file = fopen(path, "wb");

  check_not_null(file);
  check_size_eq(fwrite(text, 1, strlen(text), file), strlen(text));
  check_int_eq(fclose(file), 0);
}

static void document_loader_test_write_binary_file(const char *path) {
  static const unsigned char bytes[] = {'a', '\0', 'b'};
  FILE *file = fopen(path, "wb");

  check_not_null(file);
  check_size_eq(fwrite(bytes, 1, sizeof(bytes), file), sizeof(bytes));
  check_int_eq(fclose(file), 0);
}

spec("turbo document loader api") {
  it("should load a local text file as a document json object") {
    char *dir = document_loader_test_temp_dir();
    char *path = document_loader_test_path(dir, "notes.txt");
    json_value_t *document = NULL;

    document_loader_test_write_file(path, "loader reads local docs\n");
    check_int_eq(turbo_document_loader_load_text_file(path, "note", &document),
                 0);
    check_str_eq(turbo_json_get_string(document, "id"), path);
    check_str_eq(turbo_json_get_string(document, "uri"), path);
    check_str_eq(turbo_json_get_string(document, "kind"), "note");
    check_str_eq(turbo_json_get_string(document, "text"),
                 "loader reads local docs\n");
    check_not_null(turbo_json_get_string(document, "content_hash"));
    check_int_eq(turbo_json_get_int(document, "size", -1),
                 (int)strlen("loader reads local docs\n"));

    turbo_free_json(&document);
    remove(path);
    free(path);
    TEST_RMDIR(dir);
    free(dir);
  }

  it("should reject binary files") {
    char *dir = document_loader_test_temp_dir();
    char *path = document_loader_test_path(dir, "binary.bin");
    json_value_t *document = NULL;

    document_loader_test_write_binary_file(path);
    check_int_eq(turbo_document_loader_load_text_file(path, "file", &document),
                 -1);
    check_null(document);

    remove(path);
    free(path);
    TEST_RMDIR(dir);
    free(dir);
  }
}
