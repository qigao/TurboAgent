#ifndef TURBO_AGENT_TOOL_JOURNAL_INTERNAL_H
#define TURBO_AGENT_TOOL_JOURNAL_INTERNAL_H
#include "turbo_tool.h"

#define TURBO_AGENT_TOOL_JOURNAL_COLLECTION "agent_tool_journal"
#define TURBO_AGENT_TOOL_CLAIM_COLLECTION "agent_tool_claims"
#define TURBO_AGENT_TOOL_JOURNAL_SCHEMA_VERSION 1

static inline int turbo_agent_tool_journal_status_valid(int64_t status) {
  switch (status) {
  case TURBO_TOOL_OK:
  case TURBO_TOOL_ERROR:
  case TURBO_TOOL_INVALID_ARGUMENT:
  case TURBO_TOOL_DUPLICATE:
  case TURBO_TOOL_NOT_FOUND:
  case TURBO_TOOL_OUT_OF_MEMORY:
  case TURBO_TOOL_CANCELLED:
  case TURBO_TOOL_DEADLINE_EXCEEDED:
  case TURBO_TOOL_OUTPUT_LIMIT:
  case TURBO_TOOL_UNKNOWN_SIDE_EFFECT:
  case TURBO_TOOL_BACKPRESSURE:
  case TURBO_TOOL_FUEL_EXHAUSTED:
  case TURBO_TOOL_TRAPPED:
    return 1;
  default:
    return 0;
  }
}

#endif
