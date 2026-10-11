#ifndef TURBO_CODING_TOOLS_INTERNAL_H
#define TURBO_CODING_TOOLS_INTERNAL_H
#include "turbo_runtime_json.h"
#include "turbo_coding_tools.h"

/* Owned common result; NULL means allocation failure. */
json_value_t *turbo_coding_result(int ok, const char *status, const char *summary);
/* Borrowed context, checked synchronously; no token or context is retained. */
turbo_tool_status_t turbo_coding_check_control(const turbo_tool_execution_context_t *context);
/* Internal registration step; failed calls leave the registry unchanged. */
turbo_tool_status_t turbo_coding_search_add(turbo_tool_registry_t *registry,
                                           const turbo_coding_tools_config_t *config);
#endif
