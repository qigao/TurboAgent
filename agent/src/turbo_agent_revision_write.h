#ifndef TURBO_AGENT_REVISION_WRITE_H
#define TURBO_AGENT_REVISION_WRITE_H

#include "turbo_agent_revision_generation.h"

#include <stddef.h>
#include <stdint.h>

/* Private bridge contract only. No public/install ABI and no direct SQLite API. */
#define TA_REVISION_WRITE_EXPECT_ABSENT UINT64_C(0)
#define TA_REVISION_WRITE_EXPECT_ANY UINT64_MAX
#define TA_REVISION_NS_SOURCE_REVISION "agent.index.source_revision"
#define TA_REVISION_NS_CHUNK "agent.index.chunk"
#define TA_REVISION_NS_GRAPH "agent.index.graph"
#define TA_REVISION_NS_GENERATION "agent.index.generation"

typedef struct ta_revision_blob_s {
  const unsigned char *data;
  size_t size;
} ta_revision_blob_t;

typedef enum ta_revision_write_backend_status_e {
  TA_REVISION_WRITE_BACKEND_OK = 0,
  TA_REVISION_WRITE_BACKEND_CONSTRAINT = 1,
  TA_REVISION_WRITE_BACKEND_LIMIT = 2,
  TA_REVISION_WRITE_BACKEND_BUSY = 3,
  TA_REVISION_WRITE_BACKEND_COMMIT_UNKNOWN = 4,
  TA_REVISION_WRITE_BACKEND_ERROR = 5
} ta_revision_write_backend_status_t;

typedef struct ta_revision_write_backend_ops_s {
  ta_revision_write_backend_status_t (*put)(
      void *write_owner, const char *ns, ta_revision_id_t key,
      ta_revision_blob_t value, uint64_t expected_head_seq);
  ta_revision_write_backend_status_t (*project_text)(
      void *write_owner, const char *ns, ta_revision_id_t key,
      ta_revision_blob_t text);
  ta_revision_write_backend_status_t (*publish)(
      void *write_owner, uint64_t *out_commit_seq);
  void (*abort)(void *write_owner); /* dispose only; never retry */
} ta_revision_write_backend_ops_t;

typedef struct ta_revision_write_backend_s {
  void *write_owner; /* already-open single write transaction; consumed by call */
  const ta_revision_write_backend_ops_t *ops;
} ta_revision_write_backend_t;

typedef struct ta_revision_write_chunk_s {
  ta_revision_blob_t payload;
  ta_revision_blob_t text; /* verified extracted text; no embedded NUL */
} ta_revision_write_chunk_t;

/* Only NEW/CHANGED immutable content is staged. Unchanged generation members
 * are inherited from previous_generation and MUST NOT be rewritten. */
typedef struct ta_revision_write_source_s {
  ta_revision_id_t source;
  ta_revision_id_t revision;
  ta_revision_id_t extraction;
  ta_revision_blob_t revision_metadata;
  const ta_revision_write_chunk_t *chunks;
  size_t chunk_count;
} ta_revision_write_source_t;

typedef struct ta_revision_write_edge_s {
  ta_revision_id_t from_source;
  ta_revision_id_t from_revision;
  ta_revision_id_t to_source;
  ta_revision_id_t to_revision;
  ta_revision_id_t relation;
  ta_revision_id_t edge_id;
  ta_revision_blob_t evidence;
} ta_revision_write_edge_t;

typedef struct ta_revision_write_budget_s {
  uint32_t max_chunks; /* newly staged chunks */
  uint32_t max_edges;  /* current-generation logical edges */
  uint64_t max_payload_bytes;
} ta_revision_write_budget_t;

typedef struct ta_revision_write_request_s {
  ta_revision_scope_t scope;
  uint64_t generation_id;
  uint64_t expected_generation_head_seq; /* 0=new, exact previous CAS; ANY rejected */

  /* Complete sorted membership for the generation being published. */
  const ta_revision_generation_member_t *members;
  size_t member_count;

  /* Exact sorted subset of members that are NEW/CHANGED from previous_generation. */
  const ta_revision_write_source_t *staged_sources;
  size_t staged_source_count;

  /* Required when expected_generation_head_seq != 0. It must be the complete
   * previously published manifest read before opening this writer. CAS on the
   * generation head prevents publishing against a concurrently replaced head.
   * Current-generation retention must keep its referenced immutable records. */
  ta_revision_blob_t previous_generation;

  const ta_revision_write_edge_t *edges; /* full derived graph for new generation */
  size_t edge_count;
  ta_revision_write_budget_t budget;
} ta_revision_write_request_t;

typedef enum ta_revision_write_status_e {
  TA_REVISION_WRITE_OK = 0,
  TA_REVISION_WRITE_INVALID = 1,
  TA_REVISION_WRITE_LIMIT = 2,
  TA_REVISION_WRITE_CONFLICT = 3,
  TA_REVISION_WRITE_BUSY = 4,
  TA_REVISION_WRITE_COMMIT_UNKNOWN = 5,
  TA_REVISION_WRITE_STORAGE_ERROR = 6,
  TA_REVISION_WRITE_BACKEND_CONTRACT = 7
} ta_revision_write_status_t;

typedef struct ta_revision_write_result_s {
  uint64_t generation_id;
  uint64_t commit_seq;
} ta_revision_write_result_t;

/* Consumes backend->write_owner on every return path. Request structure and
 * incremental membership are validated before the first backend mutation.
 * Immutable new SourceRevisions, chunks+typed FTS, BOTH generation-bound graph
 * directions and the complete generation head use ONE owner/one publish.
 * COMMIT_UNKNOWN is never replayed; inspect the original request_id using a
 * clean DB owner. */
ta_revision_write_status_t ta_revision_write_publish_generation(
    const ta_revision_write_backend_t *backend,
    const ta_revision_write_request_t *request,
    ta_revision_write_result_t *out_result);

#endif
