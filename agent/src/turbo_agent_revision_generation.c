#include "turbo_agent_revision_generation.h"

#include <string.h>

/* TAG1 | generation:u64be | authority:id | collection:id | count:u16be |
 * count * (source:id | revision:id | extraction:id | chunks:u64be).
 * id is a nonempty u16be length followed by opaque bytes. */
static const unsigned char generation_magic[4] = {'T', 'A', 'G', 1};
typedef struct generation_reader_s {
  const unsigned char *data;
  size_t size, pos;
} generation_reader_t;

static int valid_id(ta_revision_id_t id) {
  return id.data && id.size && id.size <= TA_REVISION_ID_MAX_BYTES;
}
static int valid_scope(const ta_revision_scope_t *s) {
  return s && valid_id(s->authority) && valid_id(s->collection);
}
static int compare_id(ta_revision_id_t a, ta_revision_id_t b) {
  size_t n = a.size < b.size ? a.size : b.size;
  int cmp = memcmp(a.data, b.data, n);
  return cmp ? cmp : (a.size > b.size) - (a.size < b.size);
}
static int same_scope(const ta_revision_scope_t *a, const ta_revision_scope_t *b) {
  return !compare_id(a->authority, b->authority) &&
         !compare_id(a->collection, b->collection);
}
static void put_u16(ta_revision_generation_payload_t *out, size_t n) {
  out->data[out->size++] = (unsigned char)(n >> 8);
  out->data[out->size++] = (unsigned char)n;
}
static void put_u64(ta_revision_generation_payload_t *out, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    out->data[out->size++] = (unsigned char)(n >> (56u - 8u * i));
}
static void put_id(ta_revision_generation_payload_t *out, ta_revision_id_t id) {
  put_u16(out, id.size);
  memcpy(out->data + out->size, id.data, id.size);
  out->size += id.size;
}
static int take_u64(generation_reader_t *r, uint64_t *out) {
  if (r->size - r->pos < 8u) return 0;
  *out = 0;
  for (unsigned i = 0; i < 8; ++i) *out = (*out << 8) | r->data[r->pos++];
  return 1;
}
static int take_id(generation_reader_t *r, ta_revision_id_t *out) {
  if (r->size - r->pos < 2u) return 0;
  size_t n = ((size_t)r->data[r->pos] << 8) | r->data[r->pos + 1u];
  r->pos += 2u;
  if (!n || n > TA_REVISION_ID_MAX_BYTES || n > r->size - r->pos) return 0;
  out->data = r->data + r->pos;
  out->size = n;
  r->pos += n;
  return 1;
}

ta_revision_generation_status_t ta_revision_generation_encode(
    const ta_revision_scope_t *scope, uint64_t generation_id,
    const ta_revision_generation_member_t *members, size_t count,
    ta_revision_generation_payload_t *out) {
  if (!out) return TA_REVISION_GENERATION_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_scope(scope) || !generation_id || (count && !members))
    return TA_REVISION_GENERATION_INVALID;
  if (count > TA_REVISION_GENERATION_MAX_SOURCES) return TA_REVISION_GENERATION_LIMIT;
  size_t needed = 4u + 8u + 2u + 4u + scope->authority.size + scope->collection.size;
  for (size_t i = 0; i < count; ++i) {
    const ta_revision_generation_member_t *m = &members[i];
    if (!valid_id(m->source) || !valid_id(m->revision) || !valid_id(m->extraction))
      return TA_REVISION_GENERATION_INVALID;
    if (m->chunk_count > TA_REVISION_GENERATION_MAX_CHUNKS)
      return TA_REVISION_GENERATION_LIMIT;
    if (i && compare_id(members[i - 1u].source, m->source) >= 0)
      return TA_REVISION_GENERATION_INVALID;
    size_t n = 6u + 8u + m->source.size + m->revision.size + m->extraction.size;
    if (n > TA_REVISION_GENERATION_MAX_BYTES - needed)
      return TA_REVISION_GENERATION_LIMIT;
    needed += n;
  }
  /* All writes are infallible within the prevalidated fixed-size buffer. */
  memcpy(out->data, generation_magic, sizeof(generation_magic));
  out->size = sizeof(generation_magic);
  put_u64(out, generation_id);
  put_id(out, scope->authority);
  put_id(out, scope->collection);
  put_u16(out, count);
  for (size_t i = 0; i < count; ++i) {
    put_id(out, members[i].source);
    put_id(out, members[i].revision);
    put_id(out, members[i].extraction);
    put_u64(out, members[i].chunk_count);
  }
  return TA_REVISION_GENERATION_OK;
}

