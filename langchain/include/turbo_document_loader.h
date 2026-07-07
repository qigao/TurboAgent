#ifndef TURBO_DOCUMENT_LOADER_H
#define TURBO_DOCUMENT_LOADER_H

#include <platform.h>

#include "turbo_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

CXX_C_API int turbo_document_loader_load_text_file(
    const char *path, const char *kind, json_value_t **out_document_json);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_DOCUMENT_LOADER_H */
