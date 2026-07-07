#include "../include/turbo_llm_sandbox_guest.h"

static char input_buffer[64];
static char output_buffer[16];

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) {
  return 2;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_NAME)
int turbo_tool_name(int index) {
  static const char negative_name[] = "fail_negative";
  static const char overflow_name[] = "overflow_output";
  if (index == 0) {
    return (int)(long)negative_name;
  }
  if (index == 1) {
    return (int)(long)overflow_name;
  }
  return 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIPTION)
int turbo_tool_description(int index) {
  static const char negative_description[] = "Return a negative output length.";
  static const char overflow_description[] = "Return an output length past capacity.";
  if (index == 0) {
    return (int)(long)negative_description;
  }
  if (index == 1) {
    return (int)(long)overflow_description;
  }
  return 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_PARAMETERS)
int turbo_tool_parameters(int index) {
  static const char parameters[] = "{\"type\":\"object\"}";
  return (index == 0 || index == 1) ? (int)(long)parameters : 0;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_STRICT)
int turbo_tool_strict(int index) {
  return index == 0 || index == 1;
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
  (void)input_len;
  if (index == 0) {
    return -7;
  }
  if (index == 1) {
    output_buffer[0] = '{';
    output_buffer[1] = '}';
    return (int)sizeof(output_buffer) + 1;
  }
  return -1;
}
