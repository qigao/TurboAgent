#include "turbo_text_splitter.h"
#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

static size_t turbo_text_splitter_next_chunk_len(const char *text,
                                                 size_t remaining,
                                                 size_t target) {
  size_t i;

  if (remaining <= target) {
    return remaining;
  }
  for (i = target; i > target / 2; --i) {
    if (text[i] == '\n') {
      return i + 1;
    }
  }
  return target;
}

static char *turbo_text_splitter_copy_chunk(const char *text, size_t len) {
  char *copy;

  copy = (char *)malloc(len + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text, len);
  copy[len] = '\0';
  return copy;
}

int turbo_text_splitter_split_text(
    const char *text, const turbo_text_splitter_options_t *options,
    json_value_t **out_chunks_json) {
  turbo_text_splitter_options_t effective_options = {0};
  json_value_t *chunks;
  size_t text_len;
  size_t offset = 0;

  if (!text || !out_chunks_json) {
    return -1;
  }
  *out_chunks_json = NULL;
  effective_options.chunk_size = options ? options->chunk_size : 0;
  effective_options.chunk_overlap = options ? options->chunk_overlap : 0;
  if (effective_options.chunk_size == 0) {
    effective_options.chunk_size = 2048;
  }
  if (effective_options.chunk_overlap >= effective_options.chunk_size) {
    return -1;
  }

  chunks = json_create_array();
  if (!chunks) {
    return -1;
  }
  text_len = strlen(text);
  while (offset < text_len) {
    size_t remaining = text_len - offset;
    size_t chunk_len = turbo_text_splitter_next_chunk_len(
        text + offset, remaining, effective_options.chunk_size);
    json_value_t *chunk = json_create_object();
    char *chunk_text = NULL;

    if (chunk_len <= effective_options.chunk_overlap &&
        remaining > effective_options.chunk_overlap) {
      chunk_len = effective_options.chunk_overlap + 1;
    }
    if (chunk_len == 0 || !chunk) {
      json_free(chunk); chunk = NULL;
      json_free(chunks); chunks = NULL;
      return -1;
    }
    chunk_text = turbo_text_splitter_copy_chunk(text + offset, chunk_len);
    if (!chunk_text) {
      json_free(chunk); chunk = NULL;
      json_free(chunks); chunks = NULL;
      return -1;
    }
    json_object_set_string(chunk, "text", chunk_text);
    json_object_set_number(chunk, "start", (double)offset);
    json_object_set_number(chunk, "end", (double)(offset + chunk_len));
    free(chunk_text);
    json_array_add(chunks, chunk);

    if (offset + chunk_len >= text_len) {
      break;
    }
    offset += chunk_len - effective_options.chunk_overlap;
  }

  *out_chunks_json = chunks;
  return 0;
}
