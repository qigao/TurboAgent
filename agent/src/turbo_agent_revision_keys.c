#include "turbo_agent_revision_keys.h"

#include <string.h>

/* Wire prefix: TAI + format byte. Every identity is u16-BE length + bytes.
 * Lexicographic ordinal order equals numeric order via u64-BE encoding. */
static const unsigned char key_magic[4] = {'T', 'A', 'I', 1};
enum { KEY_CHUNK = 1, KEY_SOURCE_HEAD = 2, KEY_GENERATION = 3,
       KEY_EDGE_OUT = 4, KEY_EDGE_IN = 5, KEY_SOURCE_REVISION = 6,
       KEY_CATALOG_SOURCE = 7, KEY_CATALOG_PATH = 8 };

typedef struct key_reader_s {
  const unsigned char *data;
  size_t size;
  size_t pos;
} key_reader_t;

static ta_revision_key_status_t append_id(ta_revision_key_t *key,
                                           ta_revision_id_t id) {
  if (!id.data || !id.size) return TA_REVISION_KEY_INVALID;
  if (id.size > TA_REVISION_ID_MAX_BYTES) return TA_REVISION_KEY_LIMIT;
  if (key->size > sizeof(key->data) - 2u - id.size)
    return TA_REVISION_KEY_LIMIT;
  key->data[key->size++] = (unsigned char)(id.size >> 8);
  key->data[key->size++] = (unsigned char)(id.size & 255u);
  memcpy(key->data + key->size, id.data, id.size);
  key->size += id.size;
  return TA_REVISION_KEY_OK;
}

static ta_revision_key_status_t append_u64(ta_revision_key_t *key, uint64_t value) {
  unsigned i;
  if (key->size > sizeof(key->data) - 8u) return TA_REVISION_KEY_LIMIT;
  for (i = 0; i < 8; ++i)
    key->data[key->size++] = (unsigned char)(value >> (56u - 8u * i));
  return TA_REVISION_KEY_OK;
}

static ta_revision_key_status_t append_catalog_path(
    ta_revision_key_t *key, ta_revision_catalog_path_t path, int allow_empty) {
  if ((!path.data && path.size) || (!allow_empty && !path.size))
    return TA_REVISION_KEY_INVALID;
  if (path.size > TA_REVISION_CATALOG_PATH_MAX_BYTES)
    return TA_REVISION_KEY_LIMIT;
  if (key->size > sizeof(key->data) - path.size)
    return TA_REVISION_KEY_LIMIT;
  if (path.size) memcpy(key->data + key->size, path.data, path.size);
  key->size += path.size;
  return TA_REVISION_KEY_OK;
}

static ta_revision_key_status_t begin_key(ta_revision_key_t *key,
    const ta_revision_scope_t *scope, unsigned char kind) {
  ta_revision_key_status_t rc;
  if (!scope) return TA_REVISION_KEY_INVALID;
  memcpy(key->data, key_magic, sizeof(key_magic));
  key->size = sizeof(key_magic);
  rc = append_id(key, scope->authority);
  if (rc != TA_REVISION_KEY_OK) return rc;
  rc = append_id(key, scope->collection);
  if (rc != TA_REVISION_KEY_OK) return rc;
  key->data[key->size++] = kind;
  return TA_REVISION_KEY_OK;
}

static ta_revision_key_status_t finish_key(ta_revision_key_t *out,
    const ta_revision_key_t *built, ta_revision_key_status_t rc) {
  if (!out) return TA_REVISION_KEY_INVALID;
  memset(out, 0, sizeof(*out));
  if (rc == TA_REVISION_KEY_OK) {
    memcpy(out->data, built->data, built->size);
    out->size = built->size;
  }
  return rc;
}

static ta_revision_key_status_t chunk_base(const ta_revision_ref_t *ref,
                                          ta_revision_key_t *key) {
  ta_revision_key_status_t rc;
  if (!ref) return TA_REVISION_KEY_INVALID;
  rc = begin_key(key, &ref->scope, KEY_CHUNK);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, ref->source);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, ref->revision);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, ref->extraction);
  return rc;
}

ta_revision_key_status_t ta_revision_generation_key(
    const ta_revision_scope_t *scope, ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = begin_key(&key, scope, KEY_GENERATION);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_source_head_key(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = begin_key(&key, scope, KEY_SOURCE_HEAD);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(&key, source);
  return finish_key(out, &key, rc);
}

static ta_revision_key_status_t source_revision_base(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_key_t *key) {
  ta_revision_key_status_t rc = begin_key(key, scope, KEY_SOURCE_REVISION);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, source);
  return rc;
}

