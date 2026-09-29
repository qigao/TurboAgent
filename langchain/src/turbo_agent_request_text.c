#define TURBO_AGENT_INTERNAL_STATE_IMPL_REMAP 1
#include "turbo_agent_state_memory_internal.h"
#include "turbo_agent_request_text_internal.h"

#include <stdlib.h>
#include <string.h>

CXX_C_API char *turbo_agent_request_join_text_blocks(const char *left, const char *separator,
                                                     const char *right) {
  size_t left_len = left ? strlen(left) : 0;
  size_t separator_len = separator ? strlen(separator) : 0;
  size_t right_len = right ? strlen(right) : 0;
  char *joined = (char *)malloc(left_len + separator_len + right_len + 1);

  if (!joined) {
    return NULL;
  }
  if (left_len > 0) {
    memcpy(joined, left, left_len);
  }
  if (separator_len > 0) {
    memcpy(joined + left_len, separator, separator_len);
  }
  if (right_len > 0) {
    memcpy(joined + left_len + separator_len, right, right_len);
  }
  joined[left_len + separator_len + right_len] = '\0';
  return joined;
}

CXX_C_API char *turbo_agent_build_effective_instructions(const turbo_agent_t *agent,
                                                         const json_value_t *state) {
  const char *base = agent ? agent->instructions : NULL;
  char *memory_text = turbo_agent_state_memory_context_text(state);
  size_t base_len = base ? strlen(base) : 0;
  size_t memory_len = memory_text ? strlen(memory_text) : 0;
  char *buffer;

  if (base_len == 0 && memory_len == 0) {
    free(memory_text);
    return NULL;
  }
  if (base_len == 0) {
    return memory_text;
  }
  if (memory_len == 0) {
    buffer = (char *)malloc(base_len + 1);
    if (!buffer) {
      return NULL;
    }
    memcpy(buffer, base, base_len + 1);
    return buffer;
  }

  buffer = (char *)malloc(base_len + 2 + memory_len + 1);
  if (!buffer) {
    free(memory_text);
    return NULL;
  }

  memcpy(buffer, base, base_len);
  memcpy(buffer + base_len, "\n\n", 2);
  memcpy(buffer + base_len + 2, memory_text, memory_len + 1);
  free(memory_text);
  return buffer;
}
