#ifndef TURBO_DIAGRAM_H
#define TURBO_DIAGRAM_H
#include "turbo_runtime_json.h"
#include <tstr.h>
#include <salts/error_codes.h>
#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_DIAGRAM_MAX_NODES 128u
#define TURBO_DIAGRAM_MAX_EDGES 512u
#define TURBO_DIAGRAM_MAX_TEXT_BYTES 2048u
#define TURBO_DIAGRAM_MAX_OUTPUT_BYTES (1024u * 1024u)
#define TURBO_DIAGRAM_MAX_QUERY_VALUES 8192u
#define TURBO_DIAGRAM_MAX_QUERY_DEPTH 32u
#define TURBO_DIAGRAM_MAX_QUERY_BYTES (1024u * 1024u)

/** Owned, disposable view: {schema_version:1,kind:"plan"|"execution"|"knowledge",
 * nodes:[{id,label}],edges:[{from,to,label}]}. Source adapters may add metadata;
 * the renderer reads these fields and uses record_scope="published_and_current"
 * to label mixed execution snapshots/current observations. It never executes tools or changes a
 * source. Release JSON with json_free; release rendered text with tstr_free.
 * All inputs are borrowed for the call and must remain stable. Serialize edits
 * to a view; independent views may be used concurrently. Cycles are permitted
 * in a view: this is not compiler admission or scheduling authority.
 */
CXX_C_API json_value_t *turbo_diagram_create(const char *kind);
/** Append a unique node or edge between existing nodes. Inputs are copied.
 * Return OK, EINVAL for invalid arguments/view/text, EPROTO for duplicate IDs
 * or unresolved endpoints, ENOSPC at the documented bounds, ENOMEM on failure.
 * UTF-8 text must be nonempty, at most MAX_TEXT_BYTES, without embedded NUL.
 * Empty edge labels are allowed. Failed appends leave the view unchanged. */
CXX_C_API int turbo_diagram_add_node(json_value_t *view, const char *id, const char *label);
CXX_C_API int turbo_diagram_add_edge(json_value_t *view, const char *from,
                                    const char *to, const char *label);
/** Validate and render a view with at least one node as Mermaid text (no fence).
 * Clear out_text on entry; publish only on success. Return OK, EINVAL for bad
 * arguments, EPROTO for malformed views/text (including oversized text),
 * ENOSPC for node/edge/output bounds, or
 * ENOMEM. Preserve array order and map IDs to generated n0/n1/... identifiers.
 * Quote/escape labels; display CR/LF/tab as spaces, reject other ASCII controls.
 * No click callbacks, HTML labels, user styles, URLs or Mermaid directives are
 * emitted. Host applications can use the JSON IDs for their own navigation.
 * Example: turbo_diagram_mermaid(view, &text); check status; tstr_free(text).
 */
CXX_C_API int turbo_diagram_mermaid(const json_value_t *view, tstr *out_text);

typedef enum turbo_diagram_direction_e {
  TURBO_DIAGRAM_INCOMING,
  TURBO_DIAGRAM_OUTGOING,
  TURBO_DIAGRAM_BOTH
} turbo_diagram_direction_t;

/** Select an induced neighborhood around the exact source node ID focus_id.
 * Follow direction for at most max_hops (0..MAX_NODES-1); zero selects only the
 * focus. Include all edges between selected nodes, keeping original order and
 * metadata. Selected nodes receive selection_distance (minimum hops); root
 * selection records focus_id, direction, max_hops, input/omitted node/edge counts
 * and scope="input_view". A prior selection field is replaced; querying a
 * selection cannot discover anything outside that supplied view.
 *
 * Validate the entire graph before selection. Before cloning opaque metadata,
 * bound the input to MAX_QUERY_VALUES JSON values, MAX_QUERY_DEPTH nesting
 * (root depth zero) and MAX_QUERY_BYTES total key/string/number-token bytes.
 * These budgets are separate from Mermaid's escaped output-byte budget.
 * Borrow immutable input for the call; copy all output, release with json_free.
 * Clear out_view on entry and publish only on success; no input is mutated.
 * Return OK, EINVAL for arguments/direction/hops/invalid focus text, ENOENT for
 * an unknown focus, EPROTO for a malformed view, ENOSPC for budgets, or ENOMEM.
 * No reachability claim extends beyond the supplied snapshot. Cycles are valid.
 * Example: turbo_diagram_neighborhood(view, "step-1", TURBO_DIAGRAM_OUTGOING,
 *                                    2, &selection); check status; json_free(selection).
 */
CXX_C_API int turbo_diagram_neighborhood(const json_value_t *view, const char *focus_id,
    turbo_diagram_direction_t direction, size_t max_hops, json_value_t **out_view);

/** Select one minimum-hop path from focus_id to target_id within the input view.
 * Inherits neighborhood input validation, budgets, ownership and error rules.
 * ENOENT means either endpoint is unknown or no path exists within max_hops;
 * it says nothing about omitted records. Both IDs must be valid nonempty text.
 * Ties choose the first eligible source-array edge at each distance layer.
 * Copy only path nodes and predecessor edges, retaining source array order,
 * original edge directions and evidence. Node selection_distance is position
 * from the focus, even for incoming/both traversal. Equal IDs select one node
 * and zero edges, including at zero hops. Cycles and parallel edges are valid.
 * Root selection additionally records mode="shortest_path", target_id and
 * hop_count; other fields match neighborhood. No relation implies causality
 * or execution authority merely because it belongs to a connecting path.
 * Example: turbo_diagram_shortest_path(view, "a", "b", TURBO_DIAGRAM_OUTGOING,
 *                                     8, &path); check status; json_free(path).
 */
CXX_C_API int turbo_diagram_shortest_path(const json_value_t *view, const char *focus_id,
    const char *target_id, turbo_diagram_direction_t direction, size_t max_hops,
    json_value_t **out_view);
#ifdef __cplusplus
}
#endif
#endif
