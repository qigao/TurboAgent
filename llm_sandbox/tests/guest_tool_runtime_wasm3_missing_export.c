#include "../include/turbo_llm_sandbox_guest.h"

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) { return 1; }

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIBE)
int turbo_tool_describe(int index) {
  (void)index;
  return -1;
}
