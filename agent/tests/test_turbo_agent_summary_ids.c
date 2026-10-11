#include "../src/turbo_agent_util_internal.h"
#include "tinytest.h"

spec("Session summary identity commit") {
  static tstr thread_id, run_id, checkpoint_id;
  static json_value_t *summary;

  before_each() {
    thread_id = tstr_dup("old-thread");
    run_id = tstr_dup("old-run");
    checkpoint_id = tstr_dup("old-checkpoint");
    summary = json_create_object();
    check_not_null(thread_id);
    check_not_null(run_id);
    check_not_null(checkpoint_id);
    check_not_null(summary);
    check_equal(turbo_runtime_json_object_set_string(summary, "thread_id", "new-thread"), 0);
    check_equal(turbo_runtime_json_object_set_string(summary, "run_id", "new-run"), 0);
  }

  after_each() {
    tstr_free(thread_id);
    tstr_free(run_id);
    tstr_free(checkpoint_id);
    thread_id = run_id = checkpoint_id = NULL;
    json_free(summary);
    summary = NULL;
  }

  it("owns committed ids independently of the returned Runtime summary") {
    check_equal(turbo_runtime_json_object_set_string(summary, "checkpoint_id", "new-checkpoint"),
                0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_equal(thread_id, "new-thread");
    check_equal(run_id, "new-run");
    check_equal(checkpoint_id, "new-checkpoint");
    check_equal(turbo_runtime_json_object_set_string(summary, "thread_id", "caller-change"), 0);
    check_equal(turbo_runtime_json_object_set_string(summary, "run_id", "caller-change"), 0);
    check_equal(turbo_runtime_json_object_set_string(summary, "checkpoint_id", "caller-change"), 0);
    json_free(summary);
    summary = NULL;
    check_equal(thread_id, "new-thread");
    check_equal(run_id, "new-run");
    check_equal(checkpoint_id, "new-checkpoint");
  }

  it("keeps all existing identities when a required summary field is invalid") {
    tstr original_thread = thread_id;
    tstr original_run = run_id;
    tstr original_checkpoint = checkpoint_id;
    check_equal(turbo_runtime_json_object_set_string(summary, "checkpoint_id", "new-checkpoint"),
                0);
    check_equal(turbo_runtime_json_object_set_int64(summary, "run_id", 7), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                -1);
    check_true(thread_id == original_thread);
    check_true(run_id == original_run);
    check_true(checkpoint_id == original_checkpoint);
    check_equal(turbo_runtime_json_object_set_string(summary, "run_id", "new-run"), 0);
    check_equal(turbo_runtime_json_object_set_int64(summary, "thread_id", 8), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                -1);
    check_equal(turbo_agent_util_capture_summary_ids(NULL, &thread_id, &run_id, &checkpoint_id),
                -1);
    check_true(thread_id == original_thread);
    check_true(run_id == original_run);
    check_true(checkpoint_id == original_checkpoint);
    check_equal(thread_id, "old-thread");
    check_equal(run_id, "old-run");
    check_equal(checkpoint_id, "old-checkpoint");
  }

  it("retains the previous checkpoint when the summary supplies no nonempty checkpoint string") {
    tstr original_checkpoint = checkpoint_id;
    json_value_t *null_checkpoint;
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_true(checkpoint_id == original_checkpoint);
    check_equal(turbo_runtime_json_object_set_string(summary, "checkpoint_id", ""), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_true(checkpoint_id == original_checkpoint);
    check_equal(turbo_runtime_json_object_set_int64(summary, "checkpoint_id", 5), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_true(checkpoint_id == original_checkpoint);
    null_checkpoint = json_create_null();
    check_not_null(null_checkpoint);
    check_equal(turbo_runtime_json_object_set(summary, "checkpoint_id", null_checkpoint), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_true(checkpoint_id == original_checkpoint);
    check_equal(thread_id, "new-thread");
    check_equal(run_id, "new-run");
    check_equal(checkpoint_id, "old-checkpoint");
  }

  it("preserves legacy empty-string admission for required ids") {
    check_equal(turbo_runtime_json_object_set_string(summary, "thread_id", ""), 0);
    check_equal(turbo_runtime_json_object_set_string(summary, "run_id", ""), 0);
    check_equal(turbo_agent_util_capture_summary_ids(summary, &thread_id, &run_id, &checkpoint_id),
                0);
    check_not_null(thread_id);
    check_not_null(run_id);
    check_equal(tstr_len(thread_id), 0);
    check_equal(tstr_len(run_id), 0);
    check_equal(checkpoint_id, "old-checkpoint");
  }
}
