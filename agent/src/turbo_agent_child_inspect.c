#include "turbo_agent_child_inspect_internal.h"
#include "turbo_agent_state.h"

static int child_inspect_take(json_value_t *object, const char *key, json_value_t **value) {
  if (!*value) *value = json_create_null();
  if (turbo_runtime_json_object_set(object, key, *value) != TURBO_RUNTIME_JSON_OK) return -1;
  *value = NULL;
  return 0;
}

static int child_inspect_optional_string(json_value_t *object, const char *key, const char *text) {
  json_value_t *value = text && text[0] ? json_create_string(text) : json_create_null();
  int rc;
  if (!value) return -1;
  rc = child_inspect_take(object, key, &value);
  json_free(value);
  return rc;
}

void turbo_agent_child_inspect_parts_clear_internal(turbo_agent_child_inspect_parts_t *parts) {
  if (!parts) return;
  json_free(parts->run);
  json_free(parts->checkpoints);
  json_free(parts->latest_checkpoint);
  json_free(parts->checkpoint_context);
  json_free(parts->history_events);
  json_free(parts->trace_events);
  json_free(parts->thread_timeline);
  json_free(parts->branch_tree);
  *parts = (turbo_agent_child_inspect_parts_t){0};
}

int turbo_agent_child_inspect_build_internal(turbo_agent_child_inspect_parts_t *parts,
                                             json_value_t **out_inspect) {
  json_value_t *inspect = NULL;
  int rc = -1;
  if (out_inspect) *out_inspect = NULL;
  if (!parts || !out_inspect || !parts->run || !parts->checkpoints || !parts->history_events ||
      !parts->trace_events)
    goto cleanup;
  inspect = json_create_object();
  if (!inspect) goto cleanup;
  if (child_inspect_take(inspect, "run", &parts->run) != 0 ||
      child_inspect_take(inspect, "checkpoints", &parts->checkpoints) != 0 ||
      child_inspect_take(inspect, "latest_checkpoint", &parts->latest_checkpoint) != 0 ||
      child_inspect_take(inspect, "checkpoint_context", &parts->checkpoint_context) != 0 ||
      child_inspect_take(inspect, "history_events", &parts->history_events) != 0 ||
      child_inspect_take(inspect, "trace_events", &parts->trace_events) != 0 ||
      child_inspect_take(inspect, "thread_timeline", &parts->thread_timeline) != 0 ||
      child_inspect_take(inspect, "branch_tree", &parts->branch_tree) != 0)
    goto cleanup;
  *out_inspect = inspect;
  inspect = NULL;
  rc = 0;
cleanup:
  json_free(inspect);
  turbo_agent_child_inspect_parts_clear_internal(parts);
  return rc;
}

int turbo_agent_child_orchestration_build_internal(const json_value_t *output_item,
                                                   json_value_t **child_inspect,
                                                   json_value_t **out_inspect) {
  json_value_t *inspect;
  if (out_inspect) *out_inspect = NULL;
  if (!output_item || !child_inspect || !*child_inspect || !out_inspect) return -1;
  inspect = json_create_object();
  if (!inspect) return -1;
  if (child_inspect_optional_string(
          inspect, "parent_agent_run_id",
          turbo_agent_state_tool_result_parent_agent_run_id(output_item)) != 0 ||
      child_inspect_optional_string(
          inspect, "parent_tool_call_id",
          turbo_agent_state_tool_result_parent_tool_call_id(output_item)) != 0 ||
      child_inspect_optional_string(inspect, "parent_tool_name",
                                    turbo_agent_state_tool_result_parent_tool_name(output_item)) !=
          0 ||
      child_inspect_optional_string(
          inspect, "parent_graph_run_id",
          turbo_agent_state_tool_result_parent_graph_run_id(output_item)) != 0 ||
      child_inspect_optional_string(inspect, "call_frame_id",
                                    turbo_agent_state_tool_result_call_frame_id(output_item)) !=
          0 ||
      child_inspect_take(inspect, "child_inspect", child_inspect) != 0) {
    json_free(inspect);
    return -1;
  }
  *out_inspect = inspect;
  return 0;
}
