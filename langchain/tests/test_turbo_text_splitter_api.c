#include "tinytest.h"
#include "turbo_text_splitter.h"

#include <string.h>

spec("turbo text splitter api") {
  it("should split text into bounded chunks with offsets") {
    turbo_text_splitter_options_t options = {0};
    json_value_t *chunks = NULL;
    const json_value_t *first;
    const json_value_t *second;

    options.chunk_size = 12;
    check_int_eq(turbo_text_splitter_split_text(
                     "alpha beta\ngamma delta\nomega", &options, &chunks),
                 0);
    check_size_eq(turbo_json_array_size(chunks), 3);
    first = turbo_json_array_get(chunks, 0);
    second = turbo_json_array_get(chunks, 1);
    check_str_eq(turbo_json_get_string(first, "text"), "alpha beta\n");
    check_int_eq(turbo_json_get_int(first, "start", -1), 0);
    check_int_eq(turbo_json_get_int(first, "end", -1), 11);
    check_str_eq(turbo_json_get_string(second, "text"), "gamma delta\n");

    turbo_free_json(&chunks);
  }

  it("should support overlapping chunks") {
    turbo_text_splitter_options_t options = {0};
    json_value_t *chunks = NULL;
    const json_value_t *first;
    const json_value_t *second;

    options.chunk_size = 5;
    options.chunk_overlap = 2;
    check_int_eq(turbo_text_splitter_split_text("abcdefghij", &options, &chunks),
                 0);
    check_size_eq(turbo_json_array_size(chunks), 3);
    first = turbo_json_array_get(chunks, 0);
    second = turbo_json_array_get(chunks, 1);
    check_str_eq(turbo_json_get_string(first, "text"), "abcde");
    check_str_eq(turbo_json_get_string(second, "text"), "defgh");
    check_int_eq(turbo_json_get_int(second, "start", -1), 3);

    turbo_free_json(&chunks);
  }

  it("should advance when newline split is shorter than overlap") {
    turbo_text_splitter_options_t options = {0};
    json_value_t *chunks = NULL;
    const json_value_t *last;
    const char *text = "abcdef\n0123456789XYZ";

    options.chunk_size = 10;
    options.chunk_overlap = 8;
    check_int_eq(turbo_text_splitter_split_text(text, &options, &chunks), 0);
    check_true(turbo_json_array_size(chunks) > 1);
    last = turbo_json_array_get(chunks, turbo_json_array_size(chunks) - 1);
    check_int_eq(turbo_json_get_int(last, "end", -1), (int)strlen(text));

    turbo_free_json(&chunks);
  }

  it("should reject overlap that cannot advance") {
    turbo_text_splitter_options_t options = {0};
    json_value_t *chunks = NULL;

    options.chunk_size = 5;
    options.chunk_overlap = 5;
    check_int_eq(turbo_text_splitter_split_text("abcdefghij", &options, &chunks),
                 -1);
    check_null(chunks);
  }
}
