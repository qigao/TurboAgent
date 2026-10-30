# ADR: LEANN-C11 over a revisioned TurboDB KnowledgeStore

**Status:** L0 C11 reference prototype on a Draft branch; L1-L5 and production storage integration remain design only. **Owners:** TurboAgent KnowledgeStore (application identity and retrieval), TurboDB SqliteRevision (durability), private LEANN-C11 core (derived ANN algorithm). **Date:** 2026-10-30.

**Related trackers:** TurboAgent #293, #294, #295, #296, #297, #298, #299, #300; TurboDB #220, #222 and Draft PRs #221/#223/#224. This ADR adds no released ABI, approved production migration, second database, or fallback engine.

## Implemented L0 boundary (2026-10-30)

The local L0 reference includes deterministic, bounded normalized-float32 kNN/CSR construction, exact cosine search, an immutable little-endian segment blob carrying scope/model/generation identities, and a CRC32 accidental-corruption guard. The full vectors remain in L0. It is **not** the LEANN paper's HNSW high-degree pruning/PQ/recompute implementation and is **not** a production TurboDB adapter. Host must verify a strong manifest digest and ACL before opening a segment. In-tree native tests must be compiled by existing root CTest; no extra installer/consumer project, CI workflow, or fallback runtime is introduced.

## L0 label mapping to the SourceRevision codec in PR #304

The L0 segment format stores opaque **16-byte chunk_label/revision_label pairs** for bounded, deterministic local graph coordinates. These are **not** the canonical TurboAgent identifiers: PR #304 source, revision, extraction and chunk keys are bounded **variable-length, length-delimited binary values**. The labels are lookup hints only, not global IDs, source paths, proof of authorization, or sufficient evidence references.

The future KnowledgeStore-owned versioned manifest must map (authority_scope, collection, generation, segment_id, chunk_label) bijectively to the FULL canonical revision-aware chunk key (source, revision, extraction, ordinal), include a strong SHA-256 digest of the segment bytes, and refuse collisions/duplicate labels **across all segments in one published generation**. Query results resolve labels through that map using the **same pinned TurboDB read owner** that returned the generation head, then revalidate exact membership via ta_revision_generation_accept_chunk, retained-content state and current Host ACL. Unknown/corrupt/unauthorized labels fail closed. The 16-byte revision_label is a compact hint, not a substitute for source-qualified immutable revision identity.

Until this node-map and SHA-256 manifest adapter are implemented, **L0 remains an isolated exact-reference kernel**. The existing PR #304 generation manifest does not yet contain an ANN node-map, so treating L0 labels directly as its keys would be incorrect.

## Decision

Build a standalone, pure-C11 **LEANN-like** index kernel as an **optional, derived query projection** of the existing TurboAgent KnowledgeStore. Use the existing TurboDB **Revisioned SQLite** proposal for revision identity, native transactions, trusted same-transaction Graph/FTS projections, published generation manifest and recovery; do not create a parallel authoritative VectorStore, FileCatalog, source database or persistence engine.

The project name 'LEANN-C11' describes an intended algorithmic implementation of the upstream LEANN research, not byte-level compatibility with its Python packages, FAISS derivatives, file artifacts, or published benchmark numbers. Validate algorithm equivalence empirically. Build no embedded Python interpreter, NumPy, FAISS, ZeroMQ, ONNX engine or model downloader as a hidden dependency.

**Placement:** a private, separately testable static C11 library in `agent/leann/{include,src}` (proposed CMake target `TurboAgentLeannCore`), with an adapter in the existing KnowledgeStore implementation that uses only accepted TurboDB revision APIs. A future independently reusable package may be evaluated but is not approved in this ADR. LEANN-C11 core must compile and test without SQLite or TurboAgent headers; SQLite and model callback ownership belongs to the adapter.

## Distinct identities and authority

