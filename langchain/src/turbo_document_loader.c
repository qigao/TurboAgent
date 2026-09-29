#include "turbo_document_loader.h"
#include <json_parser.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t turbo_document_loader_hash_bytes(const unsigned char *data,
                                                 size_t size) {
  uint64_t hash = 1469598103934665603ull;
  size_t i;

  for (i = 0; i < size; ++i) {
    hash ^= (uint64_t)data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

static char *turbo_document_loader_hash_text(const char *text, size_t size) {
  char buffer[32];
  uint64_t hash;
  char *copy;
  size_t len;

  if (!text) {
    return NULL;
  }
  hash = turbo_document_loader_hash_bytes((const unsigned char *)text, size);
  snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)hash);
  len = strlen(buffer);
  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, buffer, len + 1);
  return copy;
}

static char *turbo_document_loader_read_file(const char *path, size_t *out_size,
                                             int64_t *out_mtime) {
  FILE *file;
  long size_long;
  size_t size;
  char *buffer;
  struct stat st;

  if (!path || !out_size || !out_mtime) {
    return NULL;
  }
  file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  size_long = ftell(file);
  if (size_long < 0) {
    fclose(file);
    return NULL;
  }
  if (fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  size = (size_t)size_long;
  buffer = (char *)malloc(size + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  if (size > 0 && fread(buffer, 1, size, file) != size) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  fclose(file);
  buffer[size] = '\0';
  *out_size = size;
  *out_mtime = stat(path, &st) == 0 ? (int64_t)st.st_mtime : 0;
  return buffer;
}

int turbo_document_loader_load_text_file(
    const char *path, const char *kind, json_value_t **out_document_json) {
  json_value_t *document;
  char *text;
  char *hash;
  size_t size = 0;
  int64_t mtime = 0;

  if (!path || !path[0] || !out_document_json) {
    return -1;
  }
  *out_document_json = NULL;
  text = turbo_document_loader_read_file(path, &size, &mtime);
  if (!text) {
    return -1;
  }
  if (size > 0 && memchr(text, '\0', size) != NULL) {
    free(text);
    return -1;
  }
  hash = turbo_document_loader_hash_text(text, size);
  if (!hash) {
    free(text);
    return -1;
  }
  document = json_create_object();
  if (!document) {
    free(hash);
    free(text);
    return -1;
  }
  json_object_set_string(document, "id", path);
  json_object_set_string(document, "uri", path);
  json_object_set_string(document, "kind", kind ? kind : "file");
  json_object_set_string(document, "title", path);
  json_object_set_string(document, "text", text);
  json_object_set_string(document, "content_hash", hash);
  json_object_set_number(document, "size", (double)size);
  json_object_set_number(document, "mtime", (double)mtime);
  free(hash);
  free(text);
  *out_document_json = document;
  return 0;
}
