#ifndef TURBO_RUNTIME_BINARY_MIR_H
#define TURBO_RUNTIME_BINARY_MIR_H


#include <platform.h>

#include "turbo_runtime_binary_schema.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_runtime_binary_mir_plan_s turbo_runtime_binary_mir_plan_t;
typedef struct turbo_runtime_binary_mir_compiler_s
    turbo_runtime_binary_mir_compiler_t;

typedef turbo_runtime_data_bind_value_t *(*turbo_runtime_binary_mir_parser_fn)(
    const uint8_t *data, size_t size, void *builder_user_data);

typedef enum {
  TURBO_RUNTIME_BINARY_MIR_OK = 0,
  TURBO_RUNTIME_BINARY_MIR_ERROR = -1,
  TURBO_RUNTIME_BINARY_MIR_INVALID_ARGUMENT = -2,
  TURBO_RUNTIME_BINARY_MIR_OUT_OF_MEMORY = -3,
  TURBO_RUNTIME_BINARY_MIR_UNSUPPORTED = -4
} turbo_runtime_binary_mir_status_t;

typedef enum {
  TURBO_RUNTIME_BINARY_MIR_ABI_VOID = 0,
  TURBO_RUNTIME_BINARY_MIR_ABI_PTR = 1,
  TURBO_RUNTIME_BINARY_MIR_ABI_I32 = 2,
  TURBO_RUNTIME_BINARY_MIR_ABI_I64 = 3,
  TURBO_RUNTIME_BINARY_MIR_ABI_DOUBLE = 4
} turbo_runtime_binary_mir_abi_type_t;

typedef enum {
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BOOL = 0,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_INT64 = 1,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_DOUBLE = 2,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_STRING = 3,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_BYTES = 4,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_OBJECT = 5,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_CREATE_ARRAY = 6,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_OBJECT_SET = 7,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_ARRAY_APPEND = 8,
  TURBO_RUNTIME_BINARY_MIR_SYMBOL_DESTROY_VALUE = 9
} turbo_runtime_binary_mir_symbol_id_t;

typedef struct turbo_runtime_binary_mir_symbol_descriptor_s {
  turbo_runtime_binary_mir_symbol_id_t id;
  const char *name;
  turbo_runtime_binary_mir_abi_type_t result_type;
  size_t argument_count;
  turbo_runtime_binary_mir_abi_type_t argument_types[4];
} turbo_runtime_binary_mir_symbol_descriptor_t;

CXX_C_API turbo_runtime_binary_mir_plan_t *
turbo_runtime_binary_mir_plan_create(
    const turbo_runtime_binary_schema_t *schema);
CXX_C_API void turbo_runtime_binary_mir_plan_destroy(
    turbo_runtime_binary_mir_plan_t *plan);
CXX_C_API size_t turbo_runtime_binary_mir_plan_symbol_count(
    const turbo_runtime_binary_mir_plan_t *plan);
CXX_C_API turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_plan_get_symbol(
    const turbo_runtime_binary_mir_plan_t *plan, size_t index,
    turbo_runtime_binary_mir_symbol_descriptor_t *out_symbol);

CXX_C_API turbo_runtime_binary_mir_compiler_t *
turbo_runtime_binary_mir_compiler_create(
    const turbo_runtime_binary_schema_t *schema);
CXX_C_API void turbo_runtime_binary_mir_compiler_destroy(
    turbo_runtime_binary_mir_compiler_t *compiler);
CXX_C_API const char *turbo_runtime_binary_mir_compiler_entry_name(
    const turbo_runtime_binary_mir_compiler_t *compiler);
CXX_C_API const turbo_runtime_binary_mir_plan_t *
turbo_runtime_binary_mir_compiler_plan(
    const turbo_runtime_binary_mir_compiler_t *compiler);
/**
 * @brief Build one executable parser entry for the compiler schema.
 */
CXX_C_API turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_compiler_build(
    turbo_runtime_binary_mir_compiler_t *compiler,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size);
/**
 * @brief Backward-compatible alias for `turbo_runtime_binary_mir_compiler_build`.
 */
CXX_C_API turbo_runtime_binary_mir_status_t
turbo_runtime_binary_mir_compiler_build_stub(
    turbo_runtime_binary_mir_compiler_t *compiler,
    const turbo_runtime_data_bind_value_api_t *api, char *error_buffer,
    size_t error_buffer_size);
CXX_C_API turbo_runtime_binary_mir_parser_fn
turbo_runtime_binary_mir_compiler_parser(
    const turbo_runtime_binary_mir_compiler_t *compiler);

#ifdef __cplusplus
}
#endif

#endif
