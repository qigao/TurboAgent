#ifndef TURBO_LEANN_COMPACT_INTERNAL_H
#define TURBO_LEANN_COMPACT_INTERNAL_H

#include "turbo_leann_compact.h"

/* Private L1 compact in-memory representation. All pointers owned by object;
 * none of these fields are exposed to the installed TurboAgent API. */
struct turbo_leann_compact_s {
  uint32_t dimensions, count, degree, edge_count, m, k, iterations;
  uint64_t generation;
  uint8_t model[32], scope[32];
  uint8_t *ids;       /* n x (chunk_label[16], revision_label[16]) */
  uint32_t *offsets;  /* (n+1) CSR offsets */
  uint32_t *edges;    /* CSR neighbor node ordinals */
  float *codebook;    /* m * k * (dim/m) shared centroids */
  uint8_t *codes;     /* n * m byte codes: no FP32 per-node vectors */
};

#endif
