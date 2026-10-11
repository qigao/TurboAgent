#ifndef TURBO_RUNTIME_DIAGRAM_H
#define TURBO_RUNTIME_DIAGRAM_H
#include "turbo_runtime_history.h"
#include "turbo_diagram.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Build a read-only diagram of the published run and its checkpoint ancestors.
 * Uses the exact published snapshot; no timestamp selection or legacy fallback.
 * Includes at most MAX_NODES-1 checkpoints. Missing/malformed parents, cycles,
 * cross-thread links and bounds fail without partial output. Edges express
 * record lineage, not tool completion, step order or execution duration.
 * State, prompts, tool arguments and output payloads are not copied into the view.
 * IDs are namespaced run:<id>/checkpoint:<id>; nodes also carry collection and
 * record_id for navigation. The view owns its data and out_view is cleared.
 * Caller keeps runtime alive and quiescent across the multi-read operation.
 * Return OK, EINVAL, ENOENT (including no published head), EBUSY (empty head or
 * backend busy), EPROTO, ENOSPC or backend/allocation error. json_free the view.
 * Example: turbo_agent_runtime_thread_diagram(runtime, "thread", &view);
 * then turbo_diagram_mermaid(view, &text), checking each return value.
 */
CXX_C_API int turbo_agent_runtime_thread_diagram(turbo_agent_runtime_t *runtime,
                                                const char *thread_id, json_value_t **out_view);
#ifdef __cplusplus
}
#endif
#endif
