#include "../include/turbo_llm_sandbox_guest.h"

static char input_buffer[64];
static char output_buffer[64];

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) {
  return 1;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_NAME)
int turbo_tool_name(int index) {
  if (index != 0) {
    return 0;
  }
  return 0x7fffffff;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIPTION)
int turbo_tool_description(int index) {
  static const char description[] = "Bad metadata pointer.";
  return index == 0 ? (int)(long)description : 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_PARAMETERS)
int turbo_tool_parameters(int index) {
  static const char parameters[] = "{\"type\":\"object\"}";
  return index == 0 ? (int)(long)parameters : 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_STRICT)
int turbo_tool_strict(int index) {
  return index == 0 ? 1 : 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_INPUT_PTR)
int turbo_tool_input_ptr(void) {
  return (int)(long)input_buffer;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_INPUT_CAPACITY)
int turbo_tool_input_capacity(void) {
  return (int)sizeof(input_buffer);
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_OUTPUT_PTR)
int turbo_tool_output_ptr(void) {
  return (int)(long)output_buffer;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_OUTPUT_CAPACITY)
int turbo_tool_output_capacity(void) {
  return (int)sizeof(output_buffer);
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_INVOKE)
int turbo_tool_invoke(int index, int input_len) {
  (void)index;
  (void)input_len;
  return -1;
}
