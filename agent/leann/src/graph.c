#include "turbo_leann_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TL_MAX_DIMENSIONS 4096u
#define TL_MAX_NODES 65535u

typedef struct tl_input_s {
  const turbo_leann_vector_t *vector;
} tl_input_t;

int tl_checked_add(size_t a, size_t b, size_t *out) {
  if (a > SIZE_MAX - b) return 0;
  *out = a + b;
  return 1;
}

int tl_checked_mul(size_t a, size_t b, size_t *out) {
  if (a != 0 && b > SIZE_MAX / a) return 0;
  *out = a * b;
  return 1;
}

int tl_valid_fingerprint(const uint8_t *fingerprint) {
  if (!fingerprint) return 0;
  uint8_t any = 0;
  for (size_t i = 0; i < TURBO_LEANN_FINGERPRINT_BYTES; ++i)
    any |= fingerprint[i];
  return any != 0;
}

int tl_valid_vector(const float *components, uint32_t dims, double *out_norm) {
  if (!components || dims == 0 || dims > TL_MAX_DIMENSIONS || !out_norm)
    return 0;
  double sum = 0.0;
  for (uint32_t j = 0; j < dims; ++j) {
    double v = components[j];
    if (!isfinite(v)) return 0;
    sum += v * v;
  }
  if (!(sum > 0.0) || !isfinite(sum)) return 0;
  *out_norm = sqrt(sum);
  return isfinite(*out_norm) && *out_norm > 0.0;
}

void tl_release(turbo_leann_segment_t *segment) {
  if (!segment) return;
  free(segment->ids);
  free(segment->offsets);
  free(segment->edges);
  free(segment->vectors);
  free(segment);
}

void turbo_leann_segment_destroy(turbo_leann_segment_t *segment) {
  tl_release(segment);
}

static int tl_input_cmp(const void *a, const void *b) {
  const tl_input_t *x = (const tl_input_t *)a;
  const tl_input_t *y = (const tl_input_t *)b;
  int c = memcmp(x->vector->chunk_id, y->vector->chunk_id, TURBO_LEANN_ID_BYTES);
  if (c) return c;
  return memcmp(x->vector->revision_id, y->vector->revision_id,
                TURBO_LEANN_ID_BYTES);
}

/* normalized cosine distance, accumulated in double for reproducibility */
static double tl_distance(const float *a, const float *b, uint32_t dim) {
  double dot = 0.0;
  for (uint32_t j = 0; j < dim; ++j) dot += (double)a[j] * (double)b[j];
  return 1.0 - dot;
}

static void tl_insert_nearest(uint32_t id, double distance, uint32_t *ids,
                              double *distances, uint32_t *size, uint32_t max) {
  uint32_t pos = 0;
  while (pos < *size &&
         (distances[pos] < distance ||
          (distances[pos] == distance && ids[pos] < id))) ++pos;
  if (pos >= max) return;
  if (*size < max) ++(*size);
  for (uint32_t k = *size - 1; k > pos; --k) {
    ids[k] = ids[k-1];
    distances[k] = distances[k-1];
  }
  ids[pos] = id;
  distances[pos] = distance;
}

static void tl_sort_ordinal(uint32_t *ids, uint32_t size) {
  for (uint32_t i = 1; i < size; ++i) {
    uint32_t v = ids[i];
    uint32_t j = i;
    while (j > 0 && ids[j-1] > v) { ids[j] = ids[j-1]; --j; }
    ids[j] = v;
  }
}

