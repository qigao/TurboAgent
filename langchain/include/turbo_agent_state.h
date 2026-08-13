#ifndef TURBO_AGENT_STATE_H
#define TURBO_AGENT_STATE_H

#include <platform.h>

#include "turbo_parser.h"
#include "turbo_runtime_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct turbo_agent_s turbo_agent_t;

/**
 * @brief Return the current supported state schema version.
 * @return State schema version number.
 */
CXX_C_API size_t turbo_agent_state_schema_version(void);

/**
 * @brief Create append-only agent state with `input` and `events` arrays.
 * @return JSON object owned by caller.
 */
CXX_C_API json_value_t *turbo_agent_state_create(void);

/**
 * @brief Create append-only agent state as a TurboParser JSON value tree.
 * @return State object owned by caller.
 */
CXX_C_API json_value_t *turbo_agent_state_create_json_value(void);

/**
 * @brief Return the schema version stored on a state object.
 * @param state Agent state.
 * @return Stored schema version, or zero when absent/invalid.
 */
CXX_C_API size_t turbo_agent_state_version(const json_value_t *state);

/**
 * @brief Return whether a state object uses a schema version supported by this build.
 * @param state Agent state.
 * @return 1 when supported, else 0.
 */
CXX_C_API int turbo_agent_state_version_supported(const json_value_t *state);

/**
 * @brief Append a user message to the initial input array.
 * @param state Agent state from turbo_agent_state_create().
 * @param text User message text.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_add_user_message(json_value_t *state, const char *text);

/**
 * @brief Return the main run event array from state.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL when absent.
 */
CXX_C_API const json_value_t *turbo_agent_state_events(const json_value_t *state);

/**
 * @brief Return the number of recorded run events.
 * @param state Agent state.
 * @return Run event count.
 */
CXX_C_API size_t turbo_agent_state_event_count(const json_value_t *state);

/**
 * @brief Return one recorded run event by index.
 * @param state Agent state.
 * @param index Zero-based run event index.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_event_at(const json_value_t *state,
                                                                size_t index);

/**
 * @brief Return the captured trace event array from state.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL when absent.
 */
CXX_C_API const json_value_t *turbo_agent_state_trace_events(const json_value_t *state);

/**
 * @brief Return the number of captured trace events.
 * @param state Agent state.
 * @return Trace event count.
 */
CXX_C_API size_t turbo_agent_state_trace_event_count(const json_value_t *state);

/**
 * @brief Return one captured trace event by index.
 * @param state Agent state.
 * @param index Zero-based trace event index.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_trace_event_at(const json_value_t *state,
                                                                      size_t index);

/**
 * @brief Return the captured trace event array as a TurboParser JSON-native clone.
 * @param state Agent state.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_trace_events_json_value(const json_value_t *state);

/**
 * @brief Append one canonical TurboParser JSON-native trace event into `state.trace_events`.
 * @param state Agent state as a TurboParser JSON-native object.
 * @param event Canonical trace event.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_add_trace_event_json_value(
    json_value_t *state, const json_value_t *event);

/**
 * @brief Capture one canonical TurboParser JSON-native trace event through the standard sink callback.
 * @param event Canonical trace event.
 * @param user_data Agent state as a TurboParser JSON-native object.
 */
CXX_C_API void turbo_agent_state_capture_trace_event_json_value(
    const json_value_t *event, void *user_data);

/**
 * @brief Store JSON text under `state.memory.<key>`.
 * @param state Agent state.
 * @param key Memory slot key.
 * @param value_json Serialized JSON value.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_memory_json(json_value_t *state, const char *key,
                                                       const char *value_json);

/**
 * @brief Read a memory slot from `state.memory`.
 * @param state Agent state.
 * @param key Memory slot key.
 * @return Borrowed JSON value or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_memory_json(const json_value_t *state,
                                                                   const char *key);

/**
 * @brief Load one memory slot from the attached store into `state.memory`.
 * @param agent Agent handle with store callbacks.
 * @param state Agent state.
 * @param key Memory slot key.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_load_memory(turbo_agent_t *agent, json_value_t *state,
                                                   const char *key);

/**
 * @brief Save one memory slot from `state.memory` into the attached store.
 * @param agent Agent handle with store callbacks.
 * @param state Agent state.
 * @param key Memory slot key.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_save_memory(turbo_agent_t *agent, json_value_t *state,
                                                   const char *key);

/**
 * @brief Append one layered instruction-memory record under `state.memory_context.layers`.
 *
 * This surface is intentionally separate from `state.memory`, which stores
 * structured JSON slots for runtime state. Memory-context layers are ordered
 * text fragments used to enrich model instructions.
 *
 * @param state Agent state.
 * @param scope Layer scope such as `project`, `user`, or `session`.
 * @param path Optional source path. Pass NULL when the layer is not file-backed.
 * @param text Layer body text.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_add_memory_context_layer(json_value_t *state,
                                                                const char *scope,
                                                                const char *path,
                                                                const char *text);

/**
 * @brief Return the layered instruction-memory object from state.
 * @param state Agent state.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_memory_context(const json_value_t *state);

/**
 * @brief Return the ordered instruction-memory layer array from state.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_memory_layers(const json_value_t *state);

/**
 * @brief Return the number of instruction-memory layers.
 * @param state Agent state.
 * @return Layer count.
 */
