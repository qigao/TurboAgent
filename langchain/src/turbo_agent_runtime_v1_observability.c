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
  const json_value_t *a_thread_json = json_object_get(a, "thread");
  const json_value_t *b_thread_json = json_object_get(b, "thread");
  const char *a_thread_id =
      (a_thread_json && json_type(a_thread_json) == JSON_OBJECT)
          ? json_get_string(a_thread_json, "id")
          : NULL;
  const char *b_thread_id =
      (b_thread_json && json_type(b_thread_json) == JSON_OBJECT)
          ? json_get_string(b_thread_json, "id")
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
  const char *a_updated_at = json_get_string(a, "latest_run_updated_at");
  const char *b_updated_at = json_get_string(b, "latest_run_updated_at");
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
  if (!runs_json || json_type(runs_json) != JSON_ARRAY) {
    return;
  }

  count = json_array_size(runs_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *run_record = json_array_get(runs_json, i);
    const char *status;

    if (!run_record || json_type(run_record) != JSON_OBJECT) {
      continue;
    }
    status = json_get_string(run_record, "status");
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
  json_value_t *counts_json = json_create_object();

  if (!counts_json) {
    return NULL;
  }

  if (timeline_json && json_type(timeline_json) == JSON_OBJECT) {
    runs_json = json_object_get(timeline_json, "runs");
    current_run_checkpoints_json =
        json_object_get(timeline_json, "current_run_checkpoints");
  }
  if (branch_tree_json && json_type(branch_tree_json) == JSON_OBJECT) {
    branches_json = json_object_get(branch_tree_json, "branches");
    edges_json = json_object_get(branch_tree_json, "edges");
  } else if (lineage_json && json_type(lineage_json) == JSON_OBJECT) {
    branches_json = json_object_get(lineage_json, "branches");
  }

  turbo_agent_runtime_observability_counts_from_runs(runs_json, &runs, &interrupted_runs,
                                                     &completed_runs);
  json_object_set_number(counts_json, "runs", (double)runs);
  json_object_set_number(counts_json, "interrupted_runs", (double)interrupted_runs);
  json_object_set_number(counts_json, "completed_runs", (double)completed_runs);
  json_object_set_number(
      counts_json, "current_run_checkpoints",
      (double)((current_run_checkpoints_json &&
                json_type(current_run_checkpoints_json) == JSON_ARRAY)
                   ? json_array_size(current_run_checkpoints_json)
                   : 0));
  json_object_set_number(
      counts_json, "branches",
      (double)((branches_json && json_type(branches_json) == JSON_ARRAY)
                   ? json_array_size(branches_json)
                   : 0));
  json_object_set_number(
      counts_json, "edges",
      (double)((edges_json && json_type(edges_json) == JSON_ARRAY)
                   ? json_array_size(edges_json)
                   : 0));
  json_object_set_number(
      counts_json, "history_events",
      (double)((history_events_json && json_type(history_events_json) == JSON_ARRAY)
                   ? json_array_size(history_events_json)
                   : 0));
  json_object_set_number(
      counts_json, "trace_events",
      (double)((trace_events_json && json_type(trace_events_json) == JSON_ARRAY)
                   ? json_array_size(trace_events_json)
                   : 0));
  return counts_json;
}

static const char *turbo_agent_runtime_observability_string_or_null(const json_value_t *object_json,
                                                                    const char *key) {
  const char *value;

  if (!object_json || json_type(object_json) != JSON_OBJECT || !key) {
    return NULL;
  }
  value = json_get_string(object_json, key);
  return (value && value[0] != '\0') ? value : NULL;
}

