#ifndef TURBO_AGENT_LEGACY_KNOWLEDGE_BATCH_H
#define TURBO_AGENT_LEGACY_KNOWLEDGE_BATCH_H
#include "turbo_agent_legacy_knowledge_sqlite.h"
#include "turbo_agent_revision_write.h"

/* Private only. Owns copies from one completed pinned read-only SQLite scan.
 * Initialize to {0}; after capture use initial_request, then release. */
typedef struct ta_legacy_knowledge_batch_s { void *private_state; } ta_legacy_knowledge_batch_t;
typedef enum ta_legacy_batch_status_e {
  TA_LEGACY_BATCH_OK=0, TA_LEGACY_BATCH_INVALID=1,
  TA_LEGACY_BATCH_SOURCE_ERROR=2, TA_LEGACY_BATCH_LIMIT=3,
  TA_LEGACY_BATCH_DENIED=4, TA_LEGACY_BATCH_NO_MEMORY=5
} ta_legacy_batch_status_t;

/* Host must authorize from an independent principal/root/scope policy, never
 * from a legacy id or URI. Rejection aborts the ENTIRE snapshot. The callback
 * cannot establish durable revocation or target-store ACL correctness. */
typedef int (*ta_legacy_batch_host_admit_fn)(
    void *host_context, const ta_revision_scope_t *host_scope,
    const ta_legacy_migration_plan_t *source);

ta_legacy_batch_status_t ta_legacy_knowledge_batch_capture(
    sqlite3 *legacy_readonly, const ta_legacy_migration_sqlite_limits_t *limits,
    const ta_revision_write_budget_t *budget, const ta_revision_scope_t *host_scope,
    ta_legacy_batch_host_admit_fn admit, void *host_context,
    ta_legacy_knowledge_batch_t *out);

/* Initial cutover only: one complete generation with EXPECT_ABSENT head CAS.
 * Returned request borrows batch storage. The Host must obtain ONE qualified
 * TurboDB write owner, revalidate current grants and call the existing
 * ta_revision_write_publish_generation(); this API never opens/writes a DB. */
ta_legacy_batch_status_t ta_legacy_knowledge_batch_initial_request(
    const ta_legacy_knowledge_batch_t *batch, uint64_t generation_id,
    ta_revision_write_request_t *out_request);
void ta_legacy_knowledge_batch_release(ta_legacy_knowledge_batch_t *batch);
#endif