CXX_C_API size_t turbo_agent_state_memory_layer_count(const json_value_t *state);

/**
 * @brief Return one instruction-memory layer by index.
 * @param state Agent state.
 * @param index Zero-based layer index.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_memory_layer_at(const json_value_t *state,
                                                                       size_t index);

/**
 * @brief Build merged instruction-memory text from all stored layers.
 * @param state Agent state.
 * @return Newly allocated text owned by caller, or NULL when no layers exist.
 */
CXX_C_API char *turbo_agent_state_memory_context_text(const json_value_t *state);

/**
 * @brief Parse planner JSON and store normalized plan state under `plan`.
 * @param state Agent state.
 * @param plan_json Planner output JSON. Accepts either `{"steps":[...]}` or raw `[...]`.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_plan_from_json(json_value_t *state,
                                                          const char *plan_json);

/**
 * @brief Get the stored plan object from state.
 * @param state Agent state.
 * @return Plan object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_plan(const json_value_t *state);

/**
 * @brief Get the number of normalized plan steps.
 * @param state Agent state.
 * @return Step count.
 */
CXX_C_API size_t turbo_agent_state_plan_step_count(const json_value_t *state);

/**
 * @brief Get the current plan step index.
 * @param state Agent state.
 * @return Current zero-based step index.
 */
CXX_C_API size_t turbo_agent_state_plan_step_index(const json_value_t *state);

/**
 * @brief Get the current plan step text.
 * @param state Agent state.
 * @return Text owned by state, or NULL when plan is complete.
 */
CXX_C_API const char *turbo_agent_state_current_plan_step_text(const json_value_t *state);

/**
 * @brief Advance the current plan step index by one.
 * @param state Agent state.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_advance_plan(json_value_t *state);

/**
 * @brief Return whether the current plan is complete.
 * @param state Agent state.
 * @return 1 when complete, else 0.
 */
CXX_C_API int turbo_agent_state_plan_complete(const json_value_t *state);

/**
 * @brief Mark the current run for replanning with an optional reason.
 * @param state Agent state.
 * @param reason Optional failure/replan reason string.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_request_replan(json_value_t *state, const char *reason);

/**
 * @brief Set the maximum number of replans allowed for this run.
 * @param state Agent state.
 * @param max_replans Zero means unlimited.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_replan_limit(json_value_t *state,
                                                        size_t max_replans);

/**
 * @brief Return whether replanning has been requested.
 * @param state Agent state.
 * @return 1 when replanning is requested, else 0.
 */
CXX_C_API int turbo_agent_state_replan_requested(const json_value_t *state);

/**
 * @brief Return how many replans have already been requested.
 * @param state Agent state.
 * @return Replan count.
 */
CXX_C_API size_t turbo_agent_state_replan_count(const json_value_t *state);

/**
 * @brief Return the configured maximum replan count.
 * @param state Agent state.
 * @return Max replans, or zero when unlimited.
 */
CXX_C_API size_t turbo_agent_state_replan_limit(const json_value_t *state);

/**
 * @brief Return the current replan reason text.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_replan_reason(const json_value_t *state);

/**
 * @brief Set the current guardrail rejection record.
 * @param state Agent state.
 * @param phase Guardrail phase such as `before_model` or `after_tool`.
 * @param reason Human-readable rejection reason.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_guardrail_rejection(json_value_t *state,
                                                               const char *phase,
                                                               const char *reason);

/**
 * @brief Return the latest guardrail rejection phase.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_guardrail_rejection_phase(const json_value_t *state);

/**
 * @brief Return the latest guardrail rejection reason.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_guardrail_rejection_reason(const json_value_t *state);

/**
 * @brief Set the current failure record.
 * @param state Agent state.
 * @param kind Failure kind such as `tool_result`, `executor_text`, or `limit`.
 * @param reason Human-readable failure reason.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_failure(json_value_t *state, const char *kind,
                                                   const char *reason);

/**
 * @brief Return the current failure kind.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_failure_kind(const json_value_t *state);

/**
 * @brief Return the current failure reason.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_failure_reason(const json_value_t *state);

/**
 * @brief Build a read-only control snapshot for the current runtime state.
 *
 * The returned object summarizes review, replan, failure, model-error, and
 * guardrail-rejection control state in one stable JSON shape for inspection,
 * export, or UI rendering.
 *
 * @param state Agent state.
 * @return JSON object owned by caller, or NULL on allocation failure.
 */