ta_revision_generation_status_t ta_revision_generation_decode(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_generation_view_t *out) {
  if (!out) return TA_REVISION_GENERATION_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_scope(expected_scope)) return TA_REVISION_GENERATION_INVALID;
  if (encoded.size > TA_REVISION_GENERATION_MAX_BYTES) return TA_REVISION_GENERATION_LIMIT;
  if (!encoded.data || encoded.size < sizeof(generation_magic) ||
      memcmp(encoded.data, generation_magic, sizeof(generation_magic)))
    return TA_REVISION_GENERATION_MALFORMED;
  generation_reader_t r = {encoded.data, encoded.size, sizeof(generation_magic)};
  ta_revision_generation_view_t tmp = {0};
  if (!take_u64(&r, &tmp.generation_id) || !tmp.generation_id ||
      !take_id(&r, &tmp.scope.authority) || !take_id(&r, &tmp.scope.collection) ||
      r.size - r.pos < 2u) return TA_REVISION_GENERATION_MALFORMED;
  tmp.member_count = ((size_t)r.data[r.pos] << 8) | r.data[r.pos + 1u];
  r.pos += 2u;
  if (tmp.member_count > TA_REVISION_GENERATION_MAX_SOURCES)
    return TA_REVISION_GENERATION_LIMIT;
  for (size_t i = 0; i < tmp.member_count; ++i) {
    ta_revision_generation_member_t *m = &tmp.members[i];
    if (!take_id(&r, &m->source) || !take_id(&r, &m->revision) ||
        !take_id(&r, &m->extraction) || !take_u64(&r, &m->chunk_count))
      return TA_REVISION_GENERATION_MALFORMED;
    if (m->chunk_count > TA_REVISION_GENERATION_MAX_CHUNKS)
      return TA_REVISION_GENERATION_LIMIT;
    if (i && compare_id(tmp.members[i - 1u].source, m->source) >= 0)
      return TA_REVISION_GENERATION_MALFORMED;
  }
  if (r.pos != r.size) return TA_REVISION_GENERATION_MALFORMED;
  if (!same_scope(&tmp.scope, expected_scope)) return TA_REVISION_GENERATION_SCOPE_MISMATCH;
  *out = tmp;
  return TA_REVISION_GENERATION_OK;
}

static const ta_revision_generation_member_t *find_member(
    const ta_revision_generation_view_t *view, ta_revision_id_t source) {
  for (size_t i = 0; i < view->member_count; ++i) {
    int cmp = compare_id(view->members[i].source, source);
    if (!cmp) return &view->members[i];
    if (cmp > 0) break;
  }
  return NULL;
}

ta_revision_generation_status_t ta_revision_generation_resolve(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t source, ta_revision_generation_selection_t *out) {
  if (!out) return TA_REVISION_GENERATION_INVALID;
  memset(out, 0, sizeof(*out));
  if (!valid_id(source)) return TA_REVISION_GENERATION_INVALID;
  ta_revision_generation_view_t view;
  ta_revision_generation_status_t rc = ta_revision_generation_decode(encoded, expected_scope, &view);
  if (rc != TA_REVISION_GENERATION_OK) return rc;
  const ta_revision_generation_member_t *m = find_member(&view, source);
  if (!m) return TA_REVISION_GENERATION_NOT_FOUND;
  ta_revision_ref_t ref = {view.scope, m->source, m->revision, m->extraction};
  if (ta_revision_chunk_prefix(&ref, &out->chunk_prefix) != TA_REVISION_KEY_OK)
    return TA_REVISION_GENERATION_MALFORMED;
  out->generation_id = view.generation_id;
  out->ref = ref;
  out->chunk_count = m->chunk_count;
  return TA_REVISION_GENERATION_OK;
}

ta_revision_generation_status_t ta_revision_generation_accept_chunk(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t chunk_key, int *out_matches) {
  if (!out_matches) return TA_REVISION_GENERATION_INVALID;
  *out_matches = 0;
  ta_revision_generation_view_t view;
  ta_revision_generation_status_t rc = ta_revision_generation_decode(encoded, expected_scope, &view);
  if (rc != TA_REVISION_GENERATION_OK) return rc;
  ta_revision_ref_t actual;
  uint64_t ordinal = 0;
  if (ta_revision_chunk_decode(chunk_key, &actual, &ordinal) != TA_REVISION_KEY_OK)
    return TA_REVISION_GENERATION_MALFORMED;
  if (!same_scope(&view.scope, &actual.scope)) return TA_REVISION_GENERATION_OK;
  const ta_revision_generation_member_t *m = find_member(&view, actual.source);
  if (m) *out_matches = !compare_id(m->revision, actual.revision) &&
                       !compare_id(m->extraction, actual.extraction) && ordinal < m->chunk_count;
  return TA_REVISION_GENERATION_OK;
}

ta_revision_generation_status_t ta_revision_generation_accept_edge(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t edge_key, int *out_matches) {
  if (!out_matches) return TA_REVISION_GENERATION_INVALID;
  *out_matches = 0;
  ta_revision_generation_view_t view;
  ta_revision_generation_status_t rc = ta_revision_generation_decode(encoded, expected_scope, &view);
  if (rc != TA_REVISION_GENERATION_OK) return rc;
  ta_revision_edge_t edge;
  ta_revision_edge_direction_t dir;
  if (ta_revision_edge_decode(edge_key, &edge, &dir) != TA_REVISION_KEY_OK)
    return TA_REVISION_GENERATION_MALFORMED;
  if (!same_scope(&view.scope, &edge.scope)) return TA_REVISION_GENERATION_OK;
  const ta_revision_generation_member_t *from = find_member(&view, edge.from_source);
  const ta_revision_generation_member_t *to = find_member(&view, edge.to_source);
  if (from && to) *out_matches = !compare_id(from->revision, edge.from_revision) &&
                                !compare_id(to->revision, edge.to_revision);
  return TA_REVISION_GENERATION_OK;
}
