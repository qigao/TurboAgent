#ifndef TURBO_LEANN_INTERNAL_H
#define TURBO_LEANN_INTERNAL_H

#include "turbo_leann.h"

/* Host endianness is irrelevant; codec reads/writes every integer explicitly. */
struct turbo_leann_segment_s {
  uint32_t dimensions;
  uint32_t count;
  uint32_t graph_degree;
  uint32_t edge_count;
  uint64_t generation;
  uint8_t model[TURBO_LEANN_FINGERPRINT_BYTES];
  uint8_t scope[TURBO_LEANN_FINGERPRINT_BYTES];
  uint8_t *ids;       /* ordered (chunk_label[16], revision_label[16]) pairs */
  uint32_t *offsets;  /* CSR, count+1 values */
  uint32_t *edges;    /* ascending node ordinals, no self or duplicate */
  float *vectors;     /* normalized row-major reference vectors, L0 only */
};

int tl_checked_add(size_t a, size_t b, size_t *out);
int tl_checked_mul(size_t a, size_t b, size_t *out);
int tl_valid_fingerprint(const uint8_t *fingerprint);
int tl_valid_vector(const float *components, uint32_t dimensions, double *out_norm);
void tl_release(turbo_leann_segment_t *segment);

#endif
