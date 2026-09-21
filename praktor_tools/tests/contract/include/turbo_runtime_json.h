#ifndef TURBO_RUNTIME_JSON_H
#define TURBO_RUNTIME_JSON_H
#include "json_parser.h"
#include <stdint.h>
int turbo_runtime_json_parse(const uint8_t *data, size_t len,
                             json_value_t **out_value);
void turbo_runtime_json_destroy(json_value_t *value);
#endif
