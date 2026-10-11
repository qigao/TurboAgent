#ifndef TURBO_AGENT_SUPERVISOR_INSPECT_INTERNAL_H
#define TURBO_AGENT_SUPERVISOR_INSPECT_INTERNAL_H

#include "turbo_runtime_json.h"

/* Borrow state for this synchronous projection without mutation. On success,
 * return an independent owned report (json_free); on failure return -1 and
 * leave the provided output slot NULL. No query, persistence or cached state. */
int turbo_agent_supervisor_inspect_build_internal(const json_value_t *state,
                                                  json_value_t **out_inspect);

#endif
