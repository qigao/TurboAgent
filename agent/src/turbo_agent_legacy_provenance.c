#include "turbo_agent_legacy_provenance.h"
#include <string.h>

enum { PROVENANCE_DOCUMENT = 1, PROVENANCE_CHUNK = 2 };
static const unsigned char magic[4] = {'T','A','P',1};
typedef struct writer_s {
  unsigned char *data;
  size_t size, used;
} writer_t;
typedef struct reader_s {
  const unsigned char *data;
  size_t size, used;
} reader_t;

static int enough_write(const writer_t *w, size_t n) {
  return w->used <= w->size && n <= w->size - w->used;
}
static int enough_read(const reader_t *r, size_t n) {
  return r->used <= r->size && n <= r->size - r->used;
}
static int put_byte(writer_t *w, unsigned char n) {
  if (!enough_write(w, 1u)) return 0;
  w->data[w->used++] = n;
  return 1;
}
static int put_u32(writer_t *w, uint32_t n) {
  if (!enough_write(w, 4u)) return 0;
  w->data[w->used++] = (unsigned char)(n >> 24);
  w->data[w->used++] = (unsigned char)(n >> 16);
  w->data[w->used++] = (unsigned char)(n >> 8);
  w->data[w->used++] = (unsigned char)n;
  return 1;
}
static int put_u64(writer_t *w, uint64_t n) {
  unsigned i;
  if (!enough_write(w, 8u)) return 0;
  for (i = 0; i < 8u; ++i)
    w->data[w->used++] = (unsigned char)(n >> (56u - 8u * i));
  return 1;
}
static int take_u32(reader_t *r, uint32_t *n) {
  unsigned i;
  if (!enough_read(r, 4u)) return 0;
  *n = 0;
  for (i = 0; i < 4u; ++i) *n = (*n << 8) | r->data[r->used++];
  return 1;
}
static int take_u64(reader_t *r, uint64_t *n) {
  unsigned i;
  if (!enough_read(r, 8u)) return 0;
  *n = 0;
  for (i = 0; i < 8u; ++i) *n = (*n << 8) | r->data[r->used++];
  return 1;
}
static int valid_field(ta_legacy_migration_text_t t, int required) {
  if (!t.data) return !required && !t.size;
  if (t.size > TA_LEGACY_MIGRATION_FIELD_MAX_BYTES ||
      (required && !t.size)) return 0;
  return !t.size || !memchr(t.data, 0, t.size);
}
static int put_field(writer_t *w, ta_legacy_migration_text_t t) {
  if (!t.data) return put_u32(w, UINT32_MAX);
  if (!enough_write(w, 4u + t.size) || !put_u32(w, (uint32_t)t.size))
    return 0;
  if (t.size) memcpy(w->data + w->used, t.data, t.size);
  w->used += t.size;
  return 1;
}
static int take_field(reader_t *r, int required,
                      ta_legacy_migration_text_t *out) {
  uint32_t n;
  *out = (ta_legacy_migration_text_t){NULL, 0};
  if (!take_u32(r, &n)) return 0;
  if (n == UINT32_MAX) return !required;
  if (n > TA_LEGACY_MIGRATION_FIELD_MAX_BYTES ||
      !enough_read(r, (size_t)n) || (required && !n)) return 0;
  out->data = r->data + r->used;
  out->size = (size_t)n;
  r->used += (size_t)n;
  return valid_field(*out, required);
}
static int put_header(writer_t *w, unsigned char kind) {
  size_t i;
  for (i = 0; i < sizeof(magic); ++i)
    if (!put_byte(w, magic[i])) return 0;
  return put_byte(w, kind);
}
static int take_header(reader_t *r, unsigned char kind) {
  if (!enough_read(r, sizeof(magic) + 1u) ||
      memcmp(r->data + r->used, magic, sizeof(magic)) ||
      r->data[r->used + sizeof(magic)] != kind) return 0;
  r->used += sizeof(magic) + 1u;
  return 1;
}
static int valid_document(const ta_legacy_migration_document_t *d) {
  return d && valid_field(d->id, 1) && valid_field(d->uri, 1) &&
      valid_field(d->kind, 1) && valid_field(d->title, 0) &&
      valid_field(d->content_hash, 1) && valid_field(d->metadata_json, 0) &&
      d->mtime >= 0 && d->size >= 0 && d->updated_at >= 0;
}
static int valid_chunk(const ta_legacy_migration_chunk_t *c) {
  return c && valid_field(c->id, 1) && valid_field(c->document_id, 1) &&
      valid_field(c->metadata_json, 0) && c->ordinal >= 0;
}

