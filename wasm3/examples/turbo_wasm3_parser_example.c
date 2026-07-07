#include "turbo_wasm3.h"

#include <stdio.h>

#ifndef WASM3_PARSER_EXAMPLE_WASM_PATH
#error "WASM3_PARSER_EXAMPLE_WASM_PATH must be defined by CMake"
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
  turbo_wasm3_vm_t *vm = NULL;
  IM3Module module = NULL;
  IM3Function json_demo = NULL;
  IM3Function csv_demo = NULL;
  IM3Function xml_demo = NULL;
  M3Result result = m3Err_none;
  int32_t guest_result = 0;
  int rc = 0;

  vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
  if (!vm) {
    fputs("failed to create turbo_wasm3 VM\n", stderr);
    return 1;
  }

  /* Enable TurboNet host linker - this provides parser functions */
  rc = turbo_wasm3_vm_enable_host(vm);
  if (rc != 0) {
    fprintf(stderr, "failed to enable TurboNet host: %d\n", rc);
    rc = 1;
    goto done;
  }

  result = turbo_wasm3_vm_load_module_file(vm, WASM3_PARSER_EXAMPLE_WASM_PATH,
                                           "parser_demo", &module);
  if (result) {
    rc = fail_result(vm, "module load", result);
    goto done;
  }

  /* Run JSON demo */
  result = m3_FindFunction(&json_demo, turbo_wasm3_vm_get_runtime(vm), "run_json_demo");
  if (result) {
    rc = fail_result(vm, "find run_json_demo", result);
    goto done;
  }
  result = m3_CallV(json_demo);
  if (result) {
    rc = fail_result(vm, "call run_json_demo", result);
    goto done;
  }
  result = m3_GetResultsV(json_demo, &guest_result);
  if (result) {
    rc = fail_result(vm, "result run_json_demo", result);
    goto done;
  }
  if (guest_result != 7) {
    fprintf(stderr, "run_json_demo failed: guest returned %d\n", guest_result);
    rc = 1;
    goto done;
  }
  puts("JSON demo passed");

  /* Run CSV demo */
  result = m3_FindFunction(&csv_demo, turbo_wasm3_vm_get_runtime(vm), "run_csv_demo");
  if (result) {
    rc = fail_result(vm, "find run_csv_demo", result);
    goto done;
  }
  result = m3_CallV(csv_demo);
  if (result) {
    rc = fail_result(vm, "call run_csv_demo", result);
    goto done;
  }
  result = m3_GetResultsV(csv_demo, &guest_result);
  if (result) {
    rc = fail_result(vm, "result run_csv_demo", result);
    goto done;
  }
  if (guest_result != 7) {
    fprintf(stderr, "run_csv_demo failed: guest returned %d\n", guest_result);
    rc = 1;
    goto done;
  }
  puts("CSV demo passed");

  /* Run XML demo */
  result = m3_FindFunction(&xml_demo, turbo_wasm3_vm_get_runtime(vm), "run_xml_demo");
  if (result) {
    rc = fail_result(vm, "find run_xml_demo", result);
    goto done;
  }
  result = m3_CallV(xml_demo);
  if (result) {
    rc = fail_result(vm, "call run_xml_demo", result);
    goto done;
  }
  result = m3_GetResultsV(xml_demo, &guest_result);
  if (result) {
    rc = fail_result(vm, "result run_xml_demo", result);
    goto done;
  }
  if (guest_result != 7) {
    fprintf(stderr, "run_xml_demo failed: guest returned %d\n", guest_result);
    rc = 1;
    goto done;
  }
  puts("XML demo passed");

done:
  turbo_wasm3_vm_destroy(vm);
  return rc;
}
