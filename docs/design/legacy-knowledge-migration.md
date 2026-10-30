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
  `CONTENT_UNAVAILABLE`, and SHA-256 is zero. TAR v2 additionally preserves the
  legacy row's nonnegative `mtime` as `source_mtime_seconds` and `updated_at` as
  `observed_at_seconds`; neither timestamp proves that current path bytes are unchanged. A 16-hex hash is reported only
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

## Read-only handle gate

The on-disk audit refuses read-write SQLite handles via
`sqlite3_db_readonly(main)`. Migration must use a separate read-only legacy
connection; it cannot borrow the live mutable KnowledgeStore connection and
upgrade its read into a write. Hard ceilings additionally cap one audit at 64
documents, 65,536 chunks per document and 64 MiB copied row bytes per document,
with callers allowed to select smaller limits.


## Complete-snapshot preflight

Migration planning is two-pass inside **one pinned read transaction**. The first
pass validates every admitted document and all of its chunks without invoking
the visitor. Only after the complete snapshot is structurally valid does a
second pass emit migration plans. A malformed late document therefore cannot
appear after earlier plans have already been handed to a target migration
writer.

A separate coverage check rejects orphan rows in `chunks` whose
`document_id` has no matching `documents.id`. SQLite foreign-key enforcement
is connection-local and may historically have been disabled, so the presence
of a FOREIGN KEY clause alone is not sufficient evidence. Orphan authoritative
rows return `TA_LEGACY_SQLITE_COVERAGE`; they are never silently dropped.

The two passes share the same deferred read transaction, so validation and
emission observe the same source snapshot. This does not itself make the target
migration atomic; the target writer must still use the one-owner publication
contract and abort on visitor/backend failure.


## Exact relational-schema admission

The reader pins the read transaction **before** schema admission. It verifies
SQLite `NOT NULL` flags (including the legacy rowid-table exception for
`TEXT PRIMARY KEY`), a single-column unique URI index, the chunk
`document_id` B-tree index, and the exact
`chunks(document_id) REFERENCES documents(id) ON DELETE CASCADE` foreign key.
A forged same-name schema with otherwise matching column names/types/PK but
without the FK fails before any migration visitor. Removing the authoritative
chunk lookup index also fails; the scanner does not silently substitute an
unbounded scan. Schema inspection, orphan coverage, complete validation and
emission now share one pinned source transaction.

These constraints admit the actual existing legacy KnowledgeStore DDL; they do
not assert that historical extracted text is original-source content, establish
path authority, or prove that a target migration was atomically published.

## Lossless private provenance payload and atomic staging candidate

The legacy scanner keeps borrowed row views only during its visitor. Before
they expire, a migration coordinator must encode two versioned private payload
kinds: one document payload per new SourceRevision and one chunk-metadata
payload for each exact legacy ordinal. The document record retains **all**
`id, uri, kind, title, mtime, size, content_hash, metadata_json, updated_at`
values. The chunk record retains `id, document_id, ordinal, metadata_json`;
the **exact original extracted** `chunks.text` bytes are separately stored
as the immutable Chunk KV value. The FTS projection must use those exact bytes
and is skipped only for a zero-length chunk. Nullable title/metadata retain
SQL NULL versus present-empty, and neither metadata nor URI is interpreted as
an ACL/path grant.

The private `ta_revision_write_publish_generation` seam requires one
provenance bundle for **every newly staged UNVERIFIED_LEGACY SourceRevision**.
Before any backend mutation it decodes every payload, reconstructs the full
legacy rows and calls the original migration planner to verify source/revision/
extraction identity, contiguous chunk IDs, exact Chunk-KV versus indexed text,
and TAR v2 observed timestamps and content length. Missing, corrupt, reordered,
changed-hash, changed-chunk or unbounded provenance fails before any put.
Document/chunk provenance keys use the same immutable SourceRevision/Chunk
identities under disjoint private namespaces and are included in the byte
budget. The adapter writes them with the same single backend owner **before**
the generation-head CAS and exactly one publish. Any staging failure aborts;
COMMIT_UNKNOWN still requires clean-owner request-id inspection, never replay.

This is a **private, SDK-independent candidate**, not production cutover or a
claim of completed TurboDB integration. A real TurboDB owner bridge, Host
authorization/ACL mapping, retained original-byte evidence and process-kill
recovery remain distinct RED gates. Do not synthesize authoritative FileCatalog
paths from opaque legacy document ID or URI, and never promote FNV-shaped text
hashes to original-byte SHA-256.

## Bounded owned snapshot before the write owner

The private `ta_legacy_knowledge_batch_capture` reads the exact admitted
legacy `documents/chunks` schema from one **read-only SQLite handle**. It
uses the existing two-pass, pinned snapshot scan; the first pass validates
all authoritative rows and chunk coverage. During the second pass, a
mandatory Host admission callback accepts **every** source or rejects the
**whole** batch. It must not derive the Host principal, root, or grant from
legacy `document.id` or `uri`; this callback is a policy boundary, not a
durable ACL or revocation proof.

Before returning, capture **copies** all opaque IDs, SourceRevision metadata,
document provenance, chunk provenance and exact extracted text into bounded
owned memory, sorts source membership by binary IDs, and closes the source
snapshot transaction. Any schema, visitor, admission, memory or size failure
releases all partial copies; it cannot invoke a write owner. The budget
limits cumulative retained bytes and chunk count. No borrowed SQLite cursor
or temporary visitor view survives capture.

`ta_legacy_knowledge_batch_initial_request` builds one complete generation
request with `EXPECT_ABSENT` head CAS. The Host must independently revalidate
grants/epoch and open the qualified TurboDB write owner, then call the existing
`ta_revision_write_publish_generation`. Repeating a migration against a
nonempty published generation must conflict rather than become an implicit
second writer. Caller owns batch until publish returns, including uncertain
commit reconciliation.

The on-disk native C11 regression captures a two-document snapshot, checks a
denied row aborts without a partial batch, exercises a finite capture limit,
changes the legacy SQLite rows *after* capture, and proves the old provenance,
text and metadata still reach one fake writer without dangling read views. A
staged failure aborts before publish. This is **not** yet a real TurboDB
adapter, Host ACL revocation acceptance, source-byte retention proof, or
process-kill durability qualification. Production KnowledgeStore remains
unchanged.


## Confirmed-commit pinned readback (private gate)

After one confirmed publish (or COMMIT_UNKNOWN reconciled as COMMITTED using
a clean request-ID inspector), the caller must open a **different** TurboDB
read_at(commit_seq) owner. The private
ta_legacy_knowledge_batch_verify_readback() checks that the backend reports
the expected pinned sequence before and after retrieval, and compares the
EXACT generation manifest, SourceRevision records, legacy document provenance,
all Chunk KV text bytes and all chunk provenance records through that one
owner. Read item/byte budgets count every physical get. Missing bytes,
mutations, wrong commit, backend errors or exhaustion fail closed, with the
entire result cleared. No repair/retry or broad-scoped retrieval is attempted.

The existing native on-disk SQLite migration test adds a separate fake
write/read owner: it proves byte-identical post-publish verification, missing
provenance, corrupted metadata, mismatched commit sequence, limit rejection
before any get and typed backend LIMIT. This is *not* proof of real TurboDB
persistence. FTS tokenization, complete Graph projection/absence of ghosts,
Host ACL/revocation, verified original source bytes and multi-process power
loss remain independent, still-blocking acceptance gates. The batch is kept
alive through readback.