ta_revision_key_status_t ta_revision_source_revision_prefix(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = source_revision_base(scope, source, &key);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_source_revision_key(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_id_t revision, ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = source_revision_base(scope, source, &key);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(&key, revision);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_catalog_source_key(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = begin_key(&key, scope, KEY_CATALOG_SOURCE);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(&key, source);
  return finish_key(out, &key, rc);
}

static ta_revision_key_status_t catalog_path_base(
    const ta_revision_scope_t *scope, ta_revision_id_t root_id,
    ta_revision_key_t *key) {
  ta_revision_key_status_t rc = begin_key(key, scope, KEY_CATALOG_PATH);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, root_id);
  return rc;
}

ta_revision_key_status_t ta_revision_catalog_path_prefix(
    const ta_revision_scope_t *scope, ta_revision_id_t root_id,
    ta_revision_catalog_path_t normalized_path_prefix,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = catalog_path_base(scope, root_id, &key);
  if (rc == TA_REVISION_KEY_OK)
    rc = append_catalog_path(&key, normalized_path_prefix, 1);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_catalog_path_key(
    const ta_revision_scope_t *scope, ta_revision_id_t root_id,
    ta_revision_catalog_path_t normalized_path,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = catalog_path_base(scope, root_id, &key);
  if (rc == TA_REVISION_KEY_OK)
    rc = append_catalog_path(&key, normalized_path, 0);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_chunk_prefix(
    const ta_revision_ref_t *ref, ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = chunk_base(ref, &key);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_chunk_key(
    const ta_revision_ref_t *ref, uint64_t ordinal, ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = chunk_base(ref, &key);
  if (rc == TA_REVISION_KEY_OK) rc = append_u64(&key, ordinal);
  return finish_key(out, &key, rc);
}

static int read_id(key_reader_t *reader, ta_revision_id_t *id) {
  size_t n;
  if (reader->size - reader->pos < 2u) return 0;
  n = ((size_t)reader->data[reader->pos] << 8) |
      (size_t)reader->data[reader->pos + 1u];
  reader->pos += 2u;
  if (!n || n > TA_REVISION_ID_MAX_BYTES || n > reader->size - reader->pos)
    return 0;
  id->data = reader->data + reader->pos;
  id->size = n;
  reader->pos += n;
  return 1;
}

static int read_u64(key_reader_t *reader, uint64_t *out) {
  unsigned i;
  if (reader->size - reader->pos < 8u) return 0;
  *out = 0;
  for (i = 0; i < 8; ++i) *out = (*out << 8) | reader->data[reader->pos++];
  return 1;
}

static int read_start(ta_revision_id_t input, key_reader_t *reader,
                     ta_revision_scope_t *scope, unsigned char *kind) {
  if (!input.data || input.size < sizeof(key_magic) ||
      input.size > TA_REVISION_KEY_MAX_BYTES ||
      memcmp(input.data, key_magic, sizeof(key_magic))) return 0;
  reader->data = input.data;
  reader->size = input.size;
  reader->pos = sizeof(key_magic);
  if (!read_id(reader, &scope->authority) ||
      !read_id(reader, &scope->collection) || reader->pos == reader->size)
    return 0;
  *kind = reader->data[reader->pos++];
  return 1;
}

ta_revision_key_status_t ta_revision_chunk_decode(
    ta_revision_id_t encoded, ta_revision_ref_t *out_ref, uint64_t *out_ordinal) {
  ta_revision_ref_t ref = {{{NULL, 0}, {NULL, 0}}, {NULL, 0}, {NULL, 0}, {NULL, 0}};
  key_reader_t r = {NULL, 0, 0};
  uint64_t ordinal = 0;
  unsigned char kind = 0;
  if (out_ref) memset(out_ref, 0, sizeof(*out_ref));
  if (out_ordinal) *out_ordinal = 0;
  if (!out_ref || !out_ordinal) return TA_REVISION_KEY_INVALID;
  if (!read_start(encoded, &r, &ref.scope, &kind) || kind != KEY_CHUNK ||
      !read_id(&r, &ref.source) || !read_id(&r, &ref.revision) ||
      !read_id(&r, &ref.extraction) || !read_u64(&r, &ordinal) || r.pos != r.size)
    return TA_REVISION_KEY_MALFORMED;
  *out_ref = ref;
  *out_ordinal = ordinal;
  return TA_REVISION_KEY_OK;
}

static int id_equal(ta_revision_id_t a, ta_revision_id_t b) {
  return a.data && b.data && a.size && a.size == b.size &&
         a.size <= TA_REVISION_ID_MAX_BYTES && !memcmp(a.data, b.data, a.size);
}

int ta_revision_chunk_matches(ta_revision_id_t encoded,
                             const ta_revision_ref_t *expected) {
  ta_revision_ref_t actual;
  uint64_t ordinal;
  if (!expected || ta_revision_chunk_decode(encoded, &actual, &ordinal) !=
      TA_REVISION_KEY_OK) return 0;
  return id_equal(actual.scope.authority, expected->scope.authority) &&
         id_equal(actual.scope.collection, expected->scope.collection) &&
         id_equal(actual.source, expected->source) &&
         id_equal(actual.revision, expected->revision) &&
         id_equal(actual.extraction, expected->extraction);
}

static ta_revision_key_status_t edge_base(const ta_revision_scope_t *scope,
    ta_revision_edge_direction_t direction, uint64_t generation_id,
    ta_revision_id_t focus_source, ta_revision_id_t focus_revision,
    ta_revision_id_t relation, ta_revision_key_t *key) {
  ta_revision_key_status_t rc;
  unsigned char kind;
  if ((direction != TA_REVISION_EDGE_OUT && direction != TA_REVISION_EDGE_IN) ||
      !generation_id)
    return TA_REVISION_KEY_INVALID;
  kind = (unsigned char)(direction == TA_REVISION_EDGE_OUT ? KEY_EDGE_OUT : KEY_EDGE_IN);
  rc = begin_key(key, scope, kind);
  if (rc == TA_REVISION_KEY_OK) rc = append_u64(key, generation_id);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, focus_source);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, focus_revision);
  if (rc == TA_REVISION_KEY_OK) rc = append_id(key, relation);
  return rc;
}

ta_revision_key_status_t ta_revision_edge_prefix(
    const ta_revision_scope_t *scope, ta_revision_edge_direction_t direction,
    uint64_t generation_id, ta_revision_id_t focus_source,
    ta_revision_id_t focus_revision, ta_revision_id_t relation,
    ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = edge_base(scope, direction, generation_id,
                                         focus_source, focus_revision, relation, &key);
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_edge_key(const ta_revision_edge_t *edge,
    ta_revision_edge_direction_t direction, ta_revision_key_t *out) {
  ta_revision_key_t key = {{0}, 0};
  ta_revision_key_status_t rc = TA_REVISION_KEY_INVALID;
  if (edge) {
    const int outgoing = direction == TA_REVISION_EDGE_OUT;
    rc = edge_base(&edge->scope, direction, edge->generation_id,
        outgoing ? edge->from_source : edge->to_source,
        outgoing ? edge->from_revision : edge->to_revision, edge->relation, &key);
    if (rc == TA_REVISION_KEY_OK)
      rc = append_id(&key, outgoing ? edge->to_source : edge->from_source);
    if (rc == TA_REVISION_KEY_OK)
      rc = append_id(&key, outgoing ? edge->to_revision : edge->from_revision);
    if (rc == TA_REVISION_KEY_OK) rc = append_id(&key, edge->edge_id);
  }
  return finish_key(out, &key, rc);
}

ta_revision_key_status_t ta_revision_edge_decode(ta_revision_id_t encoded,
    ta_revision_edge_t *out_edge, ta_revision_edge_direction_t *out_direction) {
  ta_revision_edge_t edge;
  ta_revision_id_t focus, focus_rev, other, other_rev;
  key_reader_t r = {NULL, 0, 0};
  unsigned char kind = 0;
  memset(&edge, 0, sizeof(edge));
  if (out_edge) memset(out_edge, 0, sizeof(*out_edge));
  if (out_direction) *out_direction = (ta_revision_edge_direction_t)0;
  if (!out_edge || !out_direction) return TA_REVISION_KEY_INVALID;
  if (!read_start(encoded, &r, &edge.scope, &kind) ||
      (kind != KEY_EDGE_OUT && kind != KEY_EDGE_IN) ||
      !read_u64(&r, &edge.generation_id) || !edge.generation_id ||
      !read_id(&r, &focus) || !read_id(&r, &focus_rev) ||
      !read_id(&r, &edge.relation) || !read_id(&r, &other) ||
      !read_id(&r, &other_rev) || !read_id(&r, &edge.edge_id) || r.pos != r.size)
    return TA_REVISION_KEY_MALFORMED;
  edge.from_source = kind == KEY_EDGE_OUT ? focus : other;
  edge.from_revision = kind == KEY_EDGE_OUT ? focus_rev : other_rev;
  edge.to_source = kind == KEY_EDGE_OUT ? other : focus;
  edge.to_revision = kind == KEY_EDGE_OUT ? other_rev : focus_rev;
  *out_edge = edge;
  *out_direction = kind == KEY_EDGE_OUT ? TA_REVISION_EDGE_OUT : TA_REVISION_EDGE_IN;
  return TA_REVISION_KEY_OK;
}