| ID | Owning component | Meaning |
|---|---|---|
| `source_id` | TurboAgent KnowledgeStore | Stable logical source, independent of pathname and digest; Host scope-bound |
| `revision_id` | TurboAgent KnowledgeStore | Immutable version of exactly one source; historical content may be unavailable |
| `chunk_id` | TurboAgent KnowledgeStore | Revision + extractor/version/options + chunk ordinal (stable within that extraction) |
| `model_fingerprint` | Host-selected embedding provider | Provider + model weights revision/digest + tokenizer/preprocessor identity + dimension + metric + normalization; shared across one segment |
| `embedding_identity` | TurboAgent KnowledgeStore | Model fingerprint **plus** exact immutable embedded-input digest; unique to a chunk projection |
| `index_generation` | TurboAgent KnowledgeStore | Immutable *published* query membership and ANN/FTS/Graph projection view |
| `commit_seq` | TurboDB SqliteRevision | Durable database publication ordering; not the source revision or ACL |
| `request_id` | TurboDB SqliteRevision | Stable idempotency/unknown-commit inspection token |

The KnowledgeStore remains the only domain owner. Existing RuntimeStore, Tool Journal, Graph/StateGraph executor and MemoryStore remain authoritative for their respective data. Read-only evidence edges may refer to their immutable IDs; they do not extend Host permissions.

## Runtime architecture

~~~text
 Host-approved source + model
          |
 TurboAgent KnowledgeStore
   +-- Source / SourceRevision / extracted Chunk / EvidenceRef
   +-- FTS5 (lexical), graph adjacency, path/time, exact reference
   +-- LEANN storage adapter [revision/generation-pinned reads]
            +-- LEANN-C11 static core (NO DB, NO ML runtime)
                 +-- HNSW-like graph build -> hub-preserving prune -> CSR
                 +-- PQ encode + approximate-distance LUT
                 +-- two-level approximate/exact search (AQ/EQ)
                 +-- bounded dynamic batch recomputation
            +-- explicit Host embedding callback
          |
 TurboDB SqliteRevision transaction/read owner
   +-- immutable source and revision values
   +-- immutable ANN segment pages + checksum + manifest
   +-- trusted transaction-enlisted FTS/Graph projection
   +-- CAS generation head + request inspection
~~~

## C11 algorithm, build path

1. **Input:** only verified, retained, immutable extracted Chunk texts with exact revision and extractor IDs. Host supplies a specific model fingerprint and an explicit callback; the core does not read today's file path to reconstruct yesterday's chunk.
2. **Embedding:** compute normalized, finite float32 vectors in bounded batches; enforce dimensions/metric; construct a deterministic graph using ordered chunk identities, fixed PRNG seed, `M` and `ef_construction`. Test HNSW insertion/neighbor-selection and build/rebuild determinism. Do not assume all backends give bit-identical embeddings; model changes require new index identity.
3. **High-degree pruning:** rank nodes by *original graph degree* (tie by stable node ID); preserve up to `M` edges for a configurable top-`beta` hub fraction, use smaller `m` (paper reference: roughly `M/5`) for ordinary nodes, maintain bounded bidirectional reverse edges, and check reachability. Tune `beta`, `m`, `M` against an explicit byte/recall budget; no unconditional claim that a fixed threshold is optimal.
4. **Compact format:** serialize sorted stable node labels, the graph as CSR (bounded offsets + neighbor ordinals), upper-layer/entry-point metadata, PQ subquantizer codebooks and codes, a model fingerprint, dimension/metric/normalization, generation scope and per-page digests. Discard construction FP32 embeddings after completing and verifying the compact artifacts. An optional bounded *non-authoritative* cache may retain exact hot vectors.
5. **Search:** compute the query embedding once using the selected provider; PQ asymmetric-distance lookup ranks neighbors in approximate queue AQ; the top `alpha` promising unseen candidates enter a bounded exact queue EQ after re-embedding from immutable text. Batch promising candidates across exploration steps, then return top-k by exact query distance with deterministic tie-breaking. Bound graph hops, visited nodes, embeds, bytes, memory and elapsed budget; cancellation returns explicit status, not incomplete silent success. Use an exact algorithm as a reference and a separately requested mode, never automatic fallback.
6. **Build under budget:** initially build finite shard segments; when the entire dataset exceeds RAM/disk quotas, use bounded sharding with explicit merge/rebuild. The research paper uses two-centroid assignment for overlapping shards; accepting this step requires a measured cross-shard recall gate rather than a claim of theoretical equivalence.

