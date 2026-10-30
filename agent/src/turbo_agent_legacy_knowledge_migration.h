#ifndef TURBO_AGENT_LEGACY_KNOWLEDGE_MIGRATION_H
#define TURBO_AGENT_LEGACY_KNOWLEDGE_MIGRATION_H

#include "turbo_agent_revision_generation.h"
#include "turbo_agent_source_revision_record.h"
#include <stddef.h>
#include <stdint.h>

#define TA_LEGACY_MIGRATION_FIELD_MAX_BYTES 4096u
#define TA_LEGACY_MIGRATION_CHUNK_MAX_BYTES (1024u * 1024u)

typedef struct ta_legacy_migration_text_s { const unsigned char *data; size_t size; } ta_legacy_migration_text_t;
typedef struct ta_legacy_migration_document_s {
  ta_legacy_migration_text_t id, uri, kind, title;
  int64_t mtime, size;
  ta_legacy_migration_text_t content_hash, metadata_json;
  int64_t updated_at;
} ta_legacy_migration_document_t;
typedef struct ta_legacy_migration_chunk_s {
  ta_legacy_migration_text_t id, document_id;
  int64_t ordinal;
  ta_legacy_migration_text_t text, metadata_json;
} ta_legacy_migration_chunk_t;
typedef struct ta_legacy_migration_id_s {
  unsigned char data[TA_REVISION_ID_MAX_BYTES];
  size_t size;
} ta_legacy_migration_id_t;
typedef enum ta_legacy_hash_shape_e {
  TA_LEGACY_HASH_OPAQUE_UNVERIFIED = 0,
  TA_LEGACY_HASH_HEX64_SHAPE_UNVERIFIED = 1
} ta_legacy_hash_shape_t;
typedef enum ta_legacy_migration_status_e {
  TA_LEGACY_MIGRATION_OK = 0,
  TA_LEGACY_MIGRATION_INVALID = 1,
  TA_LEGACY_MIGRATION_MALFORMED = 2,
  TA_LEGACY_MIGRATION_LIMIT = 3,
  TA_LEGACY_MIGRATION_REINGEST_REQUIRED = 4
} ta_legacy_migration_status_t;
typedef struct ta_legacy_migration_plan_s {
  ta_legacy_migration_id_t source_id, revision_id, extraction_id;
  ta_source_revision_record_t revision_record;
  ta_legacy_hash_shape_t legacy_hash_shape;
  uint64_t extracted_text_bytes;
  size_t chunk_count;
  const ta_legacy_migration_document_t *document;
  const ta_legacy_migration_chunk_t *chunks;
} ta_legacy_migration_plan_t;

/* Pure read-only planning. document.id is an opaque legacy row identity even
 * when path-shaped; uri is an alias, never an authority capability. The old
 * content_hash participates only in an opaque legacy revision identity and is
 * always recorded as UNVERIFIED_LEGACY, never SHA256_ORIGINAL. If the exact
 * identity cannot fit the private 256-byte ID bound, require re-ingestion
 * rather than hashing/truncating it. Existing FTS rows are not trusted; later
 * migration rebuilds FTS from structurally validated chunks.text. */
ta_legacy_migration_status_t ta_legacy_knowledge_plan_document(
    const ta_legacy_migration_document_t *document,
    const ta_legacy_migration_chunk_t *chunks, size_t chunk_count,
    ta_legacy_migration_plan_t *out);

#endif
