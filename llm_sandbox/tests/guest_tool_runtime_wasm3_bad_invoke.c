#include "../include/turbo_llm_sandbox_guest.h"

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 2; }

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  static const char negative[] = "{\"name\":\"fail_negative\",\"description\":\"Fail.\","
                                 "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  static const char overflow[] = "{\"name\":\"overflow_output\",\"description\":\"Overflow.\","
                                 "\"parameters\":{\"type\":\"object\"},\"strict\":true}";
  if (index == 0) return turbo_wasm_app_stdout_write(negative, (uint32_t)(sizeof(negative) - 1));
  if (index == 1) return turbo_wasm_app_stdout_write(overflow, (uint32_t)(sizeof(overflow) - 1));
  return -1;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index) {
  static const char output[] = "0123456789abcdef";
  if (index == 0) return -7;
  if (index == 1) return turbo_wasm_app_stdout_write(output, (uint32_t)(sizeof(output) - 1));
  return -1;
}
