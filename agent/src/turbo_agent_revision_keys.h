#ifndef TURBO_AGENT_REVISION_KEYS_H
#define TURBO_AGENT_REVISION_KEYS_H

#include <stddef.h>
#include <stdint.h>

/* Private integration candidate: no installed/public ABI and no storage I/O.
 * Components are Host-issued opaque bytes (not paths or C strings). The Host
 * authorizes every source/edge independently before invoking a future adapter.
 * Encoding or comparing a key never creates an authorization grant. */
#define TA_REVISION_ID_MAX_BYTES 256u
#define TA_REVISION_KEY_MAX_BYTES 4096u

typedef struct ta_revision_id_s {
  const unsigned char *data;
  size_t size;
} ta_revision_id_t;

typedef struct ta_revision_scope_s {
  ta_revision_id_t authority;
  ta_revision_id_t collection;
} ta_revision_scope_t;

typedef struct ta_revision_ref_s {
  ta_revision_scope_t scope;
  ta_revision_id_t source;
  ta_revision_id_t revision;
  ta_revision_id_t extraction;
} ta_revision_ref_t;

typedef struct ta_revision_key_s {
  unsigned char data[TA_REVISION_KEY_MAX_BYTES];
  size_t size;
} ta_revision_key_t;

typedef enum ta_revision_key_status_e {
  TA_REVISION_KEY_OK = 0,
  TA_REVISION_KEY_INVALID = 1,
  TA_REVISION_KEY_LIMIT = 2,
  TA_REVISION_KEY_MALFORMED = 3
} ta_revision_key_status_t;

typedef enum ta_revision_edge_direction_e {
  TA_REVISION_EDGE_OUT = 1,
  TA_REVISION_EDGE_IN = 2
} ta_revision_edge_direction_t;

typedef struct ta_revision_edge_s {
  ta_revision_scope_t scope;
  ta_revision_id_t from_source;
  ta_revision_id_t from_revision;
  ta_revision_id_t to_source;
  ta_revision_id_t to_revision;
  ta_revision_id_t relation;
  ta_revision_id_t edge_id; /* distinguishes separate evidence-bearing edges */
} ta_revision_edge_t;

/* Inputs borrow for the call. Encoders write the entire owned output or clear
 * it on error. Nonempty fields only, <=256 bytes each; no escaping, hashing,
 * path normalization, allocation or truncation. Output must not overlap input
 * descriptors. Input bytes may reference a prior output's data buffer. */
ta_revision_key_status_t ta_revision_generation_key(
    const ta_revision_scope_t *scope, ta_revision_key_t *out);
ta_revision_key_status_t ta_revision_source_head_key(
    const ta_revision_scope_t *scope, ta_revision_id_t source,
    ta_revision_key_t *out);
/* Mandatory exact SourceRevision + extraction prefix for an FTS query. */
ta_revision_key_status_t ta_revision_chunk_prefix(
    const ta_revision_ref_t *ref, ta_revision_key_t *out);
ta_revision_key_status_t ta_revision_chunk_key(
    const ta_revision_ref_t *ref, uint64_t ordinal, ta_revision_key_t *out);
/* Decoded IDs borrow encoded.data until that buffer changes. Output structs
 * must not overlap encoded.data. Reject wrong versions/classes/trailing bytes. */
ta_revision_key_status_t ta_revision_chunk_decode(
    ta_revision_id_t encoded, ta_revision_ref_t *out_ref, uint64_t *out_ordinal);
/* Exact structural equality, not a byte-prefix ACL test. malformed => false. */
int ta_revision_chunk_matches(ta_revision_id_t encoded,
                             const ta_revision_ref_t *expected);

ta_revision_key_status_t ta_revision_edge_prefix(
    const ta_revision_scope_t *scope, ta_revision_edge_direction_t direction,
    ta_revision_id_t focus_source, ta_revision_id_t focus_revision,
    ta_revision_id_t relation, ta_revision_key_t *out);
ta_revision_key_status_t ta_revision_edge_key(
    const ta_revision_edge_t *edge, ta_revision_edge_direction_t direction,
    ta_revision_key_t *out);
ta_revision_key_status_t ta_revision_edge_decode(
    ta_revision_id_t encoded, ta_revision_edge_t *out_edge,
    ta_revision_edge_direction_t *out_direction);

#endif
