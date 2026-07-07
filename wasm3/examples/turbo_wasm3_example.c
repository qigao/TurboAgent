#include "turbo_wasm3.h"

#include <stdio.h>

#ifndef WASM3_EXAMPLE_WASM_PATH
#error "WASM3_EXAMPLE_WASM_PATH must be defined by CMake"
#endif

#ifndef WASM3_EXAMPLE_PREOPEN_DIR
#error "WASM3_EXAMPLE_PREOPEN_DIR must be defined by CMake"
#endif

static int fail_result(turbo_wasm3_vm_t *vm, const char *step, M3Result result) {
  if (result == m3Err_trapExit && vm) {
    m3_wasi_context_t *wasi = turbo_wasm3_vm_get_wasi_context(vm);
    if (wasi && wasi->exit_code == 0) {
      return 0;
    }
    fprintf(stderr, "%s failed: wasi exit %d\n", step,
            wasi ? wasi->exit_code : -1);
    return wasi ? wasi->exit_code : 1;
  }

  fprintf(stderr, "%s failed: %s\n", step, result ? result : "unknown error");
  return 1;
}

int main(void) {
  const char *argv[] = {"turbo_wasm3_example", "cat", "/0.txt"};
  turbo_wasm3_vm_t *vm = NULL;
  IM3Module module = NULL;
  IM3Function fib = NULL;
  IM3Function start = NULL;
  uint32_t fib10 = 0;
  M3Result result = m3Err_none;
  int rc = 0;

  vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
  if (!vm) {
    fputs("failed to create turbo_wasm3 VM\n", stderr);
    return 1;
  }

  turbo_wasm3_vm_reset_preopens(vm);
  if (turbo_wasm3_vm_set_preopen(vm, 3, "/", WASM3_EXAMPLE_PREOPEN_DIR) != 0) {
    fputs("failed to configure WASI preopen\n", stderr);
    rc = 1;
    goto done;
  }
  if (turbo_wasm3_vm_set_wasi_args(vm, 3, argv) != 0) {
    fputs("failed to configure WASI argv\n", stderr);
    rc = 1;
    goto done;
  }

  result = turbo_wasm3_vm_load_module_file(vm, WASM3_EXAMPLE_WASM_PATH, "simple",
                                           &module);
  if (result) {
    rc = fail_result(vm, "module load", result);
    goto done;
  }

  result = m3_FindFunction(&fib, turbo_wasm3_vm_get_runtime(vm), "fib");
  if (result) {
    rc = fail_result(vm, "find fib", result);
    goto done;
  }
  result = m3_CallV(fib, 10);
  if (result) {
    rc = fail_result(vm, "call fib", result);
    goto done;
  }
  result = m3_GetResultsV(fib, &fib10);
  if (result) {
    rc = fail_result(vm, "read fib result", result);
    goto done;
  }

  printf("fib(10) = %u\n", fib10);

  result = m3_FindFunction(&start, turbo_wasm3_vm_get_runtime(vm), "_start");
  if (result) {
    rc = fail_result(vm, "find _start", result);
    goto done;
  }

  result = m3_CallArgv(start, 0, NULL);
  if (result) {
    rc = fail_result(vm, "call _start", result);
    goto done;
  }

done:
  turbo_wasm3_vm_destroy(vm);
  return rc;
}
