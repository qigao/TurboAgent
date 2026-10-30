#include "turbo_agent_revision_write.h"

#include <string.h>

static int valid_id(ta_revision_id_t id) {
  return id.data && id.size && id.size <= TA_REVISION_ID_MAX_BYTES;
}
static int id_equal(ta_revision_id_t a, ta_revision_id_t b) {
  return valid_id(a) && valid_id(b) && a.size == b.size &&
         !memcmp(a.data, b.data, a.size);
}
static int id_compare(ta_revision_id_t a, ta_revision_id_t b) {
  size_t n = a.size < b.size ? a.size : b.size;
  int rc = memcmp(a.data, b.data, n);
  return rc ? rc : (a.size > b.size) - (a.size < b.size);
}
static int member_equal(const ta_revision_generation_member_t *a,
                        const ta_revision_generation_member_t *b) {
  return a && b && id_equal(a->source, b->source) &&
         id_equal(a->revision, b->revision) &&
         id_equal(a->extraction, b->extraction) &&
         a->chunk_count == b->chunk_count;
}
static const ta_revision_generation_member_t *find_member(
    const ta_revision_generation_member_t *members, size_t count,
    ta_revision_id_t source) {
  size_t i;
  for (i = 0; i < count; ++i) {
    int cmp = id_compare(members[i].source, source);
    if (!cmp) return &members[i];
    if (cmp > 0) break;
  }
  return NULL;
}
static const ta_revision_write_source_t *find_staged(
    const ta_revision_write_source_t *sources, size_t count,
    ta_revision_id_t source) {
  size_t i;
  for (i = 0; i < count; ++i) {
    int cmp = id_compare(sources[i].source, source);
    if (!cmp) return &sources[i];
    if (cmp > 0) break;
  }
  return NULL;
}
static int valid_blob(ta_revision_blob_t b) { return b.data && b.size; }
static int valid_chunk_blob(ta_revision_blob_t b) { return b.data != NULL; }
static int valid_text(ta_revision_blob_t b) {
  return b.data && !memchr(b.data, 0, b.size);
}
static int valid_backend(const ta_revision_write_backend_t *b) {
  return b && b->write_owner && b->ops && b->ops->put && b->ops->project_text &&
         b->ops->publish && b->ops->abort;
}
static int add_bytes(uint64_t *total, size_t amount, uint64_t limit) {
  uint64_t n = (uint64_t)amount;
  if (n > UINT64_MAX - *total || *total + n > limit) return 0;
  *total += n;
  return 1;
}
static ta_revision_write_status_t stage_status(ta_revision_write_backend_status_t rc) {
  switch (rc) {
    case TA_REVISION_WRITE_BACKEND_OK: return TA_REVISION_WRITE_OK;
    case TA_REVISION_WRITE_BACKEND_CONSTRAINT: return TA_REVISION_WRITE_CONFLICT;
    case TA_REVISION_WRITE_BACKEND_LIMIT: return TA_REVISION_WRITE_LIMIT;
    case TA_REVISION_WRITE_BACKEND_BUSY: return TA_REVISION_WRITE_BUSY;
    case TA_REVISION_WRITE_BACKEND_COMMIT_UNKNOWN:
      return TA_REVISION_WRITE_BACKEND_CONTRACT; /* only publish may be uncertain */
    default: return TA_REVISION_WRITE_STORAGE_ERROR;
  }
}
static ta_revision_write_status_t publish_status(ta_revision_write_backend_status_t rc) {
  return rc == TA_REVISION_WRITE_BACKEND_COMMIT_UNKNOWN ?
      TA_REVISION_WRITE_COMMIT_UNKNOWN : stage_status(rc);
}
static ta_revision_write_status_t abort_with(const ta_revision_write_backend_t *b,
                                             ta_revision_write_status_t rc) {
  b->ops->abort(b->write_owner);
  return rc;
}