ta_legacy_provenance_status_t ta_legacy_provenance_document_encode(
    const ta_legacy_migration_document_t *d,
    ta_legacy_provenance_document_payload_t *out) {
  writer_t w;
  if (!out) return TA_LEGACY_PROVENANCE_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_document(d)) return TA_LEGACY_PROVENANCE_INVALID;
  w = (writer_t){out->data, sizeof(out->data), 0};
  if (!put_header(&w, PROVENANCE_DOCUMENT) ||
      !put_u64(&w, (uint64_t)d->mtime) ||
      !put_u64(&w, (uint64_t)d->size) ||
      !put_u64(&w, (uint64_t)d->updated_at) ||
      !put_field(&w, d->id) || !put_field(&w, d->uri) ||
      !put_field(&w, d->kind) || !put_field(&w, d->title) ||
      !put_field(&w, d->content_hash) || !put_field(&w, d->metadata_json)) {
    memset(out, 0, sizeof(*out));
    return TA_LEGACY_PROVENANCE_LIMIT;
  }
  out->size = w.used;
  return TA_LEGACY_PROVENANCE_OK;
}

ta_legacy_provenance_status_t ta_legacy_provenance_document_decode(
    const unsigned char *data, size_t size,
    ta_legacy_migration_document_t *out) {
  reader_t r;
  ta_legacy_migration_document_t d;
  uint64_t mtime, bytes, updated;
  if (!out) return TA_LEGACY_PROVENANCE_INVALID;
  memset(out, 0, sizeof(*out));
  if (!data || size > TA_LEGACY_PROVENANCE_DOCUMENT_MAX_BYTES)
    return TA_LEGACY_PROVENANCE_MALFORMED;
  r = (reader_t){data, size, 0};
  memset(&d, 0, sizeof(d));
  if (!take_header(&r, PROVENANCE_DOCUMENT) ||
      !take_u64(&r, &mtime) || !take_u64(&r, &bytes) ||
      !take_u64(&r, &updated) || mtime > INT64_MAX || bytes > INT64_MAX ||
      updated > INT64_MAX ||
      !take_field(&r, 1, &d.id) || !take_field(&r, 1, &d.uri) ||
      !take_field(&r, 1, &d.kind) || !take_field(&r, 0, &d.title) ||
      !take_field(&r, 1, &d.content_hash) ||
      !take_field(&r, 0, &d.metadata_json) || r.used != size)
    return TA_LEGACY_PROVENANCE_MALFORMED;
  d.mtime = (int64_t)mtime;
  d.size = (int64_t)bytes;
  d.updated_at = (int64_t)updated;
  *out = d;
  return TA_LEGACY_PROVENANCE_OK;
}

ta_legacy_provenance_status_t ta_legacy_provenance_chunk_encode(
    const ta_legacy_migration_chunk_t *c,
    ta_legacy_provenance_chunk_payload_t *out) {
  writer_t w;
  if (!out) return TA_LEGACY_PROVENANCE_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_chunk(c)) return TA_LEGACY_PROVENANCE_INVALID;
  w = (writer_t){out->data, sizeof(out->data), 0};
  if (!put_header(&w, PROVENANCE_CHUNK) ||
      !put_u64(&w, (uint64_t)c->ordinal) ||
      !put_field(&w, c->id) || !put_field(&w, c->document_id) ||
      !put_field(&w, c->metadata_json)) {
    memset(out, 0, sizeof(*out));
    return TA_LEGACY_PROVENANCE_LIMIT;
  }
  out->size = w.used;
  return TA_LEGACY_PROVENANCE_OK;
}

ta_legacy_provenance_status_t ta_legacy_provenance_chunk_decode(
    const unsigned char *data, size_t size,
    ta_legacy_migration_chunk_t *out) {
  reader_t r;
  ta_legacy_migration_chunk_t c;
  uint64_t ordinal;
  if (!out) return TA_LEGACY_PROVENANCE_INVALID;
  memset(out, 0, sizeof(*out));
  if (!data || size > TA_LEGACY_PROVENANCE_CHUNK_MAX_BYTES)
    return TA_LEGACY_PROVENANCE_MALFORMED;
  r = (reader_t){data, size, 0};
  memset(&c, 0, sizeof(c));
  if (!take_header(&r, PROVENANCE_CHUNK) ||
      !take_u64(&r, &ordinal) || ordinal > INT64_MAX ||
      !take_field(&r, 1, &c.id) || !take_field(&r, 1, &c.document_id) ||
      !take_field(&r, 0, &c.metadata_json) || r.used != size)
    return TA_LEGACY_PROVENANCE_MALFORMED;
  c.ordinal = (int64_t)ordinal;
  *out = c;
  return TA_LEGACY_PROVENANCE_OK;
}
