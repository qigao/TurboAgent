#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  JSON_NULL = 0,
  JSON_BOOL = 1,
  JSON_NUMBER = 2,
  JSON_STRING = 3,
  JSON_ARRAY = 4,
  JSON_OBJECT = 5
} json_type_t;

typedef struct json_value_s json_value_t;

json_value_t *json_parse(const char *data, size_t size);
void json_free(json_value_t *value);
json_type_t json_type(const json_value_t *value);
json_value_t *json_create_object(void);
void json_object_set_string(json_value_t *object, const char *key,
                            const char *value);
void json_object_set_number(json_value_t *object, const char *key,
                            double value);
const char *json_get_string(const json_value_t *object, const char *key);
double json_get_double(const json_value_t *object, const char *key,
                       double default_value);
char *json_serialize(const json_value_t *value, size_t *out_size);
void json_serialize_free(char *text);

#endif