static ta_revision_write_status_t validate_request(
    const ta_revision_write_request_t *r,
    ta_revision_generation_payload_t *manifest,
    ta_revision_generation_view_t *previous_view) {
  uint64_t bytes = 0, chunks = 0;
  size_t i, j, expected_staged = 0;
  ta_revision_key_t key;
  ta_revision_generation_status_t grc;

  memset(manifest, 0, sizeof(*manifest));
  memset(previous_view, 0, sizeof(*previous_view));
  if (!r || !r->generation_id ||
      r->expected_generation_head_seq == TA_REVISION_WRITE_EXPECT_ANY ||
      r->member_count > TA_REVISION_GENERATION_MAX_SOURCES ||
      (r->member_count && !r->members) ||
      (r->staged_source_count && !r->staged_sources) ||
      (r->edge_count && !r->edges) ||
      !r->budget.max_chunks || !r->budget.max_edges || !r->budget.max_payload_bytes ||
      r->edge_count > r->budget.max_edges)
    return TA_REVISION_WRITE_INVALID;

  if (!r->expected_generation_head_seq) {
    if (r->previous_generation.data || r->previous_generation.size)
      return TA_REVISION_WRITE_INVALID;
  } else {
    if (!valid_blob(r->previous_generation)) return TA_REVISION_WRITE_INVALID;
    grc = ta_revision_generation_decode(
        (ta_revision_id_t){r->previous_generation.data, r->previous_generation.size},
        &r->scope, previous_view);
    if (grc == TA_REVISION_GENERATION_LIMIT) return TA_REVISION_WRITE_LIMIT;
    if (grc != TA_REVISION_GENERATION_OK ||
        r->generation_id <= previous_view->generation_id)
      return TA_REVISION_WRITE_INVALID;
  }

  grc = ta_revision_generation_encode(&r->scope, r->generation_id, r->members,
                                      r->member_count, manifest);
  if (grc == TA_REVISION_GENERATION_LIMIT) return TA_REVISION_WRITE_LIMIT;
  if (grc != TA_REVISION_GENERATION_OK) return TA_REVISION_WRITE_INVALID;

  if (ta_revision_generation_key(&r->scope, &key) != TA_REVISION_KEY_OK ||
      !add_bytes(&bytes, key.size, r->budget.max_payload_bytes) ||
      !add_bytes(&bytes, manifest->size, r->budget.max_payload_bytes))
    return TA_REVISION_WRITE_LIMIT;

  /* Every new/changed member is staged exactly once; unchanged members are
   * inherited from the previous current generation and never rewritten. */
  for (i = 0; i < r->member_count; ++i) {
    const ta_revision_generation_member_t *m = &r->members[i];
    const ta_revision_generation_member_t *old = previous_view->generation_id ?
        find_member(previous_view->members, previous_view->member_count, m->source) : NULL;
    const ta_revision_write_source_t *staged =
        find_staged(r->staged_sources, r->staged_source_count, m->source);
    int unchanged = old && member_equal(old, m);
    if (unchanged) {
      if (staged) return TA_REVISION_WRITE_INVALID;
      continue;
    }
    ++expected_staged;
    if (!staged || !id_equal(staged->revision, m->revision) ||
        !id_equal(staged->extraction, m->extraction) ||
        staged->chunk_count != (size_t)m->chunk_count ||
        (staged->chunk_count && !staged->chunks))
      return TA_REVISION_WRITE_INVALID;
    ta_source_revision_record_payload_t revision_record_payload;
    if (ta_source_revision_record_encode(&staged->revision_record,
                                         &revision_record_payload) !=
        TA_SOURCE_REVISION_RECORD_OK)
      return TA_REVISION_WRITE_INVALID;
    if (m->chunk_count > r->budget.max_chunks - chunks)
      return TA_REVISION_WRITE_LIMIT;
    chunks += m->chunk_count;
    if (ta_revision_source_revision_key(&r->scope, m->source, m->revision, &key) !=
        TA_REVISION_KEY_OK ||
        !add_bytes(&bytes, key.size, r->budget.max_payload_bytes) ||
        !add_bytes(&bytes, revision_record_payload.size, r->budget.max_payload_bytes))
      return TA_REVISION_WRITE_LIMIT;
    for (j = 0; j < staged->chunk_count; ++j) {
      ta_revision_ref_t ref = {r->scope, m->source, m->revision, m->extraction};
      const ta_revision_write_chunk_t *chunk = &staged->chunks[j];
      if (!valid_chunk_blob(chunk->payload) || !valid_text(chunk->text) ||
          ta_revision_chunk_key(&ref, (uint64_t)j, &key) != TA_REVISION_KEY_OK)
        return TA_REVISION_WRITE_INVALID;
      if (!add_bytes(&bytes, key.size, r->budget.max_payload_bytes) ||
          !add_bytes(&bytes, chunk->payload.size, r->budget.max_payload_bytes) ||
          !add_bytes(&bytes, chunk->text.size, r->budget.max_payload_bytes))
        return TA_REVISION_WRITE_LIMIT;
    }
  }
  if (expected_staged != r->staged_source_count) return TA_REVISION_WRITE_INVALID;
  for (i = 1; i < r->staged_source_count; ++i)
    if (id_compare(r->staged_sources[i-1].source, r->staged_sources[i].source) >= 0)
      return TA_REVISION_WRITE_INVALID;
  for (i = 0; i < r->staged_source_count; ++i)
    if (!find_member(r->members, r->member_count, r->staged_sources[i].source))
      return TA_REVISION_WRITE_INVALID;

  for (i = 0; i < r->edge_count; ++i) {
    const ta_revision_write_edge_t *src = &r->edges[i];
    ta_revision_edge_t edge;
    ta_revision_key_t out_key, in_key;
    int member = 0;
    if (!valid_id(src->from_source) || !valid_id(src->from_revision) ||
        !valid_id(src->to_source) || !valid_id(src->to_revision) ||
        !valid_id(src->relation) || !valid_id(src->edge_id) || !valid_blob(src->evidence))
      return TA_REVISION_WRITE_INVALID;
    edge = (ta_revision_edge_t){r->scope, r->generation_id, src->from_source,
        src->from_revision, src->to_source, src->to_revision, src->relation, src->edge_id};
    if (ta_revision_edge_key(&edge, TA_REVISION_EDGE_OUT, &out_key) != TA_REVISION_KEY_OK ||
        ta_revision_edge_key(&edge, TA_REVISION_EDGE_IN, &in_key) != TA_REVISION_KEY_OK ||
        ta_revision_generation_accept_edge((ta_revision_id_t){manifest->data, manifest->size},
          &r->scope, (ta_revision_id_t){out_key.data, out_key.size}, &member) !=
          TA_REVISION_GENERATION_OK || !member)
      return TA_REVISION_WRITE_INVALID;
    if (!add_bytes(&bytes, out_key.size, r->budget.max_payload_bytes) ||
        !add_bytes(&bytes, in_key.size, r->budget.max_payload_bytes) ||
        !add_bytes(&bytes, src->evidence.size, r->budget.max_payload_bytes) ||
        !add_bytes(&bytes, src->evidence.size, r->budget.max_payload_bytes))
      return TA_REVISION_WRITE_LIMIT;
  }
  return TA_REVISION_WRITE_OK;
}

