#ifndef TURBO_PARSER_H
#define TURBO_PARSER_H

#include <json_parser.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef json_value_t turbo_json_doc_t;
typedef json_path_result_t turbo_json_path_result_t;
typedef json_path_program_t turbo_json_path_program_t;
typedef json_sax_parser_t turbo_json_sax_parser_t;
typedef json_path_stream_t turbo_json_path_stream_t;
typedef json_type_t turbo_json_type_t;
typedef json_sax_handler_t turbo_json_sax_handler_t;
typedef json_sax_handler_raw_t turbo_json_sax_handler_raw_t;
typedef json_path_stream_handler_t turbo_json_path_stream_handler_t;

#define TURBO_JSON_NULL JSON_NULL
#define TURBO_JSON_BOOL JSON_BOOL
#define TURBO_JSON_NUMBER JSON_NUMBER
#define TURBO_JSON_STRING JSON_STRING
#define TURBO_JSON_ARRAY JSON_ARRAY
#define TURBO_JSON_OBJECT JSON_OBJECT

static inline int turbo_parse_json(const uint8_t *data, size_t len,
                                   turbo_json_doc_t **out) {
  if (!out) return -1;
  *out = NULL;
  if (!data && len != 0) return -1;
  *out = json_parse((const char *)data, len);
  return *out ? 0 : -1;
}

static inline void turbo_free_json(turbo_json_doc_t **out) {
  if (!out || !*out) return;
  json_free(*out);
  *out = NULL;
}

#define turbo_parse_json_sax json_parse_sax
#define turbo_parse_json_sax_raw json_parse_sax_raw
#define turbo_json_sax_parser_create json_sax_parser_create
#define turbo_json_sax_parser_create_raw json_sax_parser_create_raw
#define turbo_json_sax_parser_feed json_sax_parser_feed
#define turbo_json_sax_parser_finish json_sax_parser_finish
#define turbo_json_sax_parser_error json_sax_parser_error
#define turbo_json_sax_parser_destroy json_sax_parser_destroy

#define turbo_json_type json_type
#define turbo_json_is_null json_is_null
#define turbo_json_bool json_bool
#define turbo_json_number json_number
#define turbo_json_number_text json_number_text
#define turbo_json_string json_string
#define turbo_json_string_len json_string_len
#define turbo_json_object_size json_object_size
#define turbo_json_object_key json_object_key
#define turbo_json_object_key_len json_object_key_len
#define turbo_json_object_value json_object_value
#define turbo_json_object_get json_object_get
#define turbo_json_array_size json_array_size
#define turbo_json_array_get json_array_get
#define turbo_json_get_int json_get_int
#define turbo_json_get_bool json_get_bool
#define turbo_json_get_double json_get_double
#define turbo_json_get_string json_get_string
#define turbo_json_serialize json_serialize
#define turbo_json_serialize_pretty json_serialize_pretty
#define turbo_json_serialize_pretty_crlf json_serialize_pretty_crlf
#define turbo_json_serialize_free json_serialize_free
#define turbo_json_clone json_clone
#define turbo_json_path_get json_path_get
#define turbo_json_path_query json_path_query
#define turbo_json_path_compile json_path_compile
#define turbo_json_path_get_compiled json_path_get_compiled
#define turbo_json_path_query_compiled json_path_query_compiled
#define turbo_json_path_program_free json_path_program_free
#define turbo_json_path_result_size json_path_result_size
#define turbo_json_path_result_get json_path_result_get
#define turbo_json_path_result_free json_path_result_free
#define turbo_json_path_error json_path_get_error
#define turbo_json_path_stream_create json_path_stream_create
#define turbo_json_path_stream_feed json_path_stream_feed
#define turbo_json_path_stream_finish json_path_stream_finish
#define turbo_json_path_stream_match_count json_path_stream_match_count
#define turbo_json_path_stream_error json_path_stream_error
#define turbo_json_path_stream_destroy json_path_stream_destroy
#define turbo_json_create_object json_create_object
#define turbo_json_create_array json_create_array
#define turbo_json_create_string json_create_string
#define turbo_json_create_string_n json_create_string_n
#define turbo_json_create_number json_create_number
#define turbo_json_create_int64 json_create_int64
#define turbo_json_create_uint64 json_create_uint64
#define turbo_json_create_bool json_create_bool
#define turbo_json_create_null json_create_null
#define turbo_json_object_add json_object_add
#define turbo_json_object_add_checked json_object_add_checked
#define turbo_json_array_add json_array_add
#define turbo_json_array_add_checked json_array_add_checked
#define turbo_json_object_set_string json_object_set_string
#define turbo_json_object_set_number json_object_set_number
#define turbo_json_object_set_bool json_object_set_bool
#define turbo_json_object_set_null json_object_set_null

#ifdef __cplusplus
}
#endif

#endif
