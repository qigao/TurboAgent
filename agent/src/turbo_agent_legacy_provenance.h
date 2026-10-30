#ifndef TURBO_AGENT_LEGACY_PROVENANCE_H
#define TURBO_AGENT_LEGACY_PROVENANCE_H

#include "turbo_agent_legacy_knowledge_migration.h"

/* Private, byte-preserving storage payloads, NOT Host/FileCatalog authority.
 * Nullable SQL columns retain NULL versus present-empty distinction. */
#define TA_LEGACY_PROVENANCE_DOCUMENT_MAX_BYTES \
  (32u + 6u * (TA_LEGACY_MIGRATION_FIELD_MAX_BYTES + 4u))
#define TA_LEGACY_PROVENANCE_CHUNK_MAX_BYTES \
  (16u + 3u * (TA_LEGACY_MIGRATION_FIELD_MAX_BYTES + 4u))

typedef struct ta_legacy_provenance_document_payload_s {
  unsigned char data[TA_LEGACY_PROVENANCE_DOCUMENT_MAX_BYTES];
  size_t size;
} ta_legacy_provenance_document_payload_t;
typedef struct ta_legacy_provenance_chunk_payload_s {
  unsigned char data[TA_LEGACY_PROVENANCE_CHUNK_MAX_BYTES];
  size_t size;
} ta_legacy_provenance_chunk_payload_t;
typedef enum ta_legacy_provenance_status_e {
  TA_LEGACY_PROVENANCE_OK = 0,
  TA_LEGACY_PROVENANCE_INVALID = 1,
  TA_LEGACY_PROVENANCE_MALFORMED = 2,
  TA_LEGACY_PROVENANCE_LIMIT = 3
} ta_legacy_provenance_status_t;

/* Document payload includes exact id, uri, kind, nullable title, original
 * content_hash, nullable metadata_json, mtime, size and updated_at. */
ta_legacy_provenance_status_t ta_legacy_provenance_document_encode(
    const ta_legacy_migration_document_t *document,
    ta_legacy_provenance_document_payload_t *out);
ta_legacy_provenance_status_t ta_legacy_provenance_document_decode(
    const unsigned char *data, size_t size,
    ta_legacy_migration_document_t *out);

/* Chunk payload retains exact id, document_id, ordinal and nullable
 * metadata_json. The existing immutable Chunk KV stores exact chunks.text;
 * encoding it a second time here would create two competing sources of truth.
 * Decoders return borrowed views valid only for the input's lifetime. */
ta_legacy_provenance_status_t ta_legacy_provenance_chunk_encode(
    const ta_legacy_migration_chunk_t *chunk,
    ta_legacy_provenance_chunk_payload_t *out);
ta_legacy_provenance_status_t ta_legacy_provenance_chunk_decode(
    const unsigned char *data, size_t size,
    ta_legacy_migration_chunk_t *out);

#endif
