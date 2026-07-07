#include "../include/turbo_llm_sandbox_guest.h"

static char input_buffer[512];
static char output_buffer[512];
static char last_error[128];

static void set_error(const char *text) {
  int i = 0;
  if (!text) {
    last_error[0] = '\0';
    return;
  }
  while (text[i] != '\0' && i < (int)(sizeof(last_error) - 1)) {
    last_error[i] = text[i];
    i++;
  }
  last_error[i] = '\0';
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_COUNT)
int turbo_tool_count(void) {
  return 1;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_NAME)
int turbo_tool_name(int index) {
  static const char name[] = "echo_json";
  if (index != 0) {
    return 0;
  }
  return (int)(long)name;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_DESCRIPTION)
int turbo_tool_description(int index) {
  static const char description[] = "Echo JSON from guest wasm.";
  if (index != 0) {
    return 0;
  }
  return (int)(long)description;
}

TURBO_LLM_SANDBOX_GUEST_EXPORT(TURBO_LLM_SANDBOX_EXPORT_TOOL_PARAMETERS)
int turbo_tool_parameters(int index) {
  static const char parameters[] = "{\"type\":\"object\"}";
  if (index != 0) {
    return 0;
  }
  return (int)(long)parameters;
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
  int i;
  if (index != 0) {
    set_error("bad tool index");
    return -1;
  }
  if (input_len < 0 || input_len > (int)sizeof(output_buffer)) {
    set_error("input too large");
    return -2;
  }
  for (i = 0; i < input_len; ++i) {
    output_buffer[i] = input_buffer[i];
  }
  if (input_len < (int)sizeof(output_buffer)) {
    output_buffer[input_len] = '\0';
  }
  last_error[0] = '\0';
  return input_len;
}
