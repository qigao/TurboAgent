#ifndef TURBO_PARSER_H
#define TURBO_PARSER_H

#include <stddef.h>
#include <stdint.h>

typedef enum turbo_json_type_e {
  TURBO_JSON_NULL = 0,
  TURBO_JSON_OBJECT = 1,
  TURBO_JSON_ARRAY = 2,
  TURBO_JSON_STRING = 3,
  TURBO_JSON_NUMBER = 4,
  TURBO_JSON_BOOL = 5
} turbo_json_type_t;

typedef struct json_value_s json_value_t;

int turbo_parse_json(const uint8_t *data, size_t size, json_value_t **out_value);
void turbo_free_json(json_value_t **value);
turbo_json_type_t turbo_json_type(const json_value_t *value);
json_value_t *turbo_json_create_object(void);
void turbo_json_object_set_string(json_value_t *object, const char *key, const char *value);
void turbo_json_object_set_number(json_value_t *object, const char *key, double value);
const char *turbo_json_get_string(const json_value_t *object, const char *key);
double turbo_json_get_double(const json_value_t *object, const char *key, double default_value);
char *turbo_json_serialize(const json_value_t *value, size_t *out_size);
void turbo_json_serialize_free(char *text);

#endif
