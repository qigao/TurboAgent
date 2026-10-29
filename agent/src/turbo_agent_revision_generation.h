#ifndef TURBO_AGENT_REVISION_GENERATION_H
#define TURBO_AGENT_REVISION_GENERATION_H

#include "turbo_agent_revision_keys.h"

/* Private candidate, not a public installed ABI, DB adapter or ACL service. */
#define TA_REVISION_GENERATION_MAX_SOURCES 64u
#define TA_REVISION_GENERATION_MAX_BYTES 65536u
#define TA_REVISION_GENERATION_MAX_CHUNKS UINT64_C(1048576)

typedef enum ta_revision_generation_status_e {
  TA_REVISION_GENERATION_OK = 0,
  TA_REVISION_GENERATION_INVALID = 1,
  TA_REVISION_GENERATION_LIMIT = 2,
  TA_REVISION_GENERATION_MALFORMED = 3,
  TA_REVISION_GENERATION_SCOPE_MISMATCH = 4,
  TA_REVISION_GENERATION_NOT_FOUND = 5
} ta_revision_generation_status_t;

typedef struct ta_revision_generation_member_s {
  ta_revision_id_t source, revision, extraction;
  uint64_t chunk_count; /* valid ordinals are [0, chunk_count); zero is allowed */
} ta_revision_generation_member_t;

typedef struct ta_revision_generation_payload_s {
  unsigned char data[TA_REVISION_GENERATION_MAX_BYTES];
  size_t size;
} ta_revision_generation_payload_t;

typedef struct ta_revision_generation_view_s {
  uint64_t generation_id; /* positive Agent identity, NOT a TurboDB commit_seq */
  ta_revision_scope_t scope;
  size_t member_count;
  ta_revision_generation_member_t members[TA_REVISION_GENERATION_MAX_SOURCES];
} ta_revision_generation_view_t;

typedef struct ta_revision_generation_selection_s {
  uint64_t generation_id;
  ta_revision_ref_t ref;
  uint64_t chunk_count;
  ta_revision_key_t chunk_prefix; /* owned exact revision/extraction prefix */
} ta_revision_generation_selection_t;

/* Canonical full selection for ONE bounded collection. Sources must be in
 * strictly increasing binary lexicographic order; duplicates/unsorted input
 * are rejected, not normalized. Empty selections are valid. All identities
 * borrow for the call, and every nonempty ID is bounded by the key contract.
 * Outputs must not overlap any input bytes or descriptors. All output bytes
 * are cleared on error. No allocation, file access or implicit truncation. */
ta_revision_generation_status_t ta_revision_generation_encode(
    const ta_revision_scope_t *scope, uint64_t generation_id,
    const ta_revision_generation_member_t *members, size_t member_count,
    ta_revision_generation_payload_t *out);

/* Validate the ENTIRE record before exposing any membership. expected_scope
 * must come from independent Host policy, never from the untrusted record.
 * All decoded IDs borrow the immutable input; retain it until views/selections
 * are no longer needed. No decoded view is trusted as input to another call. */
ta_revision_generation_status_t ta_revision_generation_decode(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_generation_view_t *out);

/* Resolve one exact source to one selected revision/extractor and owned query
 * prefix. Absent sources return NOT_FOUND with all outputs cleared. An empty
 * selection NEVER falls back to source-wide or namespace-wide retrieval.
 * The future DB adapter must load the record at ta_revision_generation_key()
 * and query through the SAME pinned read transaction; this codec proves no
 * snapshot, transaction, source retention, permission or data-existence claim. */
ta_revision_generation_status_t ta_revision_generation_resolve(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t source, ta_revision_generation_selection_t *out);

/* Structural membership checks, not authority grants. Missing/old identities
 * and out-of-range ordinals yield OK + false. Malformed data returns an error.
 * Graph checks both endpoint revisions; it does not verify relation policy,
 * evidence validity or that a corresponding stored edge exists. */
ta_revision_generation_status_t ta_revision_generation_accept_chunk(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t chunk_key, int *out_matches);
ta_revision_generation_status_t ta_revision_generation_accept_edge(
    ta_revision_id_t encoded, const ta_revision_scope_t *expected_scope,
    ta_revision_id_t edge_key, int *out_matches);

#endif
