#include "turbo_embedding.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct turbo_embedding_model_s {
  turbo_embedding_embed_text_fn embed_text;
  void *user_data;
  turbo_embedding_user_data_free_fn user_data_free;
};

typedef struct turbo_embedding_hashing_config_s {
  size_t dimensions;
} turbo_embedding_hashing_config_t;

static uint64_t turbo_embedding_hash_token(const char *text, size_t len) {
  uint64_t hash = 1469598103934665603ull;
  size_t i;

  for (i = 0; i < len; ++i) {
    unsigned char c = (unsigned char)text[i];
    if (c >= 'A' && c <= 'Z') {
      c = (unsigned char)(c - 'A' + 'a');
    }
    hash ^= (uint64_t)c;
    hash *= 1099511628211ull;
  }
  return hash;
}

static int turbo_embedding_token_char(char ch) {
  unsigned char c = (unsigned char)ch;

  return isalnum(c) || ch == '_';
}

turbo_embedding_model_t *
turbo_embedding_model_create(const turbo_embedding_model_config_t *config) {
  turbo_embedding_model_t *model;

  if (!config || !config->embed_text) {
    return NULL;
  }
  model = (turbo_embedding_model_t *)calloc(1, sizeof(*model));
  if (!model) {
    return NULL;
  }
  model->embed_text = config->embed_text;
  model->user_data = config->user_data;
  model->user_data_free = config->user_data_free;
  return model;
}

void turbo_embedding_model_destroy(turbo_embedding_model_t *model) {
  if (!model) {
    return;
  }
  if (model->user_data_free) {
    model->user_data_free(model->user_data);
  }
  free(model);
}

int turbo_embedding_model_embed_text(
    turbo_embedding_model_t *model, const char *text,
    json_value_t **out_embedding_json) {
  if (!model || !model->embed_text || !text || !out_embedding_json) {
    return -1;
  }
  return model->embed_text(model->user_data, text, out_embedding_json);
}

static int turbo_embedding_hashing_embed_text(void *user_data, const char *text,
                                              json_value_t **out_embedding_json) {
  turbo_embedding_hashing_config_t *config =
      (turbo_embedding_hashing_config_t *)user_data;
  json_value_t *embedding;
  double *values;
  size_t text_len;
  size_t i = 0;

  if (!config || config->dimensions == 0 || !text || !out_embedding_json) {
    return -1;
  }
  *out_embedding_json = NULL;
  values = (double *)calloc(config->dimensions, sizeof(*values));
  if (!values) {
    return -1;
  }
  text_len = strlen(text);
  while (i < text_len) {
    size_t start;
    size_t token_len;
    uint64_t hash;

    while (i < text_len && !turbo_embedding_token_char(text[i])) {
      i++;
    }
    start = i;
    while (i < text_len && turbo_embedding_token_char(text[i])) {
      i++;
    }
    token_len = i - start;
    if (token_len == 0) {
      continue;
    }
    hash = turbo_embedding_hash_token(text + start, token_len);
    values[(size_t)(hash % config->dimensions)] += 1.0;
  }

  embedding = turbo_json_create_array();
  if (!embedding) {
    free(values);
    return -1;
  }
  for (i = 0; i < config->dimensions; ++i) {
    json_value_t *value = turbo_json_create_number(values[i]);

    if (!value) {
      turbo_free_json(&embedding);
      free(values);
      return -1;
    }
    turbo_json_array_add(embedding, value);
  }
  free(values);
  *out_embedding_json = embedding;
  return 0;
}

turbo_embedding_model_t *
turbo_embedding_model_create_hashing(size_t dimensions) {
  turbo_embedding_hashing_config_t *hashing_config;
  turbo_embedding_model_config_t config = {0};

  if (dimensions == 0) {
    return NULL;
  }
  hashing_config =
      (turbo_embedding_hashing_config_t *)calloc(1, sizeof(*hashing_config));
  if (!hashing_config) {
    return NULL;
  }
  hashing_config->dimensions = dimensions;
  config.embed_text = turbo_embedding_hashing_embed_text;
  config.user_data = hashing_config;
  config.user_data_free = free;
  {
    turbo_embedding_model_t *model = turbo_embedding_model_create(&config);

    if (!model) {
      free(hashing_config);
    }
    return model;
  }
}
