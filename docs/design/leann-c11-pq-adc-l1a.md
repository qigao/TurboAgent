# LEANN-C11 L1a — deterministic PQ + bounded ADC graph traversal

**Status:** Private experimental algorithm slice in Draft PR #305; NO production
KnowledgeStore integration, TurboDB transaction adapter, high-degree pruning,
HNSW multi-level traversal, live model callback or installed SDK contract.

**Upstream decision:** TurboAgent #293/#299. Existing L0.5 segment and Node Map
remain immutable and unchanged. TurboAgent #304 defines canonical variable-length
SourceRevision/Chunk keys. LEANN segment-local 16-byte labels are opaque mapping
hints, not stable SourceRevision IDs or Host permissions. TurboDB #220/#222 are
required for atomic segment+Node Map+generation publication.

## L1a algorithm

1. A validated L0 deterministic kNN graph owns sorted segment-local labels,
   normalized FP32 vectors and CSR offsets/edges.
2. `turbo_leann_pq_build()` copies the labels and CSR structure. It trains
   `m` independent subspace codebooks with `k` centroids using deterministic
   initialization and a bounded fixed number of Lloyd iterations.
3. Store each node as **m bytes of PQ codes** rather than its full FP32 vector.
   Codebooks hold `k * dimensions` FP32 scalar values shared by all nodes.
4. `turbo_leann_search_adc()` normalizes the query and computes an asymmetric
   distance lookup table for each (subspace, centroid) pair. It traverses the
   CSR graph using deterministic best-first search from 3 fixed entry points.
5. Each node is evaluated at most once. `max_distance_evaluations` bounds the
   number of node distance computations, not graph expansion recall. Results
   are ranked by approximate **squared L2**, NOT exact cosine similarity.
   There is no automatic fallback to L0 on budget exhaustion.

No FP32 corpus vectors are stored inside the compact segment. Training needs
an existing in-memory L0 reference, but the compact result is independently
owned, serialized, and can be read after the L0 input is destroyed.

## Portable binary format v2

Little endian, header exactly 176 bytes:

| Offset | Field | Bytes |
|---:|---|---:|
| 0 | magic `TLC1PQ02` | 8 |
| 8 | version = 2 | 4 |
| 12 | header size = 176 | 4 |
| 16 | dimensions | 4 |
| 20 | node count | 4 |
| 24 | graph degree cap | 4 |
| 28 | actual edge count | 4 |
| 32 | nonzero Agent index generation | 8 |
| 40 | exact model fingerprint | 32 |
| 72 | exact security-scope fingerprint | 32 |
| 104 | total segment bytes | 8 |
| 112 | label table byte offset | 8 |
| 120 | CSR row-offset table byte offset | 8 |
| 128 | CSR edge table byte offset | 8 |
| 136 | codebook byte offset | 8 |
| 144 | PQ byte-code table offset | 8 |
| 152 | subquantizers `m` | 4 |
| 156 | centroids `k` | 4 |
| 160 | training iterations | 4 |
| 164 | reserved zero | 4 |
| 168 | CRC32 with these four bytes zeroed | 4 |
| 172 | reserved zero | 4 |

Sections are packed in this order: `node_count * 32` byte labels; `node_count+1`
uint32 CSR offsets; `edge_count` uint32 node ordinals; `dimensions*k` FP32
codebook scalars; `node_count*m` uint8 PQ code indices. No untrusted pointer,
platform-specific struct padding, pickle, Python or host-native float bit order.
Every field, code index, edge ordering, offset boundary and finite codebook
scalar is validated before exposing a deserialized object. CRC is only an
accidental corruption signal; **the single pinned TurboDB generation manifest
must SHA-256-bind both segment bytes and the full canonical-key Node Map**.

## Authorization and transaction boundary — NOT yet implemented in L1a

`turbo_leann_search_adc()` is intentionally a PRIVATE pure algorithm API.
A direct caller of it is trusted internal code, not an Agent tool or public
retrieval API. The future production binding must execute under one pinned
`TurboDB::SqliteRevision` read and must perform ALL of the following before
performing any ADC node traversal or consuming even an intermediate node:

- Host authorizes exact authority/collection/source and current ACL. Old
  generations never revive a revoked grant.
- Check generation, model identity, embedding preprocessing and scope against
  both the pinned read owner and manifest; no model-ID inference or fallback.
- Check SHA-256 of compact segment AND Node Map, deserialize both, enforce
  matching node count, labels, and collision-free full canonical Chunk keys.
- Admit **every node**, including traversal-only nodes, against exact pinned
  SourceRevision/extraction/chunk membership and Host policy. A collection-wide
  scope fingerprint is NOT proof of tenant ACL partitioning.
- Search stays inside fully authorized shards or fails `SCOPE_MISMATCH`.
  `top_k` filtering alone is not sufficient for ACLs or full recall.
- Commit prepared segments, bound Node Map and generation HEAD through one
  TurboDB writer transaction. `COMMIT_UNKNOWN` requires `request_id` inspection.

No direct SQLite fallback, no duplicate vector database, no model downloading,
no alternative parser, no installer/consumer test project or CI workflow.

## Measured local L1a fixture (not a benchmark extrapolation)

- Input: 192 deterministic vectors, 8 dimensions, graph degree 8.
- PQ: m=2, k=16, iterations=8; 24 deterministic query vectors.
- L0 serialized size: 19,348 bytes. L1a size: 14,132 bytes (27.0% smaller).
- Recall@10 versus exact cosine L0: 0.579 using exhaustive ADC traversal.
- GCC Release `-std=c11 -Wall -Wextra -Wpedantic -Wconversion -Werror`
  and Clang ASan+UBSan run all L0, L0.5 and L1a tests locally.

These small-instance values DO NOT establish 95–97% compression, quality parity,
large-corpus Recall@K, streaming disk behavior, or platform/installed SDK CI.

## Acceptance next

- L1b: high-degree-preserving graph pruning and true hub-aware graph traversal;
  fixed Recall@10/20 vs exact over 1k, 10k, 100k vectors and mixed ACL shards.
- L1c: deferred, **bounded** recompute of candidate embeddings using exact
  immutable Chunk keys and model identity from the one pinned read; reject
  missing historical text rather than consulting current file paths.
- Storage: real TurboDB Versioned SQLite segment+Node Map+generation adapter;
  injected crash before/after publication, `COMMIT_UNKNOWN` inspection,
  pruning/deletion and old-generation retained-text rejection behavior.
- Cross-platform exact HEAD C11 tests in existing TurboAgent workflow;
  performance evidence for p50/p95/p99, build CPU/peak RSS, space vs recall.