static const char *turbo_agent_runtime_observability_current_status(
    const json_value_t *timeline_json, const json_value_t *pending_run_json,
    const json_value_t *latest_run_json) {
  const json_value_t *resolved_current_run_json = NULL;
  const char *status = NULL;

  if (timeline_json && json_type(timeline_json) == JSON_OBJECT) {
    resolved_current_run_json = json_object_get(timeline_json, "resolved_current_run");
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
  value_json = json_object_get(source_json, key);
  if (!value_json) {
    json_object_set_null(target_json, key);
    return 0;
  }
  clone_json = json_clone(value_json);
  if (!clone_json) {
    return -1;
  }
  json_object_add(target_json, key, clone_json);
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

  if (!index_json || json_type(index_json) != JSON_OBJECT || !out_summary_json) {
    return -1;
  }
  *out_summary_json = NULL;
  summary_json = json_create_object();
  if (!summary_json) {
    return -1;
  }
  for (i = 0; i < sizeof(summary_keys) / sizeof(summary_keys[0]); ++i) {
    if (turbo_agent_runtime_observability_summary_copy_field(summary_json, index_json,
                                                             summary_keys[i]) != 0) {
      json_free(summary_json); summary_json = NULL;
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

  if (!summary_json || json_type(summary_json) != JSON_OBJECT || !out_matches) {
    return -1;
  }
  *out_matches = true;
  if (!filters_json || json_is_null(filters_json)) {
    return 0;
  }
  if (json_type(filters_json) != JSON_OBJECT) {
    return -1;
  }
  summary_thread_id = turbo_agent_runtime_observability_string_or_null(
      json_object_get(summary_json, "thread"), "id");

  value_json = json_object_get(filters_json, "status");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_status");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_pending_review");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_pending_review", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_failure");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_failure", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_handoff");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_handoff", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_model_error");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_model_error", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "has_guardrail_rejection");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "has_guardrail_rejection", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "replan_requested");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_BOOL) {
      return -1;
    }
    expected_bool = json_bool(value_json);
    if (json_get_bool(summary_json, "replan_requested", false) != expected_bool) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "active_agent");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text = turbo_agent_runtime_observability_string_or_null(summary_json, "active_agent");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "current_interrupt_reason");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "current_interrupt_reason");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_status");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_status");
    if (!summary_text || strcmp(summary_text, json_string(value_json)) != 0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_updated_after");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text ||
        turbo_agent_runtime_record_string_compare(summary_text, json_string(value_json)) <=
            0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "latest_run_updated_before");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    summary_text =
        turbo_agent_runtime_observability_string_or_null(summary_json, "latest_run_updated_at");
    if (!summary_text ||
        turbo_agent_runtime_record_string_compare(summary_text, json_string(value_json)) >=
            0) {
      *out_matches = false;
      return 0;
    }
  }

  value_json = json_object_get(filters_json, "thread_id_prefix");
  if (value_json && !json_is_null(value_json)) {
    const char *prefix;

    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    prefix = json_string(value_json);
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
  if (!filters_json || json_is_null(filters_json)) {
    return 0;
  }
  if (json_type(filters_json) != JSON_OBJECT) {
    return -1;
  }

  value_json = json_object_get(filters_json, "sort_by");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    *out_sort_by = json_string(value_json);
    if (strcmp(*out_sort_by, "latest_run_updated_at") != 0 &&
        strcmp(*out_sort_by, "thread_id") != 0) {
      return -1;
    }
  }

  value_json = json_object_get(filters_json, "sort_order");
  if (value_json && !json_is_null(value_json)) {
    if (json_type(value_json) != JSON_STRING) {
      return -1;
    }
    *out_sort_order = json_string(value_json);
    if (strcmp(*out_sort_order, "asc") != 0 && strcmp(*out_sort_order, "desc") != 0) {
      return -1;
    }
  }

  value_json = json_object_get(filters_json, "limit");
  if (value_json && !json_is_null(value_json)) {
    double limit_value;

    if (json_type(value_json) != JSON_NUMBER) {
      return -1;
    }
    limit_value = json_number(value_json);
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
  json_value_t *thread_timeline_json_value = NULL;
  json_value_t *history_events_json_value = NULL;
  json_value_t *trace_events_json_value = NULL;
  json_value_t *thread_state_json_value = NULL;
  json_value_t *control_snapshot_json_value = NULL;
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
    json_free(latest_run_json); latest_run_json = NULL;
  }
  if (turbo_agent_runtime_get_pending_run(runtime, thread_id, &pending_run_json) != 0) {
    json_free(pending_run_json); pending_run_json = NULL;
  }
  if (turbo_agent_runtime_get_thread_timeline_json_value(runtime, thread_id, &thread_timeline_json_value) != 0 ||
      !thread_timeline_json_value) {
    goto cleanup;
  }
  thread_timeline_json = json_clone(thread_timeline_json_value);
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
  if (turbo_agent_runtime_load_thread_history_events_json_value(runtime, thread_id,
                                                          &history_events_json_value) != 0 ||
      !history_events_json_value) {
    goto cleanup;
  }
  history_events_json = json_clone(history_events_json_value);
  if (!history_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_trace_events_json_value(runtime, thread_id, &trace_events_json_value) !=
          0 ||
      !trace_events_json_value) {
    goto cleanup;
  }
  trace_events_json = json_clone(trace_events_json_value);
  if (!trace_events_json) {
    goto cleanup;
  }
  if (turbo_agent_runtime_get_thread_state_json_value(runtime, thread_id, &thread_state_json_value) == 0 &&
      thread_state_json_value) {
    thread_state_json = json_clone(thread_state_json_value);
    if (!thread_state_json) {
      goto cleanup;
    }
    control_snapshot_json_value = turbo_agent_state_control_snapshot_json_value(thread_state_json_value);
    if (control_snapshot_json_value) {
      control_snapshot_json = json_clone(control_snapshot_json_value);
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
  if (thread_timeline_json && json_type(thread_timeline_json) == JSON_OBJECT) {
    const json_value_t *resolved_current_checkpoint_json =
        json_object_get(thread_timeline_json, "resolved_current_checkpoint");
    const char *next_node =
        turbo_agent_runtime_observability_string_or_null(resolved_current_checkpoint_json,
                                                         "next_node");
    if (resolved_current_checkpoint_json &&
        json_type(resolved_current_checkpoint_json) == JSON_OBJECT) {
      current_checkpoint_summary_json = json_clone(resolved_current_checkpoint_json);
      if (!current_checkpoint_summary_json) {
        goto cleanup;
      }
    }
    turbo_agent_runtime_interrupt_metadata(current_status, next_node, thread_state_json,
                                           &current_interrupt_reason, &current_pending_action,
                                           NULL, NULL, &owned_executor_failure_reason);
  }
  if (control_snapshot_json && json_type(control_snapshot_json) == JSON_OBJECT) {
    review_json = json_object_get(control_snapshot_json, "review");
    replan_json = json_object_get(control_snapshot_json, "replan");
    failure_json = json_object_get(control_snapshot_json, "failure");
    model_error_json = json_object_get(control_snapshot_json, "model_error");
    guardrail_json = json_object_get(control_snapshot_json, "guardrail");
    supervisor_json = json_object_get(control_snapshot_json, "supervisor");
  }
  if (review_json && json_type(review_json) == JSON_OBJECT) {
    has_pending_review = json_get_bool(review_json, "required", false) &&
                         !json_get_bool(review_json, "approved", false);
    current_review_note =
        turbo_agent_runtime_observability_string_or_null(review_json, "note");
  }
  if (replan_json && json_type(replan_json) == JSON_OBJECT) {
    replan_requested = json_get_bool(replan_json, "requested", false);
  }
  if (failure_json && json_type(failure_json) == JSON_OBJECT) {
    current_failure_reason =
        turbo_agent_runtime_observability_string_or_null(failure_json, "reason");
    has_failure = current_failure_reason && current_failure_reason[0] != '\0';
  }
  if (model_error_json && json_type(model_error_json) == JSON_OBJECT) {
    has_model_error = turbo_agent_runtime_observability_string_or_null(model_error_json, "detail") !=
                      NULL;
  }
  if (guardrail_json && json_type(guardrail_json) == JSON_OBJECT) {
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

  index_json = json_create_object();
  if (!index_json) {
    goto cleanup;
  }
  json_object_add(index_json, "thread", thread_json);
  thread_json = NULL;
  if (latest_run_json) {
    json_object_add(index_json, "latest_run", latest_run_json);
    latest_run_json = NULL;
  } else {
    json_object_set_null(index_json, "latest_run");
  }
  if (pending_run_json) {
    json_object_add(index_json, "pending_run", pending_run_json);
    pending_run_json = NULL;
  } else {
    json_object_set_null(index_json, "pending_run");
  }
  json_object_add(index_json, "thread_timeline", thread_timeline_json);
  thread_timeline_json = NULL;
  json_object_add(index_json, "thread_lineage", thread_lineage_json);
  thread_lineage_json = NULL;
  json_object_add(index_json, "branch_tree", branch_tree_json);
  branch_tree_json = NULL;
  if (current_status) {
    json_object_set_string(index_json, "current_status", current_status);
  } else {
    json_object_set_null(index_json, "current_status");
  }
  if (current_interrupt_reason) {
    json_object_set_string(index_json, "current_interrupt_reason",
                                 current_interrupt_reason);
  } else {
    json_object_set_null(index_json, "current_interrupt_reason");
  }
  if (current_pending_action) {
    json_object_set_string(index_json, "current_pending_action",
                                 current_pending_action);
  } else {
    json_object_set_null(index_json, "current_pending_action");
  }
  if (current_checkpoint_summary_json) {
    json_object_add(index_json, "current_checkpoint_summary",
                          current_checkpoint_summary_json);
    current_checkpoint_summary_json = NULL;
  } else {
    json_object_set_null(index_json, "current_checkpoint_summary");
  }
  if (latest_run_status) {
    json_object_set_string(index_json, "latest_run_status", latest_run_status);
  } else {
    json_object_set_null(index_json, "latest_run_status");
  }
  if (latest_run_updated_at) {
    json_object_set_string(index_json, "latest_run_updated_at",
                                 latest_run_updated_at);
  } else {
    json_object_set_null(index_json, "latest_run_updated_at");
  }
  if (pending_run_id) {
    json_object_set_string(index_json, "pending_run_id", pending_run_id);
  } else {
    json_object_set_null(index_json, "pending_run_id");
  }
  if (pending_checkpoint_id) {
    json_object_set_string(index_json, "pending_checkpoint_id",
                                 pending_checkpoint_id);
  } else {
    json_object_set_null(index_json, "pending_checkpoint_id");
  }
  json_object_set_bool(index_json, "has_failure", has_failure ? true : false);
  json_object_set_bool(index_json, "has_model_error",
                             has_model_error ? true : false);
  json_object_set_bool(index_json, "has_guardrail_rejection",
                             has_guardrail_rejection ? true : false);
  json_object_set_bool(index_json, "replan_requested",
                             replan_requested ? true : false);
  if (current_failure_reason) {
    json_object_set_string(index_json, "current_failure_reason",
                                 current_failure_reason);
  } else {
    json_object_set_null(index_json, "current_failure_reason");
  }
  if (current_review_note) {
    json_object_set_string(index_json, "current_review_note", current_review_note);
  } else {
    json_object_set_null(index_json, "current_review_note");
  }
  json_object_set_bool(index_json, "has_pending_review", has_pending_review ? true : false);
  json_object_set_bool(index_json, "has_handoff", has_handoff ? true : false);
  if (active_agent) {
    json_object_set_string(index_json, "active_agent", active_agent);
  } else {
    json_object_set_null(index_json, "active_agent");
  }
  json_object_add(index_json, "history_events", history_events_json);
  history_events_json = NULL;
  json_object_add(index_json, "trace_events", trace_events_json);
  trace_events_json = NULL;
  json_object_add(index_json, "counts", counts_json);
  counts_json = NULL;

  *out_index_json = index_json;
  index_json = NULL;
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  json_free(thread_state_json); thread_state_json = NULL;
  json_free(control_snapshot_json); control_snapshot_json = NULL;
  return 0;

cleanup:
  free(owned_executor_failure_reason);
  turbo_runtime_json_destroy(control_snapshot_json_value);
  turbo_runtime_json_destroy(thread_state_json_value);
  turbo_runtime_json_destroy(trace_events_json_value);
  turbo_runtime_json_destroy(history_events_json_value);
  turbo_runtime_json_destroy(thread_timeline_json_value);
  json_free(thread_state_json); thread_state_json = NULL;
  json_free(current_checkpoint_summary_json); current_checkpoint_summary_json = NULL;
  json_free(control_snapshot_json); control_snapshot_json = NULL;
  json_free(counts_json); counts_json = NULL;
  json_free(trace_events_json); trace_events_json = NULL;
  json_free(history_events_json); history_events_json = NULL;
  json_free(branch_tree_json); branch_tree_json = NULL;
  json_free(thread_lineage_json); thread_lineage_json = NULL;
  json_free(thread_timeline_json); thread_timeline_json = NULL;
  json_free(pending_run_json); pending_run_json = NULL;
  json_free(latest_run_json); latest_run_json = NULL;
  json_free(thread_json); thread_json = NULL;
  json_free(index_json); index_json = NULL;
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
  indexes_json = json_create_array();
  if (!indexes_json) {
    goto cleanup;
  }

  count = json_array_size(sorted_threads_json);
  for (i = 0; i < count; ++i) {
    const json_value_t *thread_json = json_array_get(sorted_threads_json, i);
    const char *thread_id;
    json_value_t *index_json = NULL;
    json_value_t *summary_json = NULL;
    bool matches_filters = false;

    if (!thread_json || json_type(thread_json) != JSON_OBJECT) {
      goto cleanup;
    }
    thread_id = json_get_string(thread_json, "id");
    if (!thread_id || !thread_id[0]) {
      goto cleanup;
    }
    if (turbo_agent_runtime_get_thread_observability_index(runtime, thread_id, &index_json) != 0 ||
        !index_json ||
        turbo_agent_runtime_build_observability_index_summary(index_json, &summary_json) != 0 ||
        !summary_json) {
      json_free(index_json); index_json = NULL;
      json_free(summary_json); summary_json = NULL;
      goto cleanup;
    }
    if (turbo_agent_runtime_observability_summary_matches_filters(summary_json, filters_json,
                                                                  &matches_filters) != 0) {
      json_free(index_json); index_json = NULL;
      json_free(summary_json); summary_json = NULL;
      goto cleanup;
    }
    json_free(index_json); index_json = NULL;
    if (!matches_filters) {
      json_free(summary_json); summary_json = NULL;
      continue;
    }
    json_array_add(indexes_json, summary_json);
    summary_json = NULL;

    if (!compare && limit >= 0 && (int)json_array_size(indexes_json) >= limit) {
      break;
    }
  }

  if (compare) {
    sorted_indexes_json = turbo_agent_runtime_sorted_json_array_clone(indexes_json, compare);
    if (!sorted_indexes_json) {
      goto cleanup;
    }
    json_free(indexes_json); indexes_json = NULL;
    indexes_json = sorted_indexes_json;
    sorted_indexes_json = NULL;
  }

  if (limit >= 0) {
    limited_indexes_json = json_create_array();
    if (!limited_indexes_json) {
      goto cleanup;
    }
    count = json_array_size(indexes_json);
    if ((size_t)limit < count) {
      count = (size_t)limit;
    }
    for (i = 0; i < count; ++i) {
      json_value_t *clone_json = json_clone(json_array_get(indexes_json, i));

      if (!clone_json) {
        goto cleanup;
      }
      json_array_add(limited_indexes_json, clone_json);
    }
    json_free(indexes_json); indexes_json = NULL;
    indexes_json = limited_indexes_json;
    limited_indexes_json = NULL;
  }

  *out_indexes_json = indexes_json;
  indexes_json = NULL;
  json_free(threads_json); threads_json = NULL;
  json_free(sorted_threads_json); sorted_threads_json = NULL;
  return 0;

cleanup:
  json_free(threads_json); threads_json = NULL;
  json_free(sorted_threads_json); sorted_threads_json = NULL;
  json_free(sorted_indexes_json); sorted_indexes_json = NULL;
  json_free(limited_indexes_json); limited_indexes_json = NULL;
  json_free(indexes_json); indexes_json = NULL;
  return -1;
}

CXX_C_API int turbo_agent_runtime_list_observability_indexes(
    turbo_agent_runtime_t *runtime, json_value_t **out_indexes_json) {
  return turbo_agent_runtime_list_observability_indexes_filtered(runtime, NULL, out_indexes_json);
}

