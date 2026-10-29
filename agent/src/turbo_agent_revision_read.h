#ifndef TURBO_AGENT_REVISION_READ_H
#define TURBO_AGENT_REVISION_READ_H

#include "turbo_agent_revision_generation.h"

#include <stddef.h>
#include <stdint.h>

/* Private Agent/TurboDB integration contract. It deliberately accepts an
 * already pinned read owner instead of opening a database or selecting a live
 * head. A future TurboDB bridge maps one turbodb_revision_read_t to this ABI.
 * This layer does not authorize principals and is not installed/public ABI. */
#define TA_REVISION_QUERY_MAX_BYTES 4096u
#define TA_REVISION_NS_GENERATION "agent.index.generation"
#define TA_REVISION_NS_CHUNK "agent.index.chunk"
#define TA_REVISION_NS_GRAPH "agent.index.graph"

typedef enum ta_revision_backend_status_e {
  TA_REVISION_BACKEND_OK = 0,
  TA_REVISION_BACKEND_NOT_FOUND = 1,
  TA_REVISION_BACKEND_LIMIT = 2,
  TA_REVISION_BACKEND_ERROR = 3
} ta_revision_backend_status_t;

typedef enum ta_revision_read_status_e {
  TA_REVISION_READ_OK = 0,
  TA_REVISION_READ_INVALID = 1,
  TA_REVISION_READ_NOT_FOUND = 2,
  TA_REVISION_READ_LIMIT = 3,
  TA_REVISION_READ_MALFORMED = 4,
  TA_REVISION_READ_SCOPE_MISMATCH = 5,
  TA_REVISION_READ_BACKEND_ERROR = 6,
  TA_REVISION_READ_BACKEND_CONTRACT = 7,
  TA_REVISION_READ_VISITOR_ABORTED = 8
} ta_revision_read_status_t;

typedef struct ta_revision_read_budget_s {
  uint32_t max_items;
  uint64_t max_result_bytes;
  uint64_t max_vm_steps;
} ta_revision_read_budget_t;

/* Value bytes stay valid until release(owner), when release is non-NULL.
 * A successful backend get/next must return a non-NULL byte pointer even for
 * a zero-length value when it uses an owner. The adapter never retains values
 * after the operation. */
typedef struct ta_revision_backend_value_s {
  ta_revision_id_t bytes;
  void *owner;
  void (*release)(void *owner);
} ta_revision_backend_value_t;

typedef struct ta_revision_read_backend_ops_s {
  ta_revision_backend_status_t (*get)(
      void *read_owner, const char *ns, ta_revision_id_t key,
      ta_revision_backend_value_t *out_value);
  ta_revision_backend_status_t (*open_search_text)(
      void *read_owner, const char *ns, ta_revision_id_t authorized_prefix,
      ta_revision_id_t query, const ta_revision_read_budget_t *budget,
      void **out_cursor);
  ta_revision_backend_status_t (*open_scan_prefix)(
      void *read_owner, const char *ns, ta_revision_id_t prefix,
      const ta_revision_read_budget_t *budget, void **out_cursor);
  ta_revision_backend_status_t (*cursor_next)(
      void *cursor, ta_revision_id_t *out_key,
      ta_revision_backend_value_t *out_value);
  void (*cursor_close)(void *cursor);
} ta_revision_read_backend_ops_t;

typedef struct ta_revision_read_backend_s {
  void *read_owner; /* caller-owned pinned snapshot; borrowed for the call */
  const ta_revision_read_backend_ops_t *ops;
} ta_revision_read_backend_t;

/* Visitor is synchronous. key and value are borrowed only for this callback.
 * Return zero to continue or nonzero to abort explicitly. */
typedef int (*ta_revision_read_visit_fn)(void *context,
                                         ta_revision_id_t key,
                                         ta_revision_id_t value);

/* Host must authorize (scope, source) before calling. The adapter reads the
 * generation record and performs FTS through the same read_owner. Missing,
 * empty, malformed or scope-mismatched membership never widens the query. The
 * exact selected revision/extractor prefix is passed to open_search_text(). */
ta_revision_read_status_t ta_revision_read_search_text(
    const ta_revision_read_backend_t *backend,
    const ta_revision_scope_t *authorized_scope,
    ta_revision_id_t authorized_source, ta_revision_id_t query,
    const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context,
    uint64_t *out_generation_id);

/* Traverse one exact relation and direction from the selected source revision.
 * Physical historical edges to endpoints outside the selected generation are
 * skipped; an item outside the requested binary prefix is a backend contract
 * violation. Every accepted edge has both endpoint revisions in the same
 * generation. This still does not authorize relation/evidence/tool usage. */
ta_revision_read_status_t ta_revision_read_scan_edges(
    const ta_revision_read_backend_t *backend,
    const ta_revision_scope_t *authorized_scope,
    ta_revision_id_t authorized_source, ta_revision_id_t relation,
    ta_revision_edge_direction_t direction,
    const ta_revision_read_budget_t *budget,
    ta_revision_read_visit_fn visitor, void *visitor_context,
    uint64_t *out_generation_id);

#endif
