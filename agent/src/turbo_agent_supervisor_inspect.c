#include "turbo_agent_state.h"
#include "turbo_agent_supervisor_inspect_internal.h"

/* Children stay locally owned unless insertion succeeds. */
static int supervisor_inspect_take(json_value_t *object, const char *key, json_value_t **value) {
  if (turbo_runtime_json_object_set(object, key, *value) != TURBO_RUNTIME_JSON_OK) return -1;
  *value = NULL;
  return 0;
}

int turbo_agent_supervisor_inspect_build_internal(const json_value_t *state_json,
                                                  json_value_t **out_inspect_json) {
  json_value_t *control_json = NULL;
  json_value_t *workflow_json = NULL;
  json_value_t *inspect_json = NULL;
  json_value_t *inbox_json = NULL;
  json_value_t *history_json = NULL;
  json_value_t *latest_handoff_event_json = NULL;
  json_value_t *supervisor_json = NULL;
  const json_value_t *state_inbox = NULL;
  const json_value_t *state_history = NULL;
  const json_value_t *state_latest_handoff_event = NULL;
  const json_value_t *control_supervisor = NULL;
  int rc = -1;

  if (!out_inspect_json) return -1;
  *out_inspect_json = NULL;
  if (!state_json) return -1;
  control_json = turbo_agent_state_control_snapshot_json_value(state_json);
  workflow_json = turbo_agent_state_workflow_snapshot_json_value(state_json);
  if (!control_json || !workflow_json) {
    goto cleanup;
  }

  state_inbox = turbo_agent_state_supervisor_inbox(state_json);
  state_history = turbo_agent_state_supervisor_handoff_history(state_json);
  state_latest_handoff_event = turbo_agent_state_latest_handoff_event(state_json);
  control_supervisor = json_object_get(control_json, "supervisor");

  inbox_json = state_inbox && json_type(state_inbox) == JSON_ARRAY ? json_clone(state_inbox)
                                                                   : json_create_array();
  history_json = state_history && json_type(state_history) == JSON_ARRAY ? json_clone(state_history)
                                                                         : json_create_array();
  latest_handoff_event_json =
      state_latest_handoff_event && json_type(state_latest_handoff_event) == JSON_OBJECT
          ? json_clone(state_latest_handoff_event)
          : NULL;
  supervisor_json = control_supervisor && json_type(control_supervisor) == JSON_OBJECT
                        ? json_clone(control_supervisor)
                        : json_create_object();
  inspect_json = json_create_object();
  if (!inbox_json || !history_json || !supervisor_json || !inspect_json ||
      (state_latest_handoff_event && !latest_handoff_event_json)) {
    goto cleanup;
  }

  if (supervisor_inspect_take(inspect_json, "supervisor", &supervisor_json) != 0) goto cleanup;
  if (supervisor_inspect_take(inspect_json, "inbox", &inbox_json) != 0) goto cleanup;
  if (supervisor_inspect_take(inspect_json, "handoff_history", &history_json) != 0) goto cleanup;
  if (!latest_handoff_event_json) latest_handoff_event_json = json_create_null();
  if (supervisor_inspect_take(inspect_json, "latest_handoff_event", &latest_handoff_event_json) !=
      0)
    goto cleanup;
  if (supervisor_inspect_take(inspect_json, "control_snapshot", &control_json) != 0) goto cleanup;
  if (supervisor_inspect_take(inspect_json, "workflow_snapshot", &workflow_json) != 0) goto cleanup;

  *out_inspect_json = inspect_json;
  inspect_json = NULL;
  rc = 0;

cleanup:
  json_free(inspect_json);
  inspect_json = NULL;
  json_free(supervisor_json);
  supervisor_json = NULL;
  json_free(latest_handoff_event_json);
  latest_handoff_event_json = NULL;
  json_free(history_json);
  history_json = NULL;
  json_free(inbox_json);
  inbox_json = NULL;
  json_free(workflow_json);
  workflow_json = NULL;
  json_free(control_json);
  control_json = NULL;
  return rc;
}
