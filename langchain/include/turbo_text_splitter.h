#ifndef TURBO_TEXT_SPLITTER_H
#define TURBO_TEXT_SPLITTER_H

#include <turbo_agent_api.h>
#include <stddef.h>

#include <json_parser.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_text_splitter_options_s {
  size_t chunk_size;
  size_t chunk_overlap;
} turbo_text_splitter_options_t;

CXX_C_API int turbo_text_splitter_split_text(
    const char *text, const turbo_text_splitter_options_t *options,
    json_value_t **out_chunks_json);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_TEXT_SPLITTER_H */
