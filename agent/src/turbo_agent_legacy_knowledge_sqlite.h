#ifndef TURBO_AGENT_LEGACY_KNOWLEDGE_SQLITE_H
#define TURBO_AGENT_LEGACY_KNOWLEDGE_SQLITE_H
#include "turbo_agent_legacy_knowledge_migration.h"
#include <sqlite3.h>

#define TA_LEGACY_SQLITE_MAX_DOCUMENTS 64u
#define TA_LEGACY_SQLITE_MAX_CHUNKS_PER_DOCUMENT 65536u
#define TA_LEGACY_SQLITE_MAX_COPIED_BYTES_PER_DOCUMENT (64u * 1024u * 1024u)
typedef struct ta_legacy_migration_sqlite_limits_s {
  uint32_t max_documents;
  uint32_t max_chunks_per_document;
  uint64_t max_copied_bytes_per_document;
} ta_legacy_migration_sqlite_limits_t;
typedef int (*ta_legacy_migration_plan_visitor_fn)(
    void *context, const ta_legacy_migration_plan_t *plan);
typedef enum ta_legacy_migration_sqlite_status_e {
  TA_LEGACY_SQLITE_OK = 0,
  TA_LEGACY_SQLITE_INVALID = 1,
  TA_LEGACY_SQLITE_SCHEMA = 2,
  TA_LEGACY_SQLITE_LIMIT = 3,
  TA_LEGACY_SQLITE_STORAGE_ERROR = 4,
  TA_LEGACY_SQLITE_BUSY = 5,
  TA_LEGACY_SQLITE_PLAN_ERROR = 6,
  TA_LEGACY_SQLITE_VISITOR_ABORTED = 7,
  /* Authoritative rows exist that are not reachable from the admitted document set. */
  TA_LEGACY_SQLITE_COVERAGE = 8
} ta_legacy_migration_sqlite_status_t;
/* Read-only scanner for the CURRENT legacy KnowledgeStore schema. Requires an
 * autocommit connection, pins one deferred read transaction, and never queries
 * chunks_fts. Row/plan views are borrowed only during the synchronous visitor. */
ta_legacy_migration_sqlite_status_t ta_legacy_knowledge_sqlite_plan_all(
    sqlite3 *db, const ta_legacy_migration_sqlite_limits_t *limits,
    ta_legacy_migration_plan_visitor_fn visitor, void *visitor_context,
    uint32_t *out_documents);
#endif
