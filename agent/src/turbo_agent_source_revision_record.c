#include "turbo_agent_source_revision_record.h"

#include <string.h>

static const unsigned char magic[4] = {'T','A','R',2};

static int all_zero(const unsigned char digest[32]) {
  unsigned char accum = 0;
  size_t i;
  for (i = 0; i < 32u; ++i) accum = (unsigned char)(accum | digest[i]);
  return accum == 0;
}

static int valid_record(const ta_source_revision_record_t *r) {
  if (!r) return 0;
  if (r->digest_kind == TA_SOURCE_REVISION_DIGEST_UNVERIFIED_LEGACY)
    return r->retention == TA_SOURCE_REVISION_CONTENT_UNAVAILABLE && all_zero(r->sha256);
  if (r->digest_kind == TA_SOURCE_REVISION_DIGEST_SHA256_ORIGINAL)
    return r->retention == TA_SOURCE_REVISION_CONTENT_UNAVAILABLE ||
           r->retention == TA_SOURCE_REVISION_CONTENT_RETAINED_VERIFIED;
  return 0;
}

static void put_u64(unsigned char *p, uint64_t n) {
  unsigned i;
  for (i = 0; i < 8u; ++i) p[i] = (unsigned char)(n >> (56u - 8u * i));
}
static uint64_t get_u64(const unsigned char *p) {
  uint64_t n = 0;
  unsigned i;
  for (i = 0; i < 8u; ++i) n = (n << 8) | p[i];
  return n;
}

ta_source_revision_record_status_t ta_source_revision_record_encode(
    const ta_source_revision_record_t *record,
    ta_source_revision_record_payload_t *out) {
  if (!out) return TA_SOURCE_REVISION_RECORD_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_record(record)) return TA_SOURCE_REVISION_RECORD_INVALID;
  memcpy(out->data, magic, sizeof(magic));
  put_u64(out->data + 4u, record->content_length);
  out->data[12] = (unsigned char)record->digest_kind;
  out->data[13] = (unsigned char)record->retention;
  /* bytes 14..15 reserved and zero */
  put_u64(out->data + 16u, record->source_mtime_seconds);
  put_u64(out->data + 24u, record->observed_at_seconds);
  memcpy(out->data + 32u, record->sha256, 32u);
  out->size = sizeof(out->data);
  return TA_SOURCE_REVISION_RECORD_OK;
}

ta_source_revision_record_status_t ta_source_revision_record_decode(
    const unsigned char *data, size_t size,
    ta_source_revision_record_t *out) {
  ta_source_revision_record_t r;
  if (!out) return TA_SOURCE_REVISION_RECORD_INVALID;
  memset(out, 0, sizeof(*out));
  if (!data || size != TA_SOURCE_REVISION_RECORD_BYTES ||
      memcmp(data, magic, sizeof(magic)) || data[14] || data[15])
    return TA_SOURCE_REVISION_RECORD_MALFORMED;
  memset(&r, 0, sizeof(r));
  r.content_length = get_u64(data + 4u);
  r.digest_kind = (ta_source_revision_digest_kind_t)data[12];
  r.retention = (ta_source_revision_retention_t)data[13];
  r.source_mtime_seconds = get_u64(data + 16u);
  r.observed_at_seconds = get_u64(data + 24u);
  memcpy(r.sha256, data + 32u, 32u);
  if (!valid_record(&r)) return TA_SOURCE_REVISION_RECORD_MALFORMED;
  *out = r;
  return TA_SOURCE_REVISION_RECORD_OK;
}
