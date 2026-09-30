#include "../include/turbo_wasm_tools_guest.h"

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 2; }

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char spin[] =
      "{\"name\":\"spin\",\"description\":\"Loop until interrupted or fuel is exhausted.\","
      "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  static const char trap[] =
      "{\"name\":\"trap\",\"description\":\"Trap immediately.\","
      "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  if (index == 0)
    return turbo_agent_tool_output_write(spin, (uint32_t)(sizeof(spin) - 1));
  if (index == 1)
    return turbo_agent_tool_output_write(trap, (uint32_t)(sizeof(trap) - 1));
  return -1;
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  if (index == 0) {
    volatile uint32_t counter = 0;
    for (;;) counter++;
  }
  if (index == 1) {
    __builtin_trap();
  }
  return -1;
}