CXX_C_API json_value_t *turbo_agent_state_control_snapshot(const json_value_t *state);

/**
 * @brief Build a control snapshot from a TurboParser JSON state boundary.
 * @param state Agent state as a TurboParser JSON object.
 * @return TurboParser JSON snapshot owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_agent_state_control_snapshot_json_value(const json_value_t *state);

/**
 * @brief Build a stable workflow snapshot from runtime state.
 *
 * The returned object bundles high-value workflow inspection data in one
 * exportable JSON shape, including plan state, completed steps, control
 * summary, run events, trace events, layered instruction memory, and
 * planner/executor history versions.
 *
 * @param state Agent state.
 * @return JSON object owned by caller, or NULL on allocation failure.
 */
CXX_C_API json_value_t *turbo_agent_state_workflow_snapshot(const json_value_t *state);

/**
 * @brief Build a workflow snapshot from a TurboParser JSON state boundary.
 * @param state Agent state as a TurboParser JSON object.
 * @return TurboParser JSON snapshot owned by caller, or NULL on failure.
 */
CXX_C_API json_value_t *
turbo_agent_state_workflow_snapshot_json_value(const json_value_t *state);

/**
 * @brief Set the current final answer text explicitly.
 * @param state Agent state.
 * @param text Final answer text.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_final_answer(json_value_t *state, const char *text);

/**
 * @brief Request human review with an optional note.
 * @param state Agent state.
 * @param note Optional review note.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_request_review(json_value_t *state, const char *note);

/**
 * @brief Mark the current review gate as approved or rejected.
 * @param state Agent state.
 * @param approved Non-zero to approve, zero to reject.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_review_approved(json_value_t *state, int approved);

/**
 * @brief Clear the current review gate without approving it.
 * @param state Agent state.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_clear_review(json_value_t *state);

/**
 * @brief Return whether a review gate is currently required.
 * @param state Agent state.
 * @return 1 when review is required, else 0.
 */
CXX_C_API int turbo_agent_state_review_required(const json_value_t *state);

/**
 * @brief Return whether the current review gate is approved.
 * @param state Agent state.
 * @return 1 when approved, else 0.
 */
CXX_C_API int turbo_agent_state_review_approved(const json_value_t *state);

/**
 * @brief Return the current review note text.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_review_note(const json_value_t *state);

/**
 * @brief Set the currently active supervisor agent name.
 * @param state Agent state.
 * @param active_agent Agent node/name that currently owns execution.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_set_active_agent(json_value_t *state,
                                                 const char *active_agent);

/**
 * @brief Stage one supervisor handoff request to a target agent.
 * @param state Agent state.
 * @param target_agent Next agent node/name to receive control.
 * @param reason Optional handoff reason.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_request_handoff(json_value_t *state,
                                                const char *target_agent,
                                                const char *reason);

/**
 * @brief Commit the currently staged handoff and make the target agent active.
 * @param state Agent state.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_commit_handoff(json_value_t *state);

/**
 * @brief Append one supervisor inbox message.
 * @param state Agent state.
 * @param source_agent Optional source agent label.
 * @param text Inbox body text.
 * @return 0 on success.
 */
CXX_C_API int turbo_agent_state_append_supervisor_inbox_message(
    json_value_t *state, const char *source_agent, const char *text);

/**
 * @brief Return the currently active supervisor agent, if any.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_active_agent(const json_value_t *state);

/**
 * @brief Return the staged handoff target agent, if any.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_target_agent(
    const json_value_t *state);

/**
 * @brief Return the staged handoff reason, if any.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_reason(const json_value_t *state);

/**
 * @brief Return the current supervisor inbox array.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_supervisor_inbox(
    const json_value_t *state);

/**
 * @brief Return the current supervisor inbox message count.
 * @param state Agent state.
 * @return Inbox message count.
 */
CXX_C_API size_t turbo_agent_state_supervisor_inbox_count(
    const json_value_t *state);

