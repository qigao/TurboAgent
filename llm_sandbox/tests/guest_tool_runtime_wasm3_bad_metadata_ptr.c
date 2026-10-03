#include "../include/turbo_wasm_tools_guest.h"

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 1; }

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char invalid_metadata[] = "not-json";
  if (index != 0) return -1;
  return turbo_agent_tool_output_write(invalid_metadata, (uint32_t)(sizeof(invalid_metadata) - 1));
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  (void)index;
  return -1;
}
