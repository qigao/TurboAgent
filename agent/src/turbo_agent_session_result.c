#include "turbo_agent_session.h"
#include "turbo_agent_session_result_internal.h"
#include "turbo_agent_state.h"

#include <stdlib.h>
#include <string.h>

/* Preserve the public malloc/free result ABI; Session's internal tstr ownership
 * must not escape through this caller-owned text result. */
CXX_C_API char *turbo_agent_session_result_text(const json_value_t *state) {
  const char *text;
  char *copy = NULL;
  size_t len;

  if (!state) {
    return NULL;
  }
  text = turbo_agent_state_final_answer_text(state);
  if (text && text[0] != '\0') {
    len = strlen(text);
    copy = (char *)malloc(len + 1);
    if (copy) {
      memcpy(copy, text, len + 1);
    }
  }
  return copy;
}

static void session_discard_summary(json_value_t **summary) {
  if (summary) {
    json_free(*summary);
    *summary = NULL;
  }
}

int turbo_agent_session_finish_text_internal(int rc, json_value_t *state, char **out_text,
                                             json_value_t **summary) {
  if (rc == 0) {
    *out_text = turbo_agent_session_result_text(state);
    if (!*out_text) rc = -1;
  }
  turbo_runtime_json_destroy(state);
  if (rc != 0) session_discard_summary(summary);
  return rc;
}

int turbo_agent_session_finish_json_internal(int rc, json_value_t *state, json_value_t **out_json,
                                             json_value_t **summary) {
  if (rc == 0 && (turbo_agent_state_parse_final_output_json(state, out_json) != 0 || !*out_json)) {
    json_free(*out_json);
    *out_json = NULL;
    rc = -1;
  }
  turbo_runtime_json_destroy(state);
  if (rc != 0) session_discard_summary(summary);
  return rc;
}