/**
 * @brief Return one supervisor inbox message by index.
 * @param state Agent state.
 * @param index Zero-based inbox index.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_supervisor_inbox_at(
    const json_value_t *state, size_t index);

/**
 * @brief Return the accumulated supervisor handoff history array.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_supervisor_handoff_history(
    const json_value_t *state);

/**
 * @brief Return the latest recorded supervisor handoff event from state events.
 * @param state Agent state.
 * @return Borrowed event object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_latest_handoff_event(
    const json_value_t *state);

/**
 * @brief Return the phase recorded on one `handoff` event.
 * @param event Handoff event object.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_event_phase(const json_value_t *event);

/**
 * @brief Return the source agent recorded on one `handoff` event.
 * @param event Handoff event object.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_event_from_agent(const json_value_t *event);

/**
 * @brief Return the target agent recorded on one `handoff` event.
 * @param event Handoff event object.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_event_target_agent(
    const json_value_t *event);

/**
 * @brief Return the handoff reason recorded on one `handoff` event.
 * @param event Handoff event object.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_event_reason(const json_value_t *event);

/**
 * @brief Return the active agent recorded on one `handoff` event.
 * @param event Handoff event object.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_handoff_event_active_agent(
    const json_value_t *event);

/**
 * @brief Return the latest assistant text seen in model events.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_last_output_text(const json_value_t *state);

/**
 * @brief Return the latest executor-model text for the current or most recent step.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *
turbo_agent_state_latest_executor_output_text(const json_value_t *state);

/**
 * @brief Return the planner event history versions array.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *
turbo_agent_state_planner_event_versions(const json_value_t *state);

/**
 * @brief Return the number of planner event history versions.
 * @param state Agent state.
 * @return Planner event history count.
 */
CXX_C_API size_t turbo_agent_state_planner_event_version_count(const json_value_t *state);

/**
 * @brief Return one planner event history version by index.
 * @param state Agent state.
 * @param index Zero-based version index.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *
turbo_agent_state_planner_event_version_at(const json_value_t *state, size_t index);

/**
 * @brief Return one planner event history version as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @param index Zero-based version index.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_planner_event_version_json_value(const json_value_t *state,
                                                    size_t index);

/**
 * @brief Return the latest planner event history version as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_latest_planner_event_version_json_value(
    const json_value_t *state);

/**
 * @brief Return the executor event history versions array.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *
turbo_agent_state_executor_event_versions(const json_value_t *state);

/**
 * @brief Return the number of executor event history versions.
 * @param state Agent state.
 * @return Executor event history count.
 */
CXX_C_API size_t turbo_agent_state_executor_event_version_count(const json_value_t *state);

/**
 * @brief Return one executor event history version by index.
 * @param state Agent state.
 * @param index Zero-based version index.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *
turbo_agent_state_executor_event_version_at(const json_value_t *state, size_t index);

/**
 * @brief Return one executor event history version as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @param index Zero-based version index.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_executor_event_version_json_value(const json_value_t *state,
                                                     size_t index);

/**
 * @brief Return the latest executor event history version as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_latest_executor_event_version_json_value(
    const json_value_t *state);

/**
 * @brief Return the completed plan-step summary array.
 * @param state Agent state.
 * @return Borrowed JSON array or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_completed_steps(const json_value_t *state);

/**
 * @brief Return the completed plan-step summary array as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @return JsonValue-native array owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_completed_steps_json_value(const json_value_t *state);

/**
 * @brief Return the number of completed plan steps.
 * @param state Agent state.
 * @return Completed step count.
 */
CXX_C_API size_t turbo_agent_state_completed_step_count(const json_value_t *state);

/**
 * @brief Return one completed plan-step summary by index.
 * @param state Agent state.
 * @param index Zero-based completed-step index.
 * @return Borrowed JSON object or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_completed_step_at(const json_value_t *state,
                                                                         size_t index);

/**
 * @brief Return one completed plan-step summary as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @param index Zero-based completed-step index.
 * @return JsonValue-native object owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_completed_step_json_value(const json_value_t *state,
                                             size_t index);

/**
 * @brief Return the latest completed plan-step summary as a TurboParser JSON-native clone.
 * @param state Agent state or workflow snapshot as a TurboParser JSON-native object.
 * @return JsonValue-native object owned by caller, or NULL when absent.
 */
CXX_C_API json_value_t *
turbo_agent_state_latest_completed_step_json_value(const json_value_t *state);

/**
 * @brief Record the latest model-call failure details.
 * @param state Agent state.
 * @param phase Failure phase such as `build_request`, `transport`, or `parse_response`.
 * @param detail User-facing error detail.
 * @return 0 on success, non-zero on error.
 */
