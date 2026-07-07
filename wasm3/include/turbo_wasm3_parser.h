#ifndef TURBO_WASM3_PARSER_H
#define TURBO_WASM3_PARSER_H

#include "turbo_wasm3_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Parser Registry - Unified handle-based parser for JSON/CSV/XML/INI/etc.
 * Gast sees only handles and ptr+len buffers.
 */
#define TURBO_WASM3_PARSER_TYPE_NONE 0
#define TURBO_WASM3_PARSER_TYPE_JSON 1
#define TURBO_WASM3_PARSER_TYPE_CSV 2
#define TURBO_WASM3_PARSER_TYPE_XML 3
#define TURBO_WASM3_PARSER_TYPE_INI 4
#define TURBO_WASM3_PARSER_TYPE_TOML 5
#define TURBO_WASM3_PARSER_TYPE_URI 6
#define TURBO_WASM3_PARSER_TYPE_DOTENV 7

CXX_C_API turbo_wasm3_vm_t *
turbo_wasm3_vm_get_parser_vm(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_parser_registry_set_limit(turbo_wasm3_parser_registry_t *registry,
                                      size_t max_docs);
/*
 * Parse functions - each returns a handle to parsed document.
 * import "TurboNet" "parser_json_parse" : i32 (ptr, len, out_handle)
 * import "TurboNet" "parser_csv_parse"  : i32 (ptr, len, out_handle)
 * import "TurboNet" "parser_xml_parse"  : i32 (ptr, len, out_handle)
 * import "TurboNet" "parser_ini_parse"  : i32 (ptr, len, out_handle)
 */
CXX_C_API int
turbo_wasm3_parser_parse_json(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_csv(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_xml(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_ini(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

/*
 * Free parsed document.
 * import "TurboNet" "parser_free" : i32 (handle)
 */
CXX_C_API int
turbo_wasm3_parser_free(turbo_wasm3_vm_t *vm, uint32_t handle);

/*
 * JSON accessors.
 * import "TurboNet" "json_get_type"     : i32 (handle, path_ptr, path_len, out_type)
 * import "TurboNet" "json_get_string"   : i32 (handle, path_ptr, path_len, buf, buf_len, out_written)
 * import "TurboNet" "json_get_int"      : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboNet" "json_get_bool"     : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboNet" "json_get_double"   : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboNet" "json_array_size"   : i32 (handle, path_ptr, path_len, out_size)
 * import "TurboNet" "json_object_keys"  : i32 (handle, path_ptr, path_len, buf, buf_len, out_written)
 */
CXX_C_API int
turbo_wasm3_json_get_type(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_type);

CXX_C_API int
turbo_wasm3_json_get_string(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len,
                            char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_json_get_int(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_json_get_bool(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_json_get_double(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, double *out_value);

CXX_C_API int
turbo_wasm3_json_array_size(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, uint32_t *out_size);

CXX_C_API int
turbo_wasm3_json_object_keys(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len,
                             char *buffer, size_t buffer_size, uint32_t *out_written);

/*
 * CSV accessors.
 * import "TurboNet" "csv_row_count"   : i32 (handle, out_count)
 * import "TurboNet" "csv_column_count": i32 (handle, out_count)
 * import "TurboNet" "csv_get_cell"    : i32 (handle, row, col, buf, buf_len, out_written)
 * import "TurboNet" "csv_find_column" : i32 (handle, name_ptr, name_len, out_col)
 */
CXX_C_API int
turbo_wasm3_csv_row_count(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t *out_count);

CXX_C_API int
turbo_wasm3_csv_column_count(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t *out_count);

CXX_C_API int
turbo_wasm3_csv_get_cell(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t row, uint32_t col,
                         char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_csv_find_column(turbo_wasm3_vm_t *vm, uint32_t handle, const char *name, size_t name_len, uint32_t *out_col);

/*
 * XML accessors.
 * import "TurboNet" "xml_root_name"   : i32 (handle, buf, buf_len, out_written)
 * import "TurboNet" "xml_get_text"    : i32 (handle, xpath_ptr, xpath_len, buf, buf_len, out_written)
 * import "TurboNet" "xml_count"       : i32 (handle, xpath_ptr, xpath_len, out_count)
 */
CXX_C_API int
turbo_wasm3_xml_root_name(turbo_wasm3_vm_t *vm, uint32_t handle, char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_xml_get_text(turbo_wasm3_vm_t *vm, uint32_t handle, const char *xpath, size_t xpath_len,
                         char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_xml_count(turbo_wasm3_vm_t *vm, uint32_t handle, const char *xpath, size_t xpath_len, uint32_t *out_count);

/*
 * INI accessors.
 * import "TurboNet" "ini_get_string" : i32 (handle, section_ptr, section_len, key_ptr, key_len, buf, buf_len, out_written)
 * import "TurboNet" "ini_get_int"    : i32 (handle, section_ptr, section_len, key_ptr, key_len, out_value)
 * import "TurboNet" "ini_get_bool"   : i32 (handle, section_ptr, section_len, key_ptr, key_len, out_value)
 * import "TurboNet" "ini_get_double" : i32 (handle, section_ptr, section_len, key_ptr, key_len, out_value)
 */
CXX_C_API int
turbo_wasm3_ini_get_string(turbo_wasm3_vm_t *vm, uint32_t handle, const char *section, size_t section_len,
                           const char *key, size_t key_len, char *buffer, size_t buffer_size,
                           uint32_t *out_written);

CXX_C_API int
turbo_wasm3_ini_get_int(turbo_wasm3_vm_t *vm, uint32_t handle, const char *section, size_t section_len,
                        const char *key, size_t key_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_ini_get_bool(turbo_wasm3_vm_t *vm, uint32_t handle, const char *section, size_t section_len,
                         const char *key, size_t key_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_ini_get_double(turbo_wasm3_vm_t *vm, uint32_t handle, const char *section, size_t section_len,
                           const char *key, size_t key_len, double *out_value);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_PARSER_H */
