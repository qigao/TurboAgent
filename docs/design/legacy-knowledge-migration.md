# Legacy KnowledgeStore migration planner (private, read-only)

Tracks #295 / #298 / #300. The existing SQLite KnowledgeStore stores
`documents(id,uri,kind,title,mtime,size,content_hash,metadata_json,updated_at)`
and `chunks(id,document_id,ordinal,text,metadata_json)`. Its normal
`content_hash` path is FNV-1a 64-bit over indexed/extracted text; callers can
also supply an arbitrary string. Neither case is original-source SHA-256.

This private planner performs **no database writes and no filesystem access**.
It validates already-read legacy rows and produces an opaque migration identity
plus a typed SourceRevision record:

- `document.id` is encoded as an opaque legacy source-row identity even when
  it looks like a path. It is never accepted as Host root authority.
- `uri` is retained only as a borrowed catalog alias and does not influence
  source identity.
- the migration revision identity contains the exact legacy document ID,
  legacy content_hash bytes, legacy updated_at and chunk count. It is a tagged
  row identity, **not a cryptographic content digest**.
- SourceRevision digest semantics are always `UNVERIFIED_LEGACY`, retention is
  `CONTENT_UNAVAILABLE`, and SHA-256 is zero. A 16-hex hash is reported only
  as `HEX64_SHAPE_UNVERIFIED`, never promoted to SHA-256.
- chunks must be contiguous from ordinal zero and have the exact legacy
  `<document-id>#<ordinal>` identity. Embedded NUL, malformed ownership,
  missing rows and oversized fields fail closed.
- one empty chunk is valid because legacy `upsert_text` deliberately creates
  it for an empty document.
- legacy `chunks_fts` rows are not trusted as migration input. FTS is derived
  and must be rebuilt from structurally validated `chunks.text`.
- if the exact tagged source/revision identity exceeds the private 256-byte ID
  bound, migration returns `REINGEST_REQUIRED`; it never truncates or hashes
  the legacy identity to make it fit.

The planner preserves borrowed document/chunk metadata for a later catalog and
chunk-payload migration layer. It does not claim rename identity, current file
existence, original bytes, ACL authority or rebuildability. Historical
original content that is not retained remains unavailable; migration must never
read today's path and label those bytes as the historical revision.

The standalone regression includes normal FNV-shaped rows, caller-supplied
opaque hashes, path/URI alias separation, malformed/noncontiguous chunks,
zero-length documents and overlong IDs. GCC, Clang and ASan/UBSan pass together
with all current private revision tests before this candidate is pushed.

## On-disk audit

A separate private SQLite reader now qualifies the exact current legacy
`documents` and `chunks` column layouts before planning any row. It requires
an autocommit connection, opens one deferred read transaction, applies explicit
document/chunk/copied-byte bounds and invokes the planner synchronously. It
rejects extra/altered columns and triggers on the authoritative legacy tables.

The reader deliberately never queries `chunks_fts`. The on-disk fixture
inserts a stale FTS row and still produces plans solely from the structurally
validated `chunks.text`; the new index will rebuild its own FTS projection.

The fixture also proves the audit performs no data migration in place: it reads
two legacy documents (including the one-empty-chunk representation), leaves
their row count unchanged, rejects an already-active transaction, trips finite
document bounds and fails closed after a schema injection.
