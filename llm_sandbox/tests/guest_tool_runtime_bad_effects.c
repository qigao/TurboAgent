#include "../include/turbo_wasm_tools_guest.h"

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 1; }

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char metadata[] =
      "{\"name\":\"bad_effects\",\"description\":\"Invalid effects.\","
      "\"parameters\":{\"type\":\"object\"},\"strict\":true,"
      "\"effects\":[\"pure\",\"write\"]}";
  if (index != 0) return -1;
  return turbo_agent_tool_output_write(metadata, (uint32_t)(sizeof(metadata) - 1));
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  (void)index;
  return -1;
}
