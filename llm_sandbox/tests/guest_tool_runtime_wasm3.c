#include "../include/turbo_llm_sandbox_guest.h"

static char input_buffer[512];

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 1; }

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char metadata[] =
      "{\"name\":\"echo_json\",\"description\":\"Echo JSON from guest wasm.\","
      "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  if (index != 0) return -1;
  return turbo_wasm_app_stdout_write(metadata, (uint32_t)(sizeof(metadata) - 1));
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  uint64_t input_size = 0;
  uint32_t read = 0;
  if (index != 0 || turbo_wasm_app_input_size(&input_size) != 0 ||
      input_size > sizeof(input_buffer) ||
      turbo_wasm_app_input_read(0, input_buffer, (uint32_t)sizeof(input_buffer), &read) != 0 ||
      read != input_size)
    return -1;
  return turbo_wasm_app_stdout_write(input_buffer, read);
}
