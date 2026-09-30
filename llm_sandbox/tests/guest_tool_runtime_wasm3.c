#include "../include/turbo_wasm_tools_guest.h"

static char input_buffer[512];

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 1; }

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char metadata[] =
      "{\"name\":\"echo_json\",\"description\":\"Echo JSON from guest wasm.\","
      "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  if (index != 0) return -1;
  return turbo_agent_tool_output_write(metadata, (uint32_t)(sizeof(metadata) - 1));
}

TURBO_WASM_TOOLS_GUEST_EXPORT(TURBO_WASM_TOOLS_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  int32_t input_size = 0;
  int32_t read = 0;
  if (index != 0 || ((input_size = turbo_agent_tool_input_size()) < 0) ||
      (uint32_t)input_size > (uint32_t)sizeof(input_buffer) ||
      ((read = turbo_agent_tool_input_read(0u, input_buffer,
                                           (uint32_t)sizeof(input_buffer))) < 0) ||
      read != input_size)
    return -1;
  return turbo_agent_tool_output_write(input_buffer, read);
}
