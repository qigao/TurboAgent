#include "turbo_leann_internal.h"

#include <math.h>
#include <string.h>

static int tl_better(const turbo_leann_hit_t *a, const turbo_leann_hit_t *b) {
  if (a->cosine_similarity > b->cosine_similarity) return 1;
  if (a->cosine_similarity < b->cosine_similarity) return 0;
  return memcmp(a->chunk_id, b->chunk_id, TURBO_LEANN_ID_BYTES) < 0;
}

turbo_leann_status_t turbo_leann_search_exact(
    const turbo_leann_segment_t *segment, const turbo_leann_query_t *query,
    turbo_leann_hit_t *out_hits, size_t hits_capacity, size_t *out_count) {
  if (!out_count) return TURBO_LEANN_INVALID;
  *out_count = 0;
  if (!segment || !query || !out_hits ||
      query->struct_size != sizeof(*query) ||
      query->abi_version != TURBO_LEANN_ABI_VERSION ||
      query->dimensions != segment->dimensions || !query->components ||
      query->top_k == 0 || query->top_k > hits_capacity ||
      query->top_k > 1024u) return TURBO_LEANN_INVALID;
  if (query->generation != segment->generation ||
      memcmp(query->model_fingerprint, segment->model, 32) ||
      memcmp(query->scope_fingerprint, segment->scope, 32))
    return TURBO_LEANN_SCOPE_MISMATCH;
  if (query->max_distance_evaluations < segment->count)
    return TURBO_LEANN_LIMIT;
  double norm = 0.0;
  if (!tl_valid_vector(query->components, query->dimensions, &norm))
    return TURBO_LEANN_INVALID;
  size_t used = 0;
  for (uint32_t i = 0; i < segment->count; ++i) {
    const float *v = segment->vectors+(size_t)i*segment->dimensions;
    double dot = 0.0;
    for (uint32_t j = 0; j < segment->dimensions; ++j)
      dot += (double)v[j] * (double)query->components[j];
    double score = dot / norm;
    if (score > 1.0) score = 1.0;
    if (score < -1.0) score = -1.0;
    turbo_leann_hit_t candidate;
    memcpy(candidate.chunk_id, segment->ids+(size_t)i*32, 16);
    memcpy(candidate.revision_id, segment->ids+(size_t)i*32+16, 16);
    candidate.cosine_similarity = (float)score;
    size_t pos = 0;
    while (pos < used && !tl_better(&candidate, &out_hits[pos])) ++pos;
    if (pos >= query->top_k) continue;
    if (used < query->top_k) ++used;
    for (size_t k = used-1; k > pos; --k) out_hits[k] = out_hits[k-1];
    out_hits[pos] = candidate;
  }
  *out_count = used;
  return TURBO_LEANN_OK;
}
