#ifndef TURBO_AGENT_SESSION_RESULT_INTERNAL_H
#define TURBO_AGENT_SESSION_RESULT_INTERNAL_H

#include "turbo_runtime_json.h"

/* Consume state on every outcome. Callers validate the result output slot and
 * initialize it to NULL; summary is optional and contains an owned tree or NULL.
 * On success, text is malloc-owned and JSON is independently parsed/owned.
 * On execution or conversion failure, discard/clear summary and propagate the
 * execution error (conversion failure is -1). No Session or Runtime rollback. */
int turbo_agent_session_finish_text_internal(int rc, json_value_t *state, char **out_text,
                                             json_value_t **summary);
int turbo_agent_session_finish_json_internal(int rc, json_value_t *state, json_value_t **out_json,
                                             json_value_t **summary);

#endif
