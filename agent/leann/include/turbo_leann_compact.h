#ifndef TURBO_LEANN_COMPACT_H
#define TURBO_LEANN_COMPACT_H

/* Private experimental L1a. A compact ANN index derived from a validated L0
 * graph, not a second content or vector authority. No Python, SQLite or LLM.
 * The Host MUST verify SHA-256 manifest, exact generation and all node-map
 * permissions inside ONE pinned TurboDB read before calling any search.
 */
#include "turbo_leann.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_LEANN_PQ_FORMAT_VERSION 2u

typedef struct turbo_leann_compact_s turbo_leann_compact_t;

typedef struct turbo_leann_pq_config_s {
  size_t struct_size;
  uint32_t abi_version;
  uint32_t subquantizers;     /* 1..128, must evenly divide dimensions */
  uint32_t centroids;         /* 2..256, <= training node count */
  uint32_t iterations;        /* 1..64 deterministic Lloyd passes */
  uint64_t max_training_distance_evaluations; /* n * centroids * m * (iterations+1) */
  uint64_t max_compact_bytes;
  uint64_t max_peak_allocation_bytes; /* new allocations, excluding L0 input */
} turbo_leann_pq_config_t;

typedef struct turbo_leann_adc_hit_s {
  uint8_t chunk_label[TURBO_LEANN_ID_BYTES];
  uint8_t revision_label[TURBO_LEANN_ID_BYTES];
  float adc_squared_distance; /* lower is better; NOT exact cosine */
} turbo_leann_adc_hit_t;

/* Copies CSR + labels and trains a deterministic per-subspace PQ codebook on
 * normalized FP32 L0 vectors. Compact segment contains NO per-node FP32
 * embeddings; the input L0 segment is unaffected and may be destroyed.
 * It DOES NOT implement hub-preserving graph pruning or selective recompute.
 */
turbo_leann_status_t turbo_leann_pq_build(
    const turbo_leann_segment_t *reference,
    const turbo_leann_pq_config_t *config,
    turbo_leann_compact_t **out_compact);

turbo_leann_status_t turbo_leann_pq_serialized_size(
    const turbo_leann_compact_t *segment, size_t *out_size);
turbo_leann_status_t turbo_leann_pq_serialize(
    const turbo_leann_compact_t *segment, uint8_t *dst, size_t dst_capacity,
    size_t *out_size);
turbo_leann_status_t turbo_leann_pq_deserialize(
    const uint8_t *data, size_t size, const turbo_leann_limits_t *limits,
    turbo_leann_compact_t **out_compact);

/* Bounded best-first CSR exploration with asymmetric PQ distance (ADC).
 * max_distance_evaluations caps visited nodes, not exact recall. This is an
 * explicitly APPROXIMATE query: top_k <= max_distance_evaluations; scoring
 * only begins after caller has admitted ALL nodes through the pinned Node Map.
 * Output remains unchanged on failure; count is cleared on entry.
 */
turbo_leann_status_t turbo_leann_search_adc(
    const turbo_leann_compact_t *segment, const turbo_leann_query_t *query,
    turbo_leann_adc_hit_t *out_hits, size_t hits_capacity,
    size_t *out_count);

void turbo_leann_compact_destroy(turbo_leann_compact_t *segment);

#ifdef __cplusplus
}
#endif
#endif