LEANN paper: https://arxiv.org/html/2506.08276 (Sections 3-6, Appendices A-B). Upstream repository: https://github.com/StarTrail-org/LEANN.

## Physical representation and versioned index generations

**v1 storage choice:** keep immutable compressed ANN pages in the *same logical Knowledge/Graph SQLite database* as other derived projections, using bounded page payloads through TurboDB's revision transaction owner. No second on-disk DB, sidecar index authority or unrestricted SQLite handle in the public SDK. SQLite online WAL/rollback sidecars still exist as determined by its safe journal mode; a portable single-file snapshot is an explicit backup/export operation.

Proposed non-ABI logical keys/tables (final schema admitted by #222/#294/#298):

~~~text
rev/source/{source_id}/{revision_id}      immutable original/extracted provenance
rev/chunk/{chunk_id}                      immutable text, spans and extraction identity
ann/model/{model_fingerprint}              immutable canonical model fingerprint
ann/segment/{segment_id}/page/{ordinal}   bounded packed graph/PQ pages, SHA-256/CRC checks
ann/segment/{segment_id}/descriptor       immutable header, entry points and page checksums
ann/generation/{generation_id}/manifest   immutable segment refs, security partition, membership, delta refs
ann/generation/head/{scope}/{model}       CAS current published generation
ann/stage/{request_id}/...                private uncommitted-to-query candidate data
fts/... and graph/...                     existing trusted same-transaction projections
~~~

Segments are immutable; a published manifest binds exact segment digests, an immutable `index_generation -> commit_seq` publication mapping, and an explicit active-node membership bitmap and tombstone mask for that generation. `commit_seq` is resolved through the committed request receipt, not guessed from the generation number. A query pins a database snapshot and uses the mapped committed sequence for FTS/Graph historical lookup. Keep each historical generation's manifest while pinned by a retained read/evidence handle. No mutable adjacency graph or in-place overwrite of a historical segment. A latest-generation query may have base segments and bounded immutable delta segments; it merges scored candidates in the one pinned snapshot. Every published generation must explicitly report ANN coverage (READY, BUILD_REQUIRED, UNSUPPORTED) and expected model fingerprint, never silently omit newly indexed chunks.

**Page format v1 (proposed):** fixed magic `TLANN\001`, format version, byte-order marker (little-endian on disk), section ID, length and bounded record counts, scope/model/generation/segment IDs, page ordinal, total pages, checksum and payload; graph offsets use checked uint64 and local neighbor IDs checked uint32 if node count fits. Encoder and decoder are explicit; `memcpy(struct)` is forbidden as an on-disk serialization strategy. End-to-end SHA-256 segment digest in manifest, per-page CRC32C for fast corruption detection, strict length/overflow checks and explicit unsupported-version errors. All IDs are canonical length-delimited bytes, not unvalidated `char*` pointers persisted to disk.

**Space accounting:** `N*4*avg_degree + N*pq_code_bytes + N*node_label_bytes + CSR offsets + codebooks + manifests + model/text retention + cache`. Reference example (not a benchmark): a 768-dim float32 vector is 3072 bytes, while a 24-subquantizer 8-bit PQ code uses 24 bytes *before* graph, labels, codebooks and text; actual disk savings are workload-dependent. A small index or a high-churn workload may not benefit.

## Publication and crash semantics

**Strict small-batch v1:** keep writes in one owner-approved `RevisionedSqlite` transaction:

~~~text
Host validates scope and immutable source bytes
  -> extract/prepare/embed in bounded staging (not searchable)
  -> build + checksum ANN segment pages (not searchable)
  -> begin_write(request_id), expected_generation CAS
  -> append revision/change/tombstone + chunk records
  -> enlist typed FTS/Graph/source and ANN membership changes
  -> write verified segment pages + immutable manifest
  -> CAS published generation head
  -> COMMIT (one SQLite owner)
       SUCCESS -> new generation, all projections visible
       known failure -> old generation, no half-publish
       COMMIT_UNKNOWN -> block retry; fresh reopen and inspect request_id
~~~

Large builds requiring multiple transactions use *private PREPARED staging* pages, already verified and invisible to reader manifests, then a final **single publication transaction** enlisting all revision-head, FTS, Graph and generation changes and referencing the staged immutable pages. This is not a claim that staging-page writes belong to the final SQLite transaction. If final verification or CAS fails, prior generation stays query-visible and abandoned staging is reclaimed only by a future explicit, verified GC. No query may enumerate/guess staged keys. Preserve the current Snapshot/Generation C11 owner and exact `read_at` contract, not a new raw SQL writer.

`COMMIT_UNKNOWN` never permits an automatic second attempt. Read-only `inspect_request` on clean reopen must identify either the committed request/generation or a provable non-publication; indeterminate results remain quarantined. Exhaustive fault tests must cover `xWrite`, `xSync`, `ENOSPC`, lock contention, process kill, corrupt page, interrupted migration and shutdown with pinned readers.

## Incremental additions, deletions and time travel

- **Add/modify:** append immutable new SourceRevision/Chunk data; small accepted new chunks may enter a *bounded, explicitly manifested delta segment* that the LEANN query searches along with base segments. Delta vectors may temporarily retain FP32, but their bytes are measured and capped. Compact/merge through a new generation, not an in-place graph edit. If the generation's ANN coverage is incomplete, an ANN-specific query returns `INDEX_BUILD_REQUIRED`; lexical and exact modes remain separately selectable.
- **Delete:** publish generation-specific tombstones and remove content from results. A tombstoned graph node can remain as a navigation hop only when retained source text is still available *and authorized for this query*. Revocation or physical loss must never invoke re-embedding of inaccessible content. Rebuild/compact an appropriately partitioned graph or fail `LEANN_SCOPE_UNAVAILABLE`/`SOURCE_NOT_RETAINED` explicitly.
- **Historical read:** pin the requested `index_generation` and matching SQLite read snapshot, model and manifest before any read. Reproduce historical source/chunk bytes from retained immutable data only. A content hash without retained bytes is evidence, not replayable content.
- **Retention:** deletion of a source is not physical deletion of protected Run evidence. GC is pin-aware across open queries and retained EvidenceRefs; until accepted, GC returns unsupported instead of deleting history. Keep a clear policy for models no longer installed or no longer trusted.

`commit_seq`, `SourceRevision.revision_seq`, and `index_generation` are distinct: there is no assumption that generation ID equals database commit sequence, filesystem mtime, or an ACL version.

## Scope and permission rules

LEANN's ANN graph traversal and embedding recomputation process source text, not merely final returned IDs. A global graph with post-top-k ACL filtering is **not sufficient**.

- The trusted Host resolves authority_scope, tenant/collection and current ACL into a *permitted candidate/recompute set*. Only index segments built for that compatible security partition may be traversed. Recheck every returned EvidenceRef against the pinned generation and current Host authorization.
- If the request is narrower than a compatible partition, reject LEANN mode explicitly unless a proven scoped graph traversal that does not read/re-embed forbidden nodes is implemented and accepted. Never search unauthorized nodes then drop the hits.
- Index membership filters (revision/generation/tombstone/time/source root) are applied during candidate admission and revalidated before returning hits. Query budgets count excluded traversal work. Top-k candidate filtering alone is not considered complete coverage.
- No model-provided SQL, no arbitrary schema-extension registry, no host path escape, no hidden process or internet model downloads. Graph evidence and vector proximity never confer Tool/Run permissions.

## C11 boundary sketch (illustrative, private until ABI acceptance)

~~~c
#define TURBO_LEANN_C11_ABI_VERSION 1u

typedef struct turbo_leann_index_s turbo_leann_index_t;
typedef struct turbo_leann_result_s turbo_leann_result_t;

typedef struct turbo_leann_bytes_s {
    const uint8_t *data; /* borrowed within synchronous callback */
    size_t length;
} turbo_leann_bytes_t;

typedef struct turbo_leann_runtime_s {
    size_t struct_size;
    uint32_t abi_version;
    void *host_context;
    /* Called only after Host scope + pinned-revision admission. */
    int (*get_immutable_chunk)(void *host_context, uint64_t node_ordinal,
                               uint8_t *dst, size_t capacity, size_t *out_length);
    int (*embed_batch)(void *host_context, const turbo_leann_bytes_t *texts,
                       size_t count, float *out_vectors, uint32_t dimension);
    int (*read_page)(void *host_context, uint64_t segment_id, uint32_t page_no,
                     uint8_t *dst, size_t capacity, size_t *out_length);
    int (*cancel_requested)(void *host_context);
} turbo_leann_runtime_t;

typedef struct turbo_leann_search_config_s {
    size_t struct_size;
    uint32_t abi_version;
    uint64_t generation;
    uint32_t dimension, top_k, ef_search, max_candidates;
    uint64_t max_embedded_nodes, max_bytes_read, max_result_bytes;
    const float *query_embedding; /* borrowed, normalized by contract */
    const uint8_t *model_fingerprint; /* 32-byte validated canonical digest */
} turbo_leann_search_config_t;

int turbo_leann_open_immutable(const turbo_leann_runtime_t *runtime,
                              const void *manifest, size_t manifest_length,
                              turbo_leann_index_t **out_index);
int turbo_leann_search(turbo_leann_index_t *index,
                       const turbo_leann_search_config_t *config,
                       turbo_leann_result_t **out_result);
void turbo_leann_result_destroy(turbo_leann_result_t *result);
void turbo_leann_index_close(turbo_leann_index_t *index);
~~~

The core only accepts a **Host-approved pinned manifest** and bounded page/text callbacks; it does not open paths, DB handles or network connections. This sketch is not a compilable installed header yet: status/ownership, index construction sink, node-ID typed views, callback threading/reentrancy, closure/drain and output hit accessors must be finalized before public ABI. One callback failure must abort the operation without returning partial rankings as success.

The stable **public application API** remains the one proposed in TurboAgent #297/#298 using `turbo_agent_knowledge_store_t *`, explicit modes (`EXACT`, `LEANN_COMPACT`, `LEXICAL`, `HYBRID`) and one owned EvidenceRef result contract. The LEANN core is a private component, not a second public Query API/transaction owner.

## Test plan and implementation gates

| Phase | Work | Required acceptance |
|---|---|---|
| L0 | C11 graph + CSR + typed float32 exact reference | 1k/10k/100k deterministic golden comparisons, ASan/UBSan, malformed-index rejection |
| L1 | High-degree pruning + PQ codebooks + two-level AQ/EQ search | recall@10/20 vs exact, distance/model consistency, no unbounded queue or memory |
| L2 | Bounded dynamic embedding batches + immutable-text callback | selected nodes only; failed/missing/mutated source text fails closed, model fingerprint mismatch rejected |
| L3 | TurboDB-pinned immutable segment pages + generation manifest | historical read, request_id, crash before/after commit, xSync/xWrite faults, no staged leakage |
| L4 | Scoped per-tenant/ACL partition + delta/tombstone rebuild | cross-scope adversarial tests, revision time travel and no post-top-k ACL leaks |
| L5 | KnowledgeStore integration + migration + performance | existing docs/chunks/FTS/vector data preserved, native Linux/Windows/macOS installed C11 consumer, explicit performance report |

Metrics, not assumed targets: recall@1/10/20; p50/p95/p99 cold/warm query latency; embed calls and batches per query; index size **including** node IDs, manifests and retained content; build peak RSS/temp disk; append/delete churn; long-lived snapshot retention/GC; 1k/10k/100k/1M corpus as resources allow. Compare exact float32, compact LEANN, existing FTS5 and hybrid without silently changing embedding model or ACL.

Do not merge a production switch until the upstream dependency chain (#221 -> #223 -> #224) and TurboAgent #294-#298 is accepted at appropriate exact heads. Keep the first implementation and this ADR Draft until those gates pass. Upstream paper's reported 95-97% compression and query-quality numbers are **not** inherited by this implementation.

## Explicitly deferred

- A separate database engine, a rewrite of the SQLite pager/B-tree, Adventure DB allocator, FAISS/FFI fallback, an independent hosted Embedding service, a second query authority, arbitrary graph traversal over forbidden content.
- Copying unrelated Run/Journal/Memory rows into KnowledgeStore; permission changes still belong to their Host authority.
- A new public C11 ABI or registered schema-extension API until existing TurboDB and TurboAgent version/ownership specs are accepted.

## L0.5: pinned Node Map / SHA-256 manifest boundary

The private C11 core now provides \`turbo_leann_binding.h\` and \`binding.c\`.
This is a **prototype contract**, not a live TurboDB adapter or installed ABI.

- A separate, immutable v1 binary Node Map contains entries ordered by the segment's
  canonical 128-bit local labels. Each entry stores the COMPLETE opaque canonical
  Agent #304 Chunk Key and SourceRevision Key as bounded length-delimited bytes.
  It does **not** reimplement #304's key codec. The map refuses duplicate full
  Chunk Keys even if they use different local labels.
- A manifest binds exact Agent generation, publication \`commit_seq\`, model/scope
  fingerprints, dimensions, node count, and **independent SHA-256 digests** of the
  immutable segment and Node Map blobs.
- \`turbo_leann_search_pinned_exact\` accepts a Host-supplied **already pinned**
  read owner, separately supplied published manifest, immutable buffers, bounded
  limits, and SHA-256/admission callbacks. It performs cryptographic digest
  comparison **before** decoding, checks both binary formats, maps every graph
  node to exactly one complete Chunk Key, and calls admission for **all** nodes
  before computing or publishing any result.
- The callback is REQUIRED to authenticate exact SourceRevision/extraction
  membership through Agent #304 in the **same** pinned TurboDB read and
  revalidate current Host grants. Scope fingerprints and cryptographic hashes
  are not grants. The private algorithm kernel cannot open SQLite or use a
  second runtime store. A denied/missing source fails closed.
- Results borrow full canonical key views from the supplied immutable Node Map;
  the owner must keep the Node Map and pinned read alive until results are
  consumed. No automatic fallback, implicit retry, current-file reopening,
  or commit on destruction.
- \`commit_seq\` and Agent \`generation\` remain distinct. L0.5 requires the
  caller's selected pinned publication tuple to exactly match the manifest.
  A durable live bridge using TurboDB #220/#222 and Agent #304 remains blocked
  until the upstream contracts qualify; this C11 callback seam is **not** a
  substitute for atomic write/publication or for a true Host ACL.
- SHA-256 is provided by the canonical Crypto provider callback, **not**
  reimplemented inside LEANN-C11. The unit test uses a deliberately NON-
  cryptographic fake digest callback, and does not claim to qualify SHA-256
  cryptography or a real pinned TurboDB connection.
- L0.5 in-tree test covers generation/commit/scope/model mismatch, two digest
  mismatches, a substituted map label, forged duplicate full keys, an admission
  refusal on the last node, truncation at every Map length, single-bit mutations
  of each Map byte, size/computation budgets, and successful exact retrieval.
  Local GCC C11 Release and Clang ASan/UBSan pass; CI must execute exact-head
  tests before acceptance.

## CI issue held separate from LEANN

Native SDK #41036027499 failed on macOS ARM64 at upstream NuGet cache
restore: \`gmssl_arm64-osx\` package returned HTTP 404. Linux x64, Windows x64,
Android ARM64 and metadata jobs succeeded at that exact HEAD. The failure is
external to LEANN C11 compile/test and must not be hidden by disabling tests,
a second package source, or an unrelated CI probe. The full PR CI remains
RED until qualified against a future exact HEAD.

