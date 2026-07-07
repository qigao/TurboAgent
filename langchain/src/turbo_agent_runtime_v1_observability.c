#include "turbo_agent_runtime.h"

#include "turbo_agent_runtime_internal.h"
#include "turbo_agent_runtime_v1_internal.h"
#include "turbo_agent_state.h"
#include "turbo_agent_util_internal.h"

#include <stdlib.h>
#include <string.h>

static int turbo_agent_runtime_observability_summary_thread_id_compare_asc(
    const void *left, const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const json_value_t *a_thread_json = turbo_json_object_get(a, "thread");
  const json_value_t *b_thread_json = turbo_json_object_get(b, "thread");
  const char *a_thread_id =
      (a_thread_json && turbo_json_type(a_thread_json) == TURBO_JSON_OBJECT)
          ? turbo_json_get_string(a_thread_json, "id")
          : NULL;
  const char *b_thread_id =
      (b_thread_json && turbo_json_type(b_thread_json) == TURBO_JSON_OBJECT)
          ? turbo_json_get_string(b_thread_json, "id")
          : NULL;

  return turbo_agent_runtime_record_string_compare(a_thread_id, b_thread_id);
}

static int turbo_agent_runtime_observability_summary_thread_id_compare_desc(
    const void *left, const void *right) {
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(right, left);
}

static int turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(
    const void *left, const void *right) {
  const json_value_t *a = *(const json_value_t *const *)left;
  const json_value_t *b = *(const json_value_t *const *)right;
  const char *a_updated_at = turbo_json_get_string(a, "latest_run_updated_at");
  const char *b_updated_at = turbo_json_get_string(b, "latest_run_updated_at");
  int rc = turbo_agent_runtime_record_string_compare(a_updated_at, b_updated_at);

  if (rc != 0) {
    return rc;
  }
  return turbo_agent_runtime_observability_summary_thread_id_compare_asc(left, right);
}

static int turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc(
    const void *left, const void *right) {
  return turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc(right, left);
}


static void turbo_agent_runtime_observability_counts_from_runs(
    const json_value_t *runs_json, size_t *out_runs, size_t *out_interrupted_runs,
    size_t *out_completed_runs) {
  size_t i;
  size_t count = 0;
  size_t interrupted = 0;
  size_t completed = 0;

  if (out_runs) {
    *out_runs = 0;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = 0;
  }
  if (out_completed_runs) {
    *out_completed_runs = 0;
  }
  if (!runs_json || turbo_json_type(runs_json) != TURBO_JSON_ARRAY) {
    return;
  }

  count = turbo_json_array_size(runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = turbo_json_array_get(runs_json, i);
    const char *status;

    if (!run_record || turbo_json_type(run_record) != TURBO_JSON_OBJECT) {
      continue;
    }
    status = turbo_json_get_string(run_record, "status");
    if (status && strcmp(status, "interrupted") == 0) {
      interrupted += 1;
    } else if (status && strcmp(status, "completed") == 0) {
      completed += 1;
    }
  }

  if (out_runs) {
    *out_runs = count;
  }
  if (out_interrupted_runs) {
    *out_interrupted_runs = interrupted;
  }
  if (out_completed_runs) {
    *out_completed_runs = completed;
  }
}