CXX_C_API int turbo_agent_state_set_model_error(json_value_t *state, const char *phase,
                                                       const char *detail);

/**
 * @brief Return the latest model-call failure phase.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL when no model error is stored.
 */
CXX_C_API const char *turbo_agent_state_model_error_phase(const json_value_t *state);

/**
 * @brief Return the latest model-call failure detail.
 * @param state Agent state.
 * @return Pointer owned by state, or NULL when no model error is stored.
 */
CXX_C_API const char *turbo_agent_state_model_error_detail(const json_value_t *state);

/**
 * @brief Return the latest `tool_results` event from main state events or executor history.
 * @param state Agent state.
 * @return Borrowed event object or NULL.
 */
CXX_C_API const json_value_t *
turbo_agent_state_latest_tool_results_event(const json_value_t *state);

/**
 * @brief Return the `outputs` array from one `tool_results` event.
 * @param event Tool-results event.
 * @return Borrowed array or NULL.
 */
CXX_C_API const json_value_t *turbo_agent_state_tool_results_outputs(
    const json_value_t *event);

/**
 * @brief Create one canonical tool-result output item from `call_id` and serialized output JSON/text.
 *
 * When `output` is one JSON object string, child/parent lineage fields such as
 * `child_run_id`, `parent_tool_call_id`, or `call_frame_id` are copied onto the
 * returned item so later state accessors can read them directly from
 * `tool_results.outputs[]`.
 *
 * @param call_id Tool call id from the originating model/tool call record.
 * @param output Serialized tool output payload.
 * @return JSON object owned by caller, or NULL on allocation failure.
 */
CXX_C_API json_value_t *turbo_agent_tool_result_output_item_create(const char *call_id,
                                                                   const char *output);

/**
 * @brief Return the child thread id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_child_thread_id(
    const json_value_t *output_item);

/**
 * @brief Return the child run id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_child_run_id(
    const json_value_t *output_item);

/**
 * @brief Return the child checkpoint id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_child_checkpoint_id(
    const json_value_t *output_item);

/**
 * @brief Return the child run status recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_child_status(
    const json_value_t *output_item);

/**
 * @brief Return the parent agent run id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_parent_agent_run_id(
    const json_value_t *output_item);

/**
 * @brief Return the parent tool call id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_call_id(
    const json_value_t *output_item);

/**
 * @brief Return the parent tool name recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_parent_tool_name(
    const json_value_t *output_item);

/**
 * @brief Return the parent graph run id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_parent_graph_run_id(
    const json_value_t *output_item);

/**
 * @brief Return the nested call-frame id recorded on one tool-result output item.
 * @param output_item One entry from `tool_results.outputs`.
 * @return Pointer owned by state, or NULL.
 */
CXX_C_API const char *turbo_agent_state_tool_result_call_frame_id(
    const json_value_t *output_item);

/**
 * @brief Return whether the latest executor step contains any failed tool result JSON.
 * @param state Agent state.
 * @return 1 when a latest-step tool result has `ok=false`, else 0.
 */
CXX_C_API int
turbo_agent_state_executor_tool_results_failed(const json_value_t *state);

/**
 * @brief Return the first concrete failure reason from the latest executor step.
 * @param state Agent state.
 * @return Newly allocated string owned by caller, or a generic fallback when no detail exists.
 */
CXX_C_API char *turbo_agent_executor_failure_reason(const json_value_t *state);

/**
 * @brief Return a user-facing final answer text when one is available.
 *
 * Prefers the latest executor text and falls back to the latest model output,
 * but suppresses raw tool-result JSON payloads.
 *
 * @param state Agent state.
 * @return Pointer owned by state, or NULL when no user-facing final answer exists.
 */
CXX_C_API const char *turbo_agent_state_final_answer_text(const json_value_t *state);

/**
 * @brief Parse the latest user-facing output text as JSON.
 *
 * This is intended for structured-output flows where the model is constrained
 * by a JSON schema and the caller wants a parsed JSON tree rather than raw text.
 *
 * @param state Agent state.
 * @param out_json Parsed JSON tree owned by caller on success.
 * @return 0 on success, non-zero when no valid JSON output is available.
 */
CXX_C_API int turbo_agent_state_parse_final_output_json(const json_value_t *state,
                                                               json_value_t **out_json);

/**
 * @brief Return the latest pending tool call count from the last model event.
 * @param state Agent state.
 * @return Tool call count.
 */
CXX_C_API size_t
turbo_agent_state_pending_tool_calls(const json_value_t *state);

#ifdef __cplusplus
}
#endif

#endif


