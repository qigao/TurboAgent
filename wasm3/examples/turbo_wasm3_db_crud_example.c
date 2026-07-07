#include "turbo_wasm3.h"

#include <stdio.h>

#ifndef WASM3_DB_CRUD_EXAMPLE_WASM_PATH
#error "WASM3_DB_CRUD_EXAMPLE_WASM_PATH must be defined by CMake"
#endif

static int fail_result(const char *step, M3Result result) {
  fprintf(stderr, "%s failed: %s\n", step, result ? result : "unknown error");
  return 1;
}

int main(void) {
  turbo_wasm3_vm_t *vm = NULL;
  IM3Module module = NULL;
  IM3Function run = NULL;
  int32_t result_value = 0;
  M3Result result = m3Err_none;
  int rc = 0;

  vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
  if (!vm) {
    fputs("failed to create turbo_wasm3 VM\n", stderr);
    return 1;
  }

  if (turbo_wasm3_vm_enable_sqlite_db(vm) != 0) {
    fputs("failed to enable sqlite db host\n", stderr);
    rc = 1;
    goto done;
  }
  if (turbo_wasm3_vm_enable_host(vm) != 0) {
    fputs("failed to enable TurboNet host imports\n", stderr);
    rc = 1;
    goto done;
  }

  result = turbo_wasm3_vm_load_module_file(vm, WASM3_DB_CRUD_EXAMPLE_WASM_PATH,
                                           "db_crud_demo", &module);
  if (result) {
    rc = fail_result("module load", result);
    goto done;
  }

  result = m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_db_crud_demo");
  if (result) {
    rc = fail_result("find run_db_crud_demo", result);
    goto done;
  }

  result = m3_CallV(run);
  if (result) {
    rc = fail_result("call run_db_crud_demo", result);
    goto done;
  }

  result = m3_GetResultsV(run, &result_value);
  if (result) {
    rc = fail_result("read run_db_crud_demo result", result);
    goto done;
  }

  printf("db crud demo result = %d\n", result_value);
  if (result_value != 11) {
    fprintf(stderr, "unexpected db crud demo result: %d\n", result_value);
    rc = 1;
  }

done:
  turbo_wasm3_vm_destroy(vm);
  return rc;
}