static json_value_t *turbo_agent_runtime_build_observability_counts_json(
    const json_value_t *timeline_json, const json_value_t *lineage_json,
    const json_value_t *branch_tree_json, const json_value_t *history_events_json,
    const json_value_t *trace_events_json) {
  const json_value_t *runs_json = NULL;
  const json_value_t *current_run_checkpoints_json = NULL;
  const json_value_t *branches_json = NULL;
  const json_value_t *edges_json = NULL;
  size_t runs = 0;
  size_t interrupted_runs = 0;
  size_t completed_runs = 0;
  json_value_t *counts_json = turbo_json_create_object();

  if (!counts_json) {
    return NULL;
  }

  if (timeline_json && turbo_json_type(timeline_json) == TURBO_JSON_OBJECT) {
    runs_json = turbo_json_object_get(timeline_json, "runs");
    current_run_checkpoints_json =
        turbo_json_object_get(timeline_json, "current_run_checkpoints");
  }
  if (branch_tree_json && turbo_json_type(branch_tree_json) == TURBO_JSON_OBJECT) {
    branches_json = turbo_json_object_get(branch_tree_json, "branches");
    edges_json = turbo_json_object_get(branch_tree_json, "edges");
  } else if (lineage_json && turbo_json_type(lineage_json) == TURBO_JSON_OBJECT) {
    branches_json = turbo_json_object_get(lineage_json, "branches");
  }

  turbo_agent_runtime_observability_counts_from_runs(runs_json, &runs, &interrupted_runs,
                                                     &completed_runs);
  turbo_json_object_set_number(counts_json, "runs", (double)runs);
  turbo_json_object_set_number(counts_json, "interrupted_runs", (double)interrupted_runs);
  turbo_json_object_set_number(counts_json, "completed_runs", (double)completed_runs);
  turbo_json_object_set_number(
      counts_json, "current_run_checkpoints",
      (double)((current_run_checkpoints_json &&
                turbo_json_type(current_run_checkpoints_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(current_run_checkpoints_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "branches",
      (double)((branches_json && turbo_json_type(branches_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(branches_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "edges",
      (double)((edges_json && turbo_json_type(edges_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(edges_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "history_events",
      (double)((history_events_json && turbo_json_type(history_events_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(history_events_json)
                   : 0));
  turbo_json_object_set_number(
      counts_json, "trace_events",
      (double)((trace_events_json && turbo_json_type(trace_events_json) == TURBO_JSON_ARRAY)
                   ? turbo_json_array_size(trace_events_json)
                   : 0));
  return counts_json;
}

static const char *turbo_agent_runtime_observability_string_or_null(const json_value_t *object_json,
                                                                    const char *key) {
  const char *value;

  if (!object_json || turbo_json_type(object_json) != TURBO_JSON_OBJECT || !key) {
    return NULL;
  }
  value = turbo_json_get_string(object_json, key);
  return (value && value[0] != '\0') ? value : NULL;
}

static const char *turbo_agent_runtime_observability_current_status(
    const json_value_t *timeline_json, const json_value_t *pending_run_json,
    const json_value_t *latest_run_json) {
  const json_value_t *resolved_current_run_json = NULL;
  const char *status = NULL;

  if (timeline_json && turbo_json_type(timeline_json) == TURBO_JSON_OBJECT) {
    resolved_current_run_json = turbo_json_object_get(timeline_json, "resolved_current_run");
  }
  status = turbo_agent_runtime_observability_string_or_null(resolved_current_run_json, "status");
  if (status) {
    return status;
  }
  status = turbo_agent_runtime_observability_string_or_null(pending_run_json, "status");
  if (status) {
    return status;
  }
  return turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
}

static int turbo_agent_runtime_observability_summary_copy_field(
    json_value_t *target_json, const json_value_t *source_json, const char *key) {
  const json_value_t *value_json;
  json_value_t *clone_json;

  if (!target_json || !source_json || !key) {
    return -1;
  }
  value_json = turbo_json_object_get(source_json, key);
  if (!value_json) {
    turbo_json_object_set_null(target_json, key);
    return 0;
  }
  clone_json = turbo_json_clone(value_json);
  if (!clone_json) {
    return -1;
  }
  turbo_json_object_add(target_json, key, clone_json);
  return 0;
}

static int turbo_agent_runtime_build_observability_index_summary(
    const json_value_t *index_json, json_value_t **out_summary_json) {
  static const char *const summary_keys[] = {"thread",
                                             "latest_run",
                                             "pending_run",
                                             "current_status",
                                             "current_interrupt_reason",
                                             "current_pending_action",
                                             "current_checkpoint_summary",
                                             "latest_run_status",
                                             "latest_run_updated_at",
                                             "pending_run_id",
                                             "pending_checkpoint_id",
                                             "has_failure",
                                             "has_model_error",
                                             "has_guardrail_rejection",
                                             "replan_requested",
                                             "current_failure_reason",
                                             "current_review_note",
                                             "has_pending_review",
                                             "has_handoff",
                                             "active_agent",
                                             "counts"};
  json_value_t *summary_json = NULL;
  size_t i;

  if (!index_json || turbo_json_type(index_json) != TURBO_JSON_OBJECT || !out_summary_json) {
    return -1;
  }
  *out_summary_json = NULL;
  summary_json = turbo_json_create_object();
  if (!summary_json) {
    return -1;
  }
  for (i = 0; i < sizeof(summary_keys) / sizeof(summary_keys[0]); ++i) {
    if (turbo_agent_runtime_observability_summary_copy_field(summary_json, index_json,
                                                             summary_keys[i]) != 0) {
      turbo_free_json(&summary_json);
      return -1;
    }
  }
  *out_summary_json = summary_json;
  return 0;
}

static int turbo_agent_runtime_observability_summary_matches_filters(
    const json_value_t *summary_json, const json_value_t *filters_json, bool *out_matches) {
  const json_value_t *value_json;
  const char *summary_text;
  const char *summary_thread_id;
  bool expected_bool;

  if (!summary_json || turbo_json_type(summary_json) != TURBO_JSON_OBJECT || !out_matches) {
    return -1;
  }
  *out_matches = true;
  if (!filters_json || turbo_json_is_null(filters_json)) {
    return 0;
  }
  if (turbo_json_type(filters_json) != TURBO_JSON_OBJECT) {
    return -1;
  }
  summary_thread_id = turbo_agent_runtime_observability_string_or_null(
      turbo_json_object_get(summary_json, "thread"), "id");

  value_json = turbo_json_object_get(filters_json, "status");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_status");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_pending_review");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_pending_review", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_failure");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_failure", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_handoff");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_handoff", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_model_error");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_model_error", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "has_guardrail_rejection");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "has_guardrail_rejection", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "replan_requested");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_BOOL) {
      return -1;
    }
    expected_bool = turbo_json_bool(value_json);
    if (turbo_json_get_bool(summary_json, "replan_requested", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "active_agent");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "active_agent");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "current_interrupt_reason");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_interrupt_reason");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_status");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_status");
    if (!summary_text || strcmp(summary_text, turbo_json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_updated_after");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text ||
        turbo_agent_runtime_record_string_compare(summary_text, turbo_json_string(value_json)) <=
            0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "latest_run_updated_before");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text ||
        turbo_agent_runtime_record_string_compare(summary_text, turbo_json_string(value_json)) >=
            0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = turbo_json_object_get(filters_json, "thread_id_prefix");
  if (value_json && !turbo_json_is_null(value_json)) {
    const char *prefix;

    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    prefix = turbo_json_string(value_json);
    if (!summary_thread_id || strncmp(summary_thread_id, prefix, strlen(prefix)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  return 0;
}

static int turbo_agent_runtime_observability_sort_options(
    const json_value_t *filters_json, const char **out_sort_by, const char **out_sort_order,
    int *out_limit) {
  const json_value_t *value_json;

  if (!out_sort_by || !out_sort_order || !out_limit) {
    return -1;
  }
  *out_sort_by = NULL;
  *out_sort_order = NULL;
  *out_limit = -1;
  if (!filters_json || turbo_json_is_null(filters_json)) {
    return 0;
  }
  if (turbo_json_type(filters_json) != TURBO_JSON_OBJECT) {
    return -1;
  }

  value_json = turbo_json_object_get(filters_json, "sort_by");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    *out_sort_by = turbo_json_string(value_json);
    if (strcmp(*out_sort_by, "latest_run_updated_at") != 0 &&
        strcmp(*out_sort_by, "thread_id") != 0) {
      return -1;
    }
  }

  value_json = turbo_json_object_get(filters_json, "sort_order");
  if (value_json && !turbo_json_is_null(value_json)) {
    if (turbo_json_type(value_json) != TURBO_JSON_STRING) {
      return -1;
    }
    *out_sort_order = turbo_json_string(value_json);
    if (strcmp(*out_sort_order, "asc") != 0 && strcmp(*out_sort_order, "desc") != 0) {
      return -1;
    }
  }

  value_json = turbo_json_object_get(filters_json, "limit");
  if (value_json && !turbo_json_is_null(value_json)) {
    double limit_value;

    if (turbo_json_type(value_json) != TURBO_JSON_NUMBER) {
      return -1;
    }
    limit_value = turbo_json_number(value_json);
    if (limit_value < 0.0) {
      return -1;
    }
    *out_limit = (int)limit_value;
  }

  if (!*out_sort_by && *out_sort_order) {
    *out_sort_by = "latest_run_updated_at";
  }
  if (*out_sort_by && !*out_sort_order) {
    *out_sort_order = (strcmp(*out_sort_by, "thread_id") == 0) ? "asc" : "desc";
  }
  return 0;
}

static int (*turbo_agent_runtime_observability_sort_compare(const char *sort_by,
                                                            const char *sort_order))(const void *,
                                                                                    const void *) {
  if (!sort_by || !sort_order) {
    return NULL;
  }
  if (strcmp(sort_by, "thread_id") == 0) {
    return (strcmp(sort_order, "desc") == 0)
               ? turbo_agent_runtime_observability_summary_thread_id_compare_desc
               : turbo_agent_runtime_observability_summary_thread_id_compare_asc;
  }
  return (strcmp(sort_order, "asc") == 0)
             ? turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_asc
             : turbo_agent_runtime_observability_summary_latest_run_updated_at_compare_desc;
}

CXX_C_API int turbo_agent_runtime_get_thread_observability_index(
    turbo_agent_runtime_t *runtime, const char *thread_id,
    json_value_t **out_index_json) {
  json_value_t *index_json = NULL;
  json_value_t *thread_json = NULL;
  json_value_t *latest_run_json = NULL;
  json_value_t *pending_run_json = NULL;
  json_value_t *thread_timeline_json = NULL;
  json_value_t *thread_lineage_json = NULL;
  json_value_t *branch_tree_json = NULL;
  json_value_t *history_events_json = NULL;
  json_value_t *trace_events_json = NULL;
  json_value_t *counts_json = NULL;
  json_value_t *control_snapshot_json = NULL;
  json_value_t *current_checkpoint_summary_json = NULL;
  json_value_t *thread_state_json = NULL;
  turbo_runtime_data_bind_value_t *thread_timeline_bind = NULL;
  turbo_runtime_data_bind_value_t *history_events_bind = NULL;
  turbo_runtime_data_bind_value_t *trace_events_bind = NULL;
  turbo_runtime_data_bind_value_t *thread_state_bind = NULL;
  turbo_runtime_data_bind_value_t *control_snapshot_bind = NULL;
  const json_value_t *review_json = NULL;
  const json_value_t *replan_json = NULL;
  const json_value_t *failure_json = NULL;
  const json_value_t *model_error_json = NULL;
  const json_value_t *guardrail_json = NULL;
  const json_value_t *supervisor_json = NULL;
  const char *current_status = NULL;
  const char *current_interrupt_reason = NULL;
  const char *current_pending_action = NULL;
  const char *active_agent = NULL;
  const char *latest_run_status = NULL;
  const char *latest_run_updated_at = NULL;
  const char *pending_run_id = NULL;
  const char *pending_checkpoint_id = NULL;
  char *owned_executor_failure_reason = NULL;
  const char *handoff_target_agent = NULL;
  const char *handoff_reason = NULL;
  const char *current_failure_reason = NULL;
  const char *current_review_note = NULL;
  bool has_pending_review = false;
  bool has_handoff = false;
  bool has_failure = false;
  bool has_model_error = false;
  bool has_guardrail_rejection = false;
  bool replan_requested = false;

  if (!runtime || !thread_id || !out_index_json) {
    return -1;
  }
  *out_index_json = NULL;

  if (turbo_agent_runtime_get_thread(runtime, thread_id, &thread_json) != 0 || !thread_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_latest_run(runtime, thread_id, &latest_run_json) != 0) {
    turbo_free_json(&latest_run_json);
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    turbo_free_json(&pending_run_json);
  }
  if (turbo_agent_runtime_get_thread_timeline_bind(runtime, thread_id, &thread_timeline_bind) != 0 ||
      !thread_timeline_bind) {
    goto cleanup;
  }
  thread_timeline_json = turbo_runtime_data_bind_value_to_json(thread_timeline_bind);
  if (!thread_timeline_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_list_thread_lineage(runtime, thread_id, &thread_lineage_json) != 0 ||
      !thread_lineage_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_branch_tree(runtime, thread_id, &branch_tree_json) != 0 ||
      !branch_tree_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_load_thread_history_events_bind(runtime, thread_id,
                                                          &history_events_bind) != 0 ||
      !history_events_bind) {
    goto cleanup;
  }
  history_events_json = turbo_runtime_data_bind_value_to_json(history_events_bind);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_trace_events_bind(runtime, thread_id, &trace_events_bind) !=
          0 ||
      !trace_events_bind) {
    goto cleanup;
  }
  trace_events_json = turbo_runtime_data_bind_value_to_json(trace_events_bind);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_state_bind(runtime, thread_id, &thread_state_bind) == 0 &&
      thread_state_bind) {
    thread_state_json = turbo_runtime_data_bind_value_to_json(thread_state_bind);
    if (!thread_state_json) {
      goto cleanup;
    }
    control_snapshot_bind = turbo_agent_state_control_snapshot_bind(thread_state_bind);
    if (control_snapshot_bind) {
      control_snapshot_json = turbo_runtime_data_bind_value_to_json(control_snapshot_bind);
      if (!control_snapshot_json) {
        goto cleanup;
      }
    }
  }

  counts_json = turbo_agent_runtime_build_observability_counts_json(
      thread_timeline_json, thread_lineage_json, branch_tree_json, history_events_json,
      trace_events_json);
  if (!counts_json) {
    goto cleanup;
  }
  current_status = turbo_agent_runtime_observability_current_status(
      thread_timeline_json, pending_run_json, latest_run_json);
  latest_run_status =
      turbo_agent_runtime_observability_string_or_null(latest_run_json, "status");
  latest_run_updated_at =
      turbo_agent_runtime_observability_string_or_null(latest_run_json, "updated_at");
  pending_run_id = turbo_agent_runtime_observability_string_or_null(pending_run_json, "id");
  pending_checkpoint_id =
      turbo_agent_runtime_observability_string_or_null(pending_run_json, "latest_checkpoint_id");
  if (thread_timeline_json && turbo_json_type(thread_timeline_json) == TURBO_JSON_OBJECT) {
    const json_value_t *resolved_current_checkpoint_json =
        turbo_json_object_get(thread_timeline_json, "resolved_current_checkpoint");
    const char *next_node =
        turbo_agent_runtime_observability_string_or_null(resolved_current_checkpoint_json,
                                                         "next_node");
    if (resolved_current_checkpoint_json &&
        turbo_json_type(resolved_current_checkpoint_json) == TURBO_JSON_OBJECT) {
      current_checkpoint_summary_json = turbo_json_clone(resolved_current_checkpoint_json);
      if (!current_checkpoint_summary_json) {
        goto cleanup;
      }
    }
    turbo_agent_runtime_interrupt_metadata(current_status, next_node, thread_state_json,
                                           &current_interrupt_reason, &current_pending_action,
                                           NULL, NULL, &owned_executor_failure_reason);
  }
  if (control_snapshot_json && turbo_json_type(control_snapshot_json) == TURBO_JSON_OBJECT) {
    review_json = turbo_json_object_get(control_snapshot_json, "review");
    replan_json = turbo_json_object_get(control_snapshot_json, "replan");
    failure_json = turbo_json_object_get(control_snapshot_json, "failure");
    model_error_json = turbo_json_object_get(control_snapshot_json, "model_error");
    guardrail_json = turbo_json_object_get(control_snapshot_json, "guardrail");
    supervisor_json = turbo_json_object_get(control_snapshot_json, "supervisor");
  }
  if (review_json && turbo_json_type(review_json) == TURBO_JSON_OBJECT) {
    has_pending_review = turbo_json_get_bool(review_json, "required", false) &&
                         !turbo_json_get_bool(review_json, "approved", false);
    current_review_note =
        turbo_agent_runtime_observability_string_or_null(review_json, "note");
  }
  if (replan_json && turbo_json_type(replan_json) == TURBO_JSON_OBJECT) {
    replan_requested = turbo_json_get_bool(replan_json, "requested", false);
  }
  if (failure_json && turbo_json_type(failure_json) == TURBO_JSON_OBJECT) {
    current_failure_reason =
        turbo_agent_runtime_observability_string_or_null(failure_json, "reason");
    has_failure = current_failure_reason && current_failure_reason[0] != '\0';
  }
  if (model_error_json && turbo_json_type(model_error_json) == TURBO_JSON_OBJECT) {
    has_model_error = turbo_agent_runtime_observability_string_or_null(model_error_json, "detail") !=
                      NULL;
  }
  if (guardrail_json && turbo_json_type(guardrail_json) == TURBO_JSON_OBJECT) {
    has_guardrail_rejection =
        turbo_agent_runtime_observability_string_or_null(guardrail_json, "reason") != NULL;
  }
  active_agent = turbo_agent_runtime_observability_string_or_null(supervisor_json, "active_agent");
  handoff_target_agent =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "target_agent");
  handoff_reason =
      turbo_agent_runtime_observability_string_or_null(supervisor_json, "handoff_reason");
  has_handoff = (handoff_target_agent && handoff_target_agent[0] != '\0') ||
                (handoff_reason && handoff_reason[0] != '\0');

  index_json = turbo_json_create_object();
  if (!index_json) {
    goto cleanup;
  }
  turbo_json_object_add(index_json, "thread", thread_json);
  thread_json = NULL;
  if (latest_run_json) {
    turbo_json_object_add(index_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "latest_run");
  }
  if (pending_run_json) {
    turbo_json_object_add(index_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "pending_run");
  }
  turbo_json_object_add(index_json, "thread_timeline", thread_timeline_json);
  thread_timeline_json = NULL;
  turbo_json_object_add(index_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  turbo_json_object_add(index_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  if (current_status) {
    turbo_json_object_set_string(index_json, "current_status", current_status);
  } else {
    turbo_json_object_set_null(index_json, "current_status");
  }
  if (current_interrupt_reason) {
    turbo_json_object_set_string(index_json, "current_interrupt_reason",
                                 current_interrupt_reason);
  } else {
    turbo_json_object_set_null(index_json, "current_interrupt_reason");
  }
  if (current_pending_action) {
    turbo_json_object_set_string(index_json, "current_pending_action",
                                 current_pending_action);
  } else {
    turbo_json_object_set_null(index_json, "current_pending_action");
  }
  if (current_checkpoint_summary_json) {
    turbo_json_object_add(index_json, "current_checkpoint_summary",
                          current_checkpoint_summary_json);
    current_checkpoint_summary_json = NULL;
  } else {
    turbo_json_object_set_null(index_json, "current_checkpoint_summary");
  }
  if (latest_run_status) {
    turbo_json_object_set_string(index_json, "latest_run_status", latest_run_status);
  } else {
    turbo_json_object_set_null(index_json, "latest_run_status");
  }
  if (latest_run_updated_at) {
    turbo_json_object_set_string(index_json, "latest_run_updated_at",
                                 latest_run_updated_at);
  } else {
    turbo_json_object_set_null(index_json, "latest_run_updated_at");
  }
  if (pending_run_id) {
    turbo_json_object_set_string(index_json, "pending_run_id", pending_run_id);
  } else {
    turbo_json_object_set_null(index_json, "pending_run_id");
  }
  if (pending_checkpoint_id) {
    turbo_json_object_set_string(index_json, "pending_checkpoint_id",
                                 pending_checkpoint_id);
  } else {
    turbo_json_object_set_null(index_json, "pending_checkpoint_id");
  }
  turbo_json_object_set_bool(index_json, "has_failure", has_failure ? true : false);
  turbo_json_object_set_bool(index_json, "has_model_error",
                             has_model_error ? true : false);
  turbo_json_object_set_bool(index_json, "has_guardrail_rejection",
                             has_guardrail_rejection ? true : false);
  turbo_json_object_set_bool(index_json, "replan_requested",
                             replan_requested ? true : false);
  if (current_failure_reason) {
    turbo_json_object_set_string(index_json, "current_failure_reason",
                                 current_failure_reason);
  } else {
    turbo_json_object_set_null(index_json, "current_failure_reason");
  }
  if (current_review_note) {
    turbo_json_object_set_string(index_json, "current_review_note", current_review_note);
  } else {
    turbo_json_object_set_null(index_json, "current_review_note");
  }
  turbo_json_object_set_bool(index_json, "has_pending_review", has_pending_review ? true : false);
  turbo_json_object_set_bool(index_json, "has_handoff", has_handoff ? true : false);
  if (active_agent) {
    turbo_json_object_set_string(index_json, "active_agent", active_agent);
  } else {
    turbo_json_object_set_null(index_json, "active_agent");
  }
  turbo_json_object_add(index_json, "history_events", history_events_json);
  history_events_json = NULL;
  turbo_json_object_add(index_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  turbo_json_object_add(index_json, "counts", counts_json);
  counts_json = NULL;

  *out_index_json = index_json;
  index_json = NULL;
  free(owned_executor_failure_reason);
  turbo_runtime_data_bind_value_destroy(control_snapshot_bind);
  turbo_runtime_data_bind_value_destroy(thread_state_bind);
  turbo_runtime_data_bind_value_destroy(trace_events_bind);
  turbo_runtime_data_bind_value_destroy(history_events_bind);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&thread_state_json);
  turbo_free_json(&control_snapshot_json);
  return 0;

cleanup:
  free(owned_executor_failure_reason);
  turbo_runtime_data_bind_value_destroy(control_snapshot_bind);
  turbo_runtime_data_bind_value_destroy(thread_state_bind);
  turbo_runtime_data_bind_value_destroy(trace_events_bind);
  turbo_runtime_data_bind_value_destroy(history_events_bind);
  turbo_runtime_data_bind_value_destroy(thread_timeline_bind);
  turbo_free_json(&thread_state_json);
  turbo_free_json(&current_checkpoint_summary_json);
  turbo_free_json(&control_snapshot_json);
  turbo_free_json(&counts_json);
  turbo_free_json(&trace_events_json);
  turbo_free_json(&history_events_json);
  turbo_free_json(&branch_tree_json);
  turbo_free_json(&thread_lineage_json);
  turbo_free_json(&thread_timeline_json);
  turbo_free_json(&pending_run_json);
  turbo_free_json(&latest_run_json);
  turbo_free_json(&thread_json);
  turbo_free_json(&index_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_list_observability_indexes_filtered(
    turbo_agent_runtime_t *runtime, const json_value_t *filters_json,
    json_value_t **out_indexes_json) {
  json_value_t *threads_json = NULL;
  json_value_t *sorted_threads_json = NULL;
  json_value_t *indexes_json = NULL;
  json_value_t *sorted_indexes_json = NULL;
  json_value_t *limited_indexes_json = NULL;
  const char *sort_by = NULL;
  const char *sort_order = NULL;
  int limit = -1;
  int (*compare)(const void *, const void *) = NULL;
  size_t count;
  size_t i;

  if (!runtime || !out_indexes_json) {
    return -1;
  }
  *out_indexes_json = NULL;
  if (turbo_agent_runtime_observability_sort_options(filters_json, &sort_by, &sort_order,
                                                     &limit) != 0) {
    return -1;
  }
  compare = turbo_agent_runtime_observability_sort_compare(sort_by, sort_order);
  if (turbo_agent_runtime_store_list_json(runtime, turbo_agent_runtime_threads_collection, NULL,
                                          NULL, &threads_json) != 0 ||
      !threads_json) {
    goto cleanup;
  }
  sorted_threads_json = turbo_agent_runtime_sorted_json_array_clone(
      threads_json, turbo_agent_runtime_run_updated_at_compare_desc);
  if (!sorted_threads_json) {
    goto cleanup;
  }
  indexes_json = turbo_json_create_array();
  if (!indexes_json) {
    goto cleanup;
  }

  count = turbo_json_array_size(sorted_threads_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *thread_json = turbo_json_array_get(sorted_threads_json, i);
    const char *thread_id;
    json_value_t *index_json = NULL;
    json_value_t *summary_json = NULL;
    bool matches_filters = false;

    if (!thread_json || turbo_json_type(thread_json) != TURBO_JSON_OBJECT) {
      goto cleanup;
    }
    thread_id = turbo_json_get_string(thread_json, "id");
    if (!thread_id || !thread_id[0]) {
      goto cleanup;
    }
    if (turbo_agent_runtime_get_thread_observability_index(runtime, thread_id, &index_json) != 0 ||
        !index_json ||
        turbo_agent_runtime_build_observability_index_summary(index_json, &summary_json) != 0 ||
        !summary_json) {
      turbo_free_json(&index_json);
      turbo_free_json(&summary_json);
      goto cleanup;
    }
    if (turbo_agent_runtime_observability_summary_matches_filters(summary_json, filters_json,
                                                                  &matches_filters) != 0) {
      turbo_free_json(&index_json);
      turbo_free_json(&summary_json);
      goto cleanup;
    }
    turbo_free_json(&index_json);
    if (!matches_filters) {
      turbo_free_json(&summary_json);
      continue;
    }
    turbo_json_array_add(indexes_json, summary_json);
    summary_json = NULL;

    if (!compare && limit >= 0 && (int)turbo_json_array_size(indexes_json) >= limit) {
      break;
    }
  }

  if (compare) {
    sorted_indexes_json = turbo_agent_runtime_sorted_json_array_clone(indexes_json, compare);
    if (!sorted_indexes_json) {
      goto cleanup;
    }
    turbo_free_json(&indexes_json);
    indexes_json = sorted_indexes_json;
    sorted_indexes_json = NULL;
  }

  if (limit >= 0) {
    limited_indexes_json = turbo_json_create_array();
    if (!limited_indexes_json) {
      goto cleanup;
    }
    count = turbo_json_array_size(indexes_json);
    if ((size_t)limit < count) {
      count = (size_t)limit;
    }
    for (i = 0; i < count; ++i) {
      json_value_t *clone_json = turbo_json_clone(turbo_json_array_get(indexes_json, i));

      if (!clone_json) {
        goto cleanup;
      }
      turbo_json_array_add(limited_indexes_json, clone_json);
    }
    turbo_free_json(&indexes_json);
    indexes_json = limited_indexes_json;
    limited_indexes_json = NULL;
  }

  *out_indexes_json = indexes_json;
  indexes_json = NULL;
  turbo_free_json(&threads_json);
  turbo_free_json(&sorted_threads_json);
  return 0;

cleanup:
  turbo_free_json(&threads_json);
  turbo_free_json(&sorted_threads_json);
  turbo_free_json(&sorted_indexes_json);
  turbo_free_json(&limited_indexes_json);
  turbo_free_json(&indexes_json);
  return -1;
}

CXX_C_API int turbo_agent_runtime_list_observability_indexes(
    turbo_agent_runtime_t *runtime, json_value_t **out_indexes_json) {
  return turbo_agent_runtime_list_observability_indexes_filtered(runtime, NULL, out_indexes_json);
}

