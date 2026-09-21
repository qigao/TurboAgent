#ifndef PRAKTOR_H
#define PRAKTOR_H

#include <stddef.h>
#include <stdint.h>

typedef enum praktor_result {
  PRAKTOR_RESULT_SUCCESS = 0,
  PRAKTOR_RESULT_EXECUTION_FAILED = 1,
  PRAKTOR_RESULT_INVALID_ARGUMENT = -1,
  PRAKTOR_RESULT_INVALID_JSON = -2,
  PRAKTOR_RESULT_OUT_OF_MEMORY = -3,
  PRAKTOR_RESULT_INTERNAL_ERROR = -4
} praktor_result;

#define PRAKTOR_ABI_MAJOR 2u
#define PRAKTOR_ABI_MINOR 0u
#define PRAKTOR_CAPABILITY_JSON_WORKFLOW (UINT64_C(1) << 0)

typedef enum praktor_error_phase {
  PRAKTOR_ERROR_PHASE_NONE = 0,
  PRAKTOR_ERROR_PHASE_REQUEST = 1,
  PRAKTOR_ERROR_PHASE_INPUT_JSON = 2,
  PRAKTOR_ERROR_PHASE_EXECUTION = 3,
  PRAKTOR_ERROR_PHASE_RESULT_JSON = 4
} praktor_error_phase;

typedef struct praktor_execute_request {
  uint32_t struct_size;
  const char *workflow_path;
  const char *input_json;
  size_t input_json_size;
} praktor_execute_request;

typedef struct praktor_owned_json {
  uint32_t struct_size;
  char *data;
  size_t size;
} praktor_owned_json;

typedef struct praktor_error {
  uint32_t struct_size;
  praktor_error_phase phase;
  char message[512];
} praktor_error;

#define PRAKTOR_EXECUTE_REQUEST_INIT {sizeof(praktor_execute_request), NULL, NULL, 0}
#define PRAKTOR_OWNED_JSON_INIT {sizeof(praktor_owned_json), NULL, 0}
#define PRAKTOR_ERROR_INIT {sizeof(praktor_error), PRAKTOR_ERROR_PHASE_NONE, {0}}

typedef int32_t (*praktor_execute_workflow_fn)(
    const praktor_execute_request *, praktor_owned_json *, praktor_error *);
typedef void (*praktor_release_json_fn)(praktor_owned_json *);

typedef struct praktor_api {
  uint32_t struct_size;
  uint32_t abi_major;
  uint32_t abi_minor;
  uint64_t capabilities;
  praktor_execute_workflow_fn execute_workflow;
  praktor_release_json_fn release_json;
} praktor_api;

const praktor_api *praktor_get_api(void);

#endif
