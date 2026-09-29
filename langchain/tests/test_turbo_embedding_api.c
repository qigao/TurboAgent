#include "tinytest.h"
#include <json_parser.h>
#include "turbo_embedding.h"

typedef struct embedding_test_state_s {
  int call_count;
  int freed;
} embedding_test_state_t;

static int embedding_test_embed(void *user_data, const char *text,
                                json_value_t **out_embedding_json) {
  embedding_test_state_t *state = (embedding_test_state_t *)user_data;
  json_value_t *embedding;

  check_not_null(state);
  check_equal(text, "custom text");
  check_not_null(out_embedding_json);
  state->call_count++;

  embedding = json_create_array();
  check_not_null(embedding);
  json_array_add(embedding, json_create_number(7.0));
  json_array_add(embedding, json_create_number(3.0));
  *out_embedding_json = embedding;
  return 0;
}

static void embedding_test_free(void *user_data) {
  embedding_test_state_t *state = (embedding_test_state_t *)user_data;

  check_not_null(state);
  state->freed = 1;
}

static double embedding_test_sum(const json_value_t *embedding) {
  double sum = 0.0;
  size_t i;

  for (i = 0; i < json_array_size(embedding); ++i) {
    const json_value_t *value = json_array_get(embedding, i);

    check_true(json_type(value) == JSON_NUMBER);
    sum += json_number(value);
  }
  return sum;
}

static void embedding_test_check_same_values(const json_value_t *left,
                                             const json_value_t *right) {
  size_t count = json_array_size(left);
  size_t i;

  check_equal(json_array_size(right), count);
  for (i = 0; i < count; ++i) {
    check_equal((int)json_number(json_array_get(left, i)),
                 (int)json_number(json_array_get(right, i)));
  }
}

spec("turbo embedding api") {
  it("should wrap a custom embedding callback") {
    embedding_test_state_t state = {0};
    turbo_embedding_model_config_t config = {0};
    turbo_embedding_model_t *model;
    json_value_t *embedding = NULL;

    config.embed_text = embedding_test_embed;
    config.user_data = &state;
    config.user_data_free = embedding_test_free;

    model = turbo_embedding_model_create(&config);
    check_not_null(model);
    check_equal(turbo_embedding_model_embed_text(model, "custom text",
                                                  &embedding),
                 0);
    check_equal(json_array_size(embedding), 2);
    check_equal((int)json_number(json_array_get(embedding, 0)), 7);
    check_equal(state.call_count, 1);

    json_free(embedding); embedding = NULL;
    turbo_embedding_model_destroy(model);
    check_equal(state.freed, 1);
  }

  it("should create deterministic local hashing embeddings") {
    turbo_embedding_model_t *model = turbo_embedding_model_create_hashing(8);
    json_value_t *embedding = NULL;
    json_value_t *case_a = NULL;
    json_value_t *case_b = NULL;

    check_not_null(model);
    check_equal(turbo_embedding_model_embed_text(model, "Alpha beta alpha",
                                                  &embedding),
                 0);
    check_equal(json_array_size(embedding), 8);
    check_equal((int)embedding_test_sum(embedding), 3);

    check_equal(
        turbo_embedding_model_embed_text(model, "Alpha beta", &case_a), 0);
    check_equal(
        turbo_embedding_model_embed_text(model, "alpha beta", &case_b), 0);
    embedding_test_check_same_values(case_a, case_b);

    json_free(case_b); case_b = NULL;
    json_free(case_a); case_a = NULL;
    json_free(embedding); embedding = NULL;
    turbo_embedding_model_destroy(model);
  }

  it("should reject invalid hashing dimensions") {
    check_null(turbo_embedding_model_create_hashing(0));
  }
}
