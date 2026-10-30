# FileCatalog revision-store key identity (private candidate)

Tracks #295 / #300 and extends PR #304's private `TAI` key contract. This slice
defines **identity only**: no public ABI, filesystem opener, catalog value schema,
write adapter integration or production migration.

## Keys

- `catalog_source(scope, source_id)` identifies one logical source inside the
  Host-authorized authority/collection scope.
- `catalog_path(scope, root_id, normalized_path)` identifies one path slot
  below one retained Host root capability.
- `catalog_path_prefix(scope, root_id, normalized_path_prefix)` creates a
  bounded B-tree byte-prefix range. The path is the final un-length-prefixed
  key field so a Host-selected component-safe normalized prefix can enumerate
  descendants without scanning another root.

`root_id` remains an opaque <=256-byte Host identity. A normalized path is an
opaque bounded byte string up to 2048 bytes that has already passed the root's
platform/path policy. The codec does not normalize, case-fold, resolve symlinks,
open files or authorize anything.

An empty path prefix is valid only for enumerating the selected root. An exact
path key must be nonempty. Neighboring root IDs are length-delimited and cannot
share a catalog prefix. A raw path byte prefix is intentionally not a security
boundary: the Host must provide a component-safe prefix such as `dir/` rather
than treating `dir` as equivalent to a path-component query.

## Revision-store history

The catalog should use the same TurboDB revision store and pinned read sequence
as Generation/SourceRevision/FTS/Graph. Updating one logical catalog key creates
historical values through `commit_seq`; a second catalog database and a
separate `observed_to` mutation protocol are unnecessary.

The value schema remains a separate gate. It must distinguish Host-stable source
identity/path authority from legacy-row provenance and carry explicit lifecycle
(PRESENT/TOMBSTONE) plus bounded observation hints. It must never promote
inode/file-ID/mtime/size hints into rename or unchanged-content proof.

## Legacy migration boundary

Legacy KnowledgeStore `document.id` and `uri` are **not** inputs to
`catalog_path()`. They were not obtained through the retained Host root
capability and therefore cannot synthesize `root_id + normalized_path`
authority. Legacy metadata may be preserved as `LEGACY_ROW_UNVERIFIED`
provenance, while authoritative path catalog entries require Host re-ingestion.

## Test gate

The existing SDK-independent revision-key test adds:

- catalog source vs mutable source-head key separation;
- exact root separation;
- component-safe path prefix inclusion/exclusion;
- embedded NUL/0xff binary path identity;
- empty exact-path rejection and 2048-byte path cap.

Passing this key test proves only deterministic storage identity. It does not
prove FileCatalog authorization, rename policy, path normalization, persisted
catalog values or migration completeness.
