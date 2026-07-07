#ifndef TURBO_EMBEDDING_H
#define TURBO_EMBEDDING_H

#include <platform.h>
#include <stddef.h>

#include "turbo_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_embedding_model_s turbo_embedding_model_t;

typedef int (*turbo_embedding_embed_text_fn)(void *user_data, const char *text,
                                             json_value_t **out_embedding_json);
typedef void (*turbo_embedding_user_data_free_fn)(void *user_data);

typedef struct turbo_embedding_model_config_s {
  turbo_embedding_embed_text_fn embed_text;
  void *user_data;
  turbo_embedding_user_data_free_fn user_data_free;
} turbo_embedding_model_config_t;

CXX_C_API turbo_embedding_model_t *
turbo_embedding_model_create(const turbo_embedding_model_config_t *config);

CXX_C_API void turbo_embedding_model_destroy(turbo_embedding_model_t *model);

CXX_C_API int turbo_embedding_model_embed_text(
    turbo_embedding_model_t *model, const char *text,
    json_value_t **out_embedding_json);

CXX_C_API turbo_embedding_model_t *
turbo_embedding_model_create_hashing(size_t dimensions);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_EMBEDDING_H */
