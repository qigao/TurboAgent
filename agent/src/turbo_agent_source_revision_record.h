#ifndef TURBO_AGENT_SOURCE_REVISION_RECORD_H
#define TURBO_AGENT_SOURCE_REVISION_RECORD_H

#include <stddef.h>
#include <stdint.h>

#define TA_SOURCE_REVISION_RECORD_BYTES 48u

typedef enum ta_source_revision_digest_kind_e {
  TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY = 0,
  TA_SOURCE_REVISION_DIGEST_SHA256_ORIGINAL = 1
} ta_source_revision_digest_kind_t;

typedef enum ta_source_revision_retention_e {
  TA_SOURCE_REVISION_CONTENT_UNAVAILABLE = 0,
  TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED = 1
} ta_source_revision_retention_t;

typedef struct ta_source_revision_record_s {
  uint64_t content_length;
  ta_source_revision_digest_kind_t digest_kind;
  ta_source_revision_retention_t retention;
  unsigned char sha256[32]; /* zero for UNVERIFIED_LEGACY */
} ta_source_revision_record_t;

typedef struct ta_source_revision_record_payload_s {
  unsigned char data[TA_SOURCE_REVISION_RECORD_BYTES];
  size_t size;
} ta_source_revision_record_payload_t;

typedef enum ta_source_revision_record_status_e {
  TA_SOURCE_REVISION_RECORD_OK = 0,
  TA_SOURCE_REVISION_RECORD_INVALID = 1,
  TA_SOURCE_REVISION_RECORD_MALFORMED = 2
} ta_source_revision_record_status_t;

/* This codec records digest SEMANTICS; it does not compute or verify SHA-256.
 * Host ingestion must hash the original bytes before using SHA256_ORIGINAL.
 * Legacy extracted-text hashes map to UNVERIFIED_LEGACY and may not claim
 * retained verified original content. */
ta_source_revision_record_status_t ta_source_revision_record_encode(
    const ta_source_revision_record_t *record,
    ta_source_revision_record_payload_t *out);
ta_source_revision_record_status_t ta_source_revision_record_decode(
    const unsigned char *data, size_t size,
    ta_source_revision_record_t *out);

#endif