ta_revision_write_status_t ta_revision_write_publish_generation(
    const ta_revision_write_backend_t *backend, const ta_revision_write_request_t *r,
    ta_revision_write_result_t *out) {
  ta_revision_generation_payload_t manifest;
  ta_revision_generation_view_t previous;
  ta_revision_write_status_t status;
  ta_revision_write_backend_status_t brc;
  uint64_t commit_seq = 0;
  size_t i, j;
  ta_revision_key_t key;

  if (out) memset(out, 0, sizeof(*out));
  if (!valid_backend(backend) || !out) return TA_REVISION_WRITE_INVALID;
  status = validate_request(r, &manifest, &previous);
  if (status != TA_REVISION_WRITE_OK) return abort_with(backend, status);

  for (i = 0; i < r->staged_source_count; ++i) {
    const ta_revision_write_source_t *src = &r->staged_sources[i];
    ta_source_revision_record_payload_t revision_record_payload;
    if (ta_revision_source_revision_key(&r->scope, src->source, src->revision, &key) != TA_REVISION_KEY_OK ||
        ta_source_revision_record_encode(&src->revision_record, &revision_record_payload) !=
          TA_SOURCE_REVISION_RECORD_OK)
      return abort_with(backend, TA_REVISION_WRITE_INVALID);
    brc = backend->ops->put(backend->write_owner, TA_REVISION_NS_SOURCE_REVISION,
        (ta_revision_id_t){key.data, key.size},
        (ta_revision_blob_t){revision_record_payload.data, revision_record_payload.size},
        TA_REVISION_WRITE_EXPECT_ABSENT);
    if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));
    for (j = 0; j < src->chunk_count; ++j) {
      ta_revision_ref_t ref = {r->scope, src->source, src->revision, src->extraction};
      const ta_revision_write_chunk_t *chunk = &src->chunks[j];
      if (ta_revision_chunk_key(&ref, (uint64_t)j, &key) != TA_REVISION_KEY_OK)
        return abort_with(backend, TA_REVISION_WRITE_INVALID);
      brc = backend->ops->put(backend->write_owner, TA_REVISION_NS_CHUNK,
          (ta_revision_id_t){key.data, key.size}, chunk->payload, TA_REVISION_WRITE_EXPECT_ABSENT);
      if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));
      if (chunk->text.size) {
        brc = backend->ops->project_text(backend->write_owner, TA_REVISION_NS_CHUNK,
            (ta_revision_id_t){key.data, key.size}, chunk->text);
        if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));
      }
    }
  }

  for (i = 0; i < r->edge_count; ++i) {
    const ta_revision_write_edge_t *src = &r->edges[i];
    ta_revision_edge_t edge = {r->scope, r->generation_id, src->from_source,
        src->from_revision, src->to_source, src->to_revision, src->relation, src->edge_id};
    ta_revision_key_t out_key, in_key;
    if (ta_revision_edge_key(&edge, TA_REVISION_EDGE_OUT, &out_key) != TA_REVISION_KEY_OK ||
        ta_revision_edge_key(&edge, TA_REVISION_EDGE_IN, &in_key) != TA_REVISION_KEY_OK)
      return abort_with(backend, TA_REVISION_WRITE_INVALID);
    brc = backend->ops->put(backend->write_owner, TA_REVISION_NS_GRAPH,
        (ta_revision_id_t){out_key.data, out_key.size}, src->evidence, TA_REVISION_WRITE_EXPECT_ABSENT);
    if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));
    brc = backend->ops->put(backend->write_owner, TA_REVISION_NS_GRAPH,
        (ta_revision_id_t){in_key.data, in_key.size}, src->evidence, TA_REVISION_WRITE_EXPECT_ABSENT);
    if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));
  }

  if (ta_revision_generation_key(&r->scope, &key) != TA_REVISION_KEY_OK)
    return abort_with(backend, TA_REVISION_WRITE_INVALID);
  brc = backend->ops->put(backend->write_owner, TA_REVISION_NS_GENERATION,
      (ta_revision_id_t){key.data, key.size}, (ta_revision_blob_t){manifest.data, manifest.size},
      r->expected_generation_head_seq);
  if (brc != TA_REVISION_WRITE_BACKEND_OK) return abort_with(backend, stage_status(brc));

  brc = backend->ops->publish(backend->write_owner, &commit_seq);
  if (brc != TA_REVISION_WRITE_BACKEND_OK) {
    status = publish_status(brc);
    backend->ops->abort(backend->write_owner);
    return status;
  }
  if (!commit_seq) return TA_REVISION_WRITE_BACKEND_CONTRACT;
  out->generation_id = r->generation_id;
  out->commit_seq = commit_seq;
  return TA_REVISION_WRITE_OK;
}


ta_revision_reconcile_status_t ta_revision_write_reconcile_request(
    const ta_revision_inspect_backend_t *backend, ta_revision_blob_t request_id,
    uint64_t *out_commit_seq) {
  uint64_t seq = 0;
  ta_revision_inspect_backend_status_t rc;
  if (out_commit_seq) *out_commit_seq = 0;
  if (!backend || !backend->clean_owner || !backend->ops ||
      !backend->ops->inspect_request || !out_commit_seq ||
      !request_id.data || !request_id.size ||
      request_id.size > TA_REVISION_WRITE_REQUEST_ID_MAX_BYTES)
    return TA_REVISION_RECONCILE_INVALID;
  rc = backend->ops->inspect_request(backend->clean_owner, request_id, &seq);
  if (rc == TA_REVISION_INSPECT_BACKEND_NOT_FOUND)
    return TA_REVISION_RECONCILE_NOT_COMMITTED;
  if (rc != TA_REVISION_INSPECT_BACKEND_OK)
    return TA_REVISION_RECONCILE_BACKEND_ERROR;
  if (!seq) return TA_REVISION_RECONCILE_BACKEND_CONTRACT;
  *out_commit_seq = seq;
  return TA_REVISION_RECONCILE_COMMITTED;
}
