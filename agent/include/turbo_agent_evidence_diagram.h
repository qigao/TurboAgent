#ifndef TURBO_AGENT_EVIDENCE_DIAGRAM_H
#define TURBO_AGENT_EVIDENCE_DIAGRAM_H
#include "turbo_runtime_diagram.h"
#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_AGENT_DIAGRAM_MAX_JOURNALS 32u
#define TURBO_AGENT_DIAGRAM_MAX_FIELD_BYTES 1024u
#define TURBO_AGENT_DIAGRAM_MAX_ORIGIN_CALLS 64u
#define TURBO_AGENT_DIAGRAM_CALL_ORIGINS 1u
#define TURBO_AGENT_DIAGRAM_WORKFLOW_STEPS 2u

/** Add selected current tool-journal observations to the published thread view.
 * journal_ids holds 1..MAX_JOURNALS unique nonempty UTF-8 record keys. Selected
 * strings obey MAX_FIELD_BYTES; combined nodes/edges obey turbo_diagram.h.
 * Journal nodes use journal:<id>, collection=agent_tool_journal, record_id,
 * copied identity/hash/phase/status/policy/timestamp and freshness=current_record.
 * Edges mean same_run (thread/run identity only), never checkpoint inclusion,
 * plan-step identity, tool success or execution order. Started side effects
 * remain unknown. Stored hashes are not recomputed or authenticated.
 * No arguments/output/detail payload is exported; backend get loads the record.
 *
 * Borrow inputs for this synchronous call. Caller keeps runtime/store alive and
 * quiescent, including external writers, across all reads; no transaction is
 * created. No tools execute and no records change. Clear out_view on entry;
 * publish only a complete independent owned view, released with json_free.
 * Return OK; EINVAL for input/duplicates; ENOENT for missing head/record/ancestor;
 * EBUSY for unpublished head/backend busy; EPROTO for malformed selected metadata
 * or mismatched thread/run; ENOSPC for bounds; ENOMEM or propagated store errors.
 * Example: const char *ids[] = {journal_id};
 * turbo_agent_thread_tool_diagram(runtime, thread_id, ids, 1, &view);
 * check status; turbo_diagram_mermaid(view, &text); check status;
 * tstr_free(text); json_free(view).
 */
CXX_C_API int turbo_agent_thread_tool_diagram(turbo_agent_runtime_t *runtime,
    const char *thread_id, const char *const *journal_ids, size_t journal_count,
    json_value_t **out_view);

/** Optional published model-request origins; flags is zero, CALL_ORIGINS, or
 * CALL_ORIGINS | WORKFLOW_STEPS. WORKFLOW_STEPS requires CALL_ORIGINS.
 * Inherits the ownership, quiescence, bounds and errors above. Unknown flags
 * return EINVAL. Scoped planner/executor turn keys locate snapshot events;
 * matching call_id/tool name adds event:<turn_key> and same_call_identity edges
 * with exact source_pointer metadata in thread_heads. No argument comparison,
 * execution order, success or compiled plan-step identity is implied.
 * Journal origin_status is matched, not_in_snapshot, or unresolved_identity
 * (legacy/unscoped keys). Malformed scopes, conflicting names or duplicate
 * matching calls return EPROTO; more than MAX_ORIGIN_CALLS entries in a selected
 * event returns ENOSPC. The full head is loaded, but no event history is scanned.
 * WORKFLOW_STEPS also projects explicit executor plan_step_origin metadata as
 * workflow-step:<plan_version>:<step_index> nodes and prepared_for_step edges.
 * Model event step_origin_status is matched, unrecorded (old state), or
 * not_applicable (planner). Invalid recorded bindings or missing referenced
 * steps return EPROTO. Step text is excluded. This identifies a prepared
 * workflow step, never a compiled DAG step, approval, or successful execution.
 * Example: pass TURBO_AGENT_DIAGRAM_CALL_ORIGINS to opt in; zero matches the
 * original function's behavior. All output remains independently owned.
 */
CXX_C_API int turbo_agent_thread_tool_diagram_ex(turbo_agent_runtime_t *runtime,
    const char *thread_id, const char *const *journal_ids, size_t journal_count,
    unsigned flags, json_value_t **out_view);

#ifdef __cplusplus
}
#endif
#endif