turbo_leann_status_t turbo_leann_build(const turbo_leann_build_t *config,
    const turbo_leann_vector_t *vectors, size_t count,
    turbo_leann_segment_t **out_segment) {
  if (!out_segment) return TURBO_LEANN_INVALID;
  *out_segment = NULL;
  if (!config || !vectors || config->struct_size != sizeof(*config) ||
      config->abi_version != TURBO_LEANN_ABI_VERSION ||
      config->dimensions == 0 || config->dimensions > TL_MAX_DIMENSIONS ||
      config->graph_degree < 2 || config->graph_degree > 64 ||
      config->generation == 0 ||
      !tl_valid_fingerprint(config->model_fingerprint) ||
      !tl_valid_fingerprint(config->scope_fingerprint))
    return TURBO_LEANN_INVALID;
  if (count == 0 || count > TL_MAX_NODES || count > config->max_nodes ||
      config->max_segment_bytes == 0 || config->max_pairwise_evaluations == 0)
    return TURBO_LEANN_LIMIT;
  if (sizeof(float) != 4 || FLT_RADIX != 2 || FLT_MANT_DIG != 24)
    return TURBO_LEANN_VERSION_UNSUPPORTED;
  if (count > 1 && count > config->max_pairwise_evaluations / (count - 1))
    return TURBO_LEANN_LIMIT;

  uint32_t degree = (uint32_t)(count - 1);
  if (degree > config->graph_degree) degree = config->graph_degree;
  size_t vector_count, vectors_bytes, ids_bytes, offset_bytes, edge_count, edges_bytes;
  size_t total = 144u;
  if (!tl_checked_mul(count, config->dimensions, &vector_count) ||
      !tl_checked_mul(vector_count, sizeof(float), &vectors_bytes) ||
      !tl_checked_mul(count, 32u, &ids_bytes) ||
      !tl_checked_mul(count+1, sizeof(uint32_t), &offset_bytes) ||
      !tl_checked_mul(count, degree, &edge_count) ||
      !tl_checked_mul(edge_count, sizeof(uint32_t), &edges_bytes) ||
      !tl_checked_add(total, ids_bytes, &total) ||
      !tl_checked_add(total, offset_bytes, &total) ||
      !tl_checked_add(total, edges_bytes, &total) ||
      !tl_checked_add(total, vectors_bytes, &total) ||
      total > config->max_segment_bytes || edge_count > UINT32_MAX)
    return TURBO_LEANN_LIMIT;

  tl_input_t *sorted = (tl_input_t *)calloc(count, sizeof(*sorted));
  turbo_leann_segment_t *s = (turbo_leann_segment_t *)calloc(1, sizeof(*s));
  if (!sorted || !s) { free(sorted); tl_release(s); return TURBO_LEANN_NO_MEMORY; }
  s->dimensions = config->dimensions;
  s->count = (uint32_t)count;
  s->graph_degree = degree;
  s->edge_count = (uint32_t)edge_count;
  s->generation = config->generation;
  memcpy(s->model, config->model_fingerprint, sizeof(s->model));
  memcpy(s->scope, config->scope_fingerprint, sizeof(s->scope));
  s->ids = (uint8_t *)malloc(ids_bytes);
  s->offsets = (uint32_t *)calloc(count+1, sizeof(uint32_t));
  s->edges = edges_bytes ? (uint32_t *)malloc(edges_bytes) : NULL;
  s->vectors = (float *)malloc(vectors_bytes);
  if (!s->ids || !s->offsets || (edges_bytes && !s->edges) || !s->vectors) {
    free(sorted); tl_release(s); return TURBO_LEANN_NO_MEMORY;
  }

  for (size_t i = 0; i < count; ++i) {
    double norm = 0.0;
    if (!tl_valid_vector(vectors[i].components, config->dimensions, &norm)) {
      free(sorted); tl_release(s); return TURBO_LEANN_INVALID;
    }
    sorted[i].vector = &vectors[i];
  }
  qsort(sorted, count, sizeof(*sorted), tl_input_cmp);
  for (size_t i = 0; i < count; ++i) {
    const turbo_leann_vector_t *v = sorted[i].vector;
    if (i && memcmp(sorted[i-1].vector->chunk_id, v->chunk_id,
                    TURBO_LEANN_ID_BYTES) == 0) {
      free(sorted); tl_release(s); return TURBO_LEANN_INVALID;
    }
    memcpy(s->ids+i*32, v->chunk_id, 16);
    memcpy(s->ids+i*32+16, v->revision_id, 16);
    double norm = 0.0;
    (void)tl_valid_vector(v->components, s->dimensions, &norm);
    for (uint32_t j = 0; j < s->dimensions; ++j)
      s->vectors[i*s->dimensions+j] = (float)((double)v->components[j]/norm);
  }
  free(sorted);

  /* L0 builds deterministic bounded kNN rows with mandatory ring edges.
   * It does NOT claim to implement LEANN's HNSW/hub pruning (L1).
   * The ring guarantees graph reachability even for separated clusters. */
  uint32_t used_edges = 0;
  for (uint32_t i = 0; i < s->count; ++i) {
    s->offsets[i] = used_edges;
    uint32_t ids[64];
    double distances[64];
    uint32_t nearest_count = 0;
    uint32_t anchors[2] = {0};
    uint32_t anchor_count = 0;
    if (s->count > 1) anchors[anchor_count++] = (i + s->count - 1) % s->count;
    if (s->count > 2) anchors[anchor_count++] = (i + 1) % s->count;
    uint32_t max_nearest = degree - anchor_count;
    for (uint32_t j = 0; j < s->count; ++j) {
      if (i == j || (anchor_count > 0 && j == anchors[0]) ||
          (anchor_count > 1 && j == anchors[1])) continue;
      if (!max_nearest) continue;
      tl_insert_nearest(j, tl_distance(s->vectors+(size_t)i*s->dimensions,
                        s->vectors+(size_t)j*s->dimensions,s->dimensions),
                        ids, distances, &nearest_count, max_nearest);
    }
    for (uint32_t j = 0; j < anchor_count; ++j) ids[j+nearest_count] = anchors[j];
    uint32_t used = nearest_count + anchor_count;
    tl_sort_ordinal(ids, used);
    for (uint32_t j = 0; j < used; ++j) s->edges[used_edges++] = ids[j];
  }
  s->offsets[s->count] = used_edges;
  s->edge_count = used_edges;
  *out_segment = s;
  return TURBO_LEANN_OK;
}
