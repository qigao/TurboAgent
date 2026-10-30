#ifndef TURBO_LEANN_H
#define TURBO_LEANN_H

/* Private, standalone C11 index kernel. Not an installed TurboAgent ABI. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_LEANN_ABI_VERSION 1u
#define TURBO_LEANN_ID_BYTES 16u
#define TURBO_LEANN_FINGERPRINT_BYTES 32u

typedef enum turbo_leann_status_e {
  TURBO_LEANN_OK = 0,
  TURBO_LEANN_INVALID = 1,
  TURBO_LEANN_LIMIT = 2,
  TURBO_LEANN_NO_MEMORY = 3,
  TURBO_LEANN_CORRUPT = 4,
  TURBO_LEANN_VERSION_UNSUPPORTED = 5,
  TURBO_LEANN_SCOPE_MISMATCH = 6
} turbo_leann_status_t;

typedef struct turbo_leann_segment_s turbo_leann_segment_t;

/* L0 node labels are opaque 128-bit, segment-local lookup keys. They are NOT
 * TurboAgent SourceRevision/Chunk IDs: the latter are length-delimited
 * (scope, source, revision, extraction, ordinal) binary keys in #304.
 * A future trusted same-transaction TurboDB manifest maps each label to its
 * FULL canonical key; no label alone grants authority or identifies content.
 * Enforce collision-free uniqueness across the published generation, and
 * validate every resolved key + generation + current Host ACL on lookup.
 */
typedef struct turbo_leann_vector_s {
  uint8_t chunk_label[TURBO_LEANN_ID_BYTES];
  uint8_t revision_label[TURBO_LEANN_ID_BYTES];
  const float *components; /* borrowed, dimensions floats */
} turbo_leann_vector_t;

typedef struct turbo_leann_build_s {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t dimensions;
  uint32_t graph_degree; /* 2..64; degree capped by node_count-1 */
  uint64_t generation;   /* nonzero, immutable index generation */
  uint8_t model_fingerprint[TURBO_LEANN_FINGERPRINT_BYTES];
  uint8_t scope_fingerprint[TURBO_LEANN_FINGERPRINT_BYTES];
  uint32_t max_nodes;
  uint64_t max_pairwise_evaluations; /* explicit O(N^2) build budget */
  uint64_t max_segment_bytes;
} turbo_leann_build_t;

typedef struct turbo_leann_limits_s {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t max_nodes;
  uint32_t max_dimensions;
  uint64_t max_segment_bytes;
} turbo_leann_limits_t;

typedef struct turbo_leann_query_s {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t generation;
  uint8_t model_fingerprint[TURBO_LEANN_FINGERPRINT_BYTES];
  uint8_t scope_fingerprint[TURBO_LEANN_FINGERPRINT_BYTES];
  const float *components; /* borrowed for synchronous call */
  uint32_t dimensions;
  uint32_t top_k;
  uint32_t max_distance_evaluations; /* must cover all nodes for exact search */
} turbo_leann_query_t;

/* Returned labels must be resolved through the pinned owner map, never
 * treated as source paths, global IDs or authorization tokens. */
typedef struct turbo_leann_hit_s {
  uint8_t chunk_label[TURBO_LEANN_ID_BYTES];
  uint8_t revision_label[TURBO_LEANN_ID_BYTES];
  float cosine_similarity;
} turbo_leann_hit_t;

/* L0 reference algorithm: deterministic normalized float32 exact search, NOT
 * compact LEANN/PQ search. All nodes in a segment share one Host-admitted
 * security scope and one model identity. The caller must prove this before
 * building; a fingerprint is an identity check, NOT an authorization grant.
 * Build is O(node_count^2 * dimension) and refused above explicit budgets.
 */
turbo_leann_status_t turbo_leann_build(const turbo_leann_build_t *config,
    const turbo_leann_vector_t *vectors, size_t count,
    turbo_leann_segment_t **out_segment);

/* Returns exactly one immutable, versioned, platform-independent LE blob.
 * CRC32 detects accidental corruption; the TurboDB generation manifest MUST
 * independently bind its SHA-256 digest before admitting the segment.
 */
turbo_leann_status_t turbo_leann_serialized_size(
    const turbo_leann_segment_t *segment, size_t *out_size);
turbo_leann_status_t turbo_leann_serialize(
    const turbo_leann_segment_t *segment, uint8_t *dst, size_t dst_capacity,
    size_t *out_size);
turbo_leann_status_t turbo_leann_deserialize(const uint8_t *data, size_t size,
    const turbo_leann_limits_t *limits, turbo_leann_segment_t **out_segment);

/* Fail closed on any mismatched scope/model/generation. Caller pins a single
 * committed KnowledgeStore generation, validates current Host ACL, and MUST
 * restrict the whole segment (including graph navigation) to that scope.
 * No retries, fallback index or reads of current external source paths.
 * Output is cleared on entry and only published for a completed search.
 */
turbo_leann_status_t turbo_leann_search_exact(
    const turbo_leann_segment_t *segment, const turbo_leann_query_t *query,
    turbo_leann_hit_t *out_hits, size_t hits_capacity, size_t *out_count);

void turbo_leann_segment_destroy(turbo_leann_segment_t *segment);

#ifdef __cplusplus
}
#endif
#endif
