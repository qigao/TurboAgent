#include "../include/turbo_wasm_tools_guest.h"

TURBO_WASM_TOOLS_GUEST_IMPORT("wasi_snapshot_preview1", "random_get")
extern int32_t forbidden_random_get(int32_t buffer, int32_t length);

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) {
  /* Keep the forbidden import live in the module. Linking must fail before use. */
  return forbidden_random_get(0, 0);
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  (void)index;
  return -1;
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  (void)index;
  return -1;
}
