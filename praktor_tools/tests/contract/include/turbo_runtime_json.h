#ifndef TURBO_RUNTIME_JSON_H
#define TURBO_RUNTIME_JSON_H
#include "json_parser.h"
#include <stdint.h>

typedef enum turbo_runtime_json_status_e {
  TURBO_RUNTIME_JSON_OK = 0,
  TURBO_RUNTIME_JSON_ERROR = -1,
  TURBO_RUNTIME_JSON_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_JSON_OUT_OF_MEMORY = -3,
  TURBO_RUNTIME_JSON_TYPE_MISMATCH = -4,
  TURBO_RUNTIME_JSON_NOT_FOUND = -5
} turbo_runtime_json_status_t;

int turbo_runtime_json_parse(const uint8_t *data, size_t len,
                             json_value_t **out_value);
void turbo_runtime_json_destroy(json_value_t *value);
#endif
