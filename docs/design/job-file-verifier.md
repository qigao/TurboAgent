# Rooted local file verification for bound Jobs

Status: implemented Host adapter for the [mandatory verifier](job-host-verifier.md).
`turbo_agent_job_file_verifier.h` exports an opaque adapter, bounded configuration,
complete borrowed callbacks and SHA-256 authoring helpers. It reads local files;
it does not fetch URLs, execute commands, choose quality decisions, send messages
or verify remote delivery receipts.

## Ownership and trust

Create an adapter with existing Host-admitted workspace_root and verification_root,
export callbacks, and supply them to create_verified_with_store(). Release every
borrowing Harness/execution before destroying the adapter. Host serializes adapter
operations and destroy. Configuration/root strings are not borrowed after create;
the exported adapter user_data is borrowed. No manifest is read during construction,
and no source, evidence or manifest file is created, repaired or substituted.

Salts root-relative capabilities retain both directories. Host must keep its
verification root trustworthy and pin/serialize source/evidence views through each
synchronous operation. Root handles and before/after file metadata checks do not
provide an atomic filesystem snapshot or a distributed fence. Reads have byte/count
bounds, not hard I/O deadlines: use trusted regular files on an appropriately bounded
local filesystem. General cancellation supervision remains separate.

Missing roots/files return ENOENT, malformed/stale data EPROTO, future manifest
schemas ENOTSUP, configured byte/count limits EFBIG, and allocation failure ENOMEM.
Recognized native permission/link failures map to EPERM; other native I/O errors
map to EIO. No verification error permits fallback, automatic retry or repair.

## Source manifest pinned by the proposal

For Job J, verification_root contains `J.sources.json`. The proposal freezes
`source_revision = "sha256-" + SHA256(exact manifest file bytes)` (71 bytes).
Formatting changes require a new proposal before binding; a bound Job cannot
silently adopt replacement manifest bytes. Schema 1 has exactly five fields:

| Field | Value |
| --- | --- |
| schema_version | 1 |
| kind | job_file_sources |
| job_id | J |
| source_json_sha256 | 64 lowercase hex SHA-256 of exact frozen source_json bytes |
| files | Nonempty workspace-root-relative file entry array |

Each entry has exactly `path`, `byte_count` (nonnegative integer), and `sha256`
(64 lowercase hex). Files may be binary, include NUL and be explicitly empty.
The adapter checks actual open-file size, streams SHA-256, detects growth/premature
EOF, and rechecks metadata. Content is hashed even if length/mtime are unchanged.
Freshness covers only the declared file set: Host owns completeness and its
relationship to the Inspect tool's real inputs. This does not implicitly cover an
entire repository, dependencies or environment. The pinned digest is not a signature.

## Evidence manifest and actual artifacts

For evidence_ref E, verification_root contains `E.evidence.json`. E is an identifier
of 1..64 ASCII letters/digits/underscore/hyphen, never an arbitrary path. Schema 1
has exactly twelve fields:

| Field | Required relationship |
| --- | --- |
| schema_version, kind | 1, job_file_evidence |
| job_id, job_revision, proposal_version | Exact bound Job/proposal |
| source_revision | Exact frozen source manifest identity |
| decision, evidence_ref | Explicit accepted/rejected Host decision and E |
| criteria_sha256 | Digest of the Job's acceptance_criteria JSON string value |
| result_sha256 | Digest of actual validated canonical typed result JSON value |
| execution_reference | Exact thirteen-field Job execution reference |
| files | Nonempty verification-root-relative artifact entry array |

Artifact entries use the same path/count/hash format. Every reference field is
compared by value, independent of that object's key order. Criteria/result digests
use exact compact TurboParser serialization through `digest_json`; JSON strings
include serialization quotes/escaping and object insertion order is preserved.
This is not RFC 8785 or an order-independent semantic hash. An equivalent reordered
result representation fails closed and requires explicit Host reconciliation.
Raw program/manifest/file digests use `digest_bytes` or standard SHA-256 on exact
bytes. The helper output buffer has 65 bytes and must not alias its input.

Evidence artifacts are reread and hashed, not merely named. A missing/changed report,
replaced result/Run reference or unrelated criteria refuses current acceptance and
delivery reads. Host explicitly authors the decision: this adapter checks content
integrity/identity and cannot interpret an arbitrary report to establish semantic
quality. Manifests remain Host-trusted, without signatures or a second canonical
result. Remote receipt verification and outbound authority remain separate.

## Bounds and paths

| Limit | Default | Hard maximum |
| --- | --- | --- |
| Manifest bytes | 64 KiB | 256 KiB |
| Files per check | 32 | 64 |
| Bytes per declared file | 4 MiB | 64 MiB |
| Aggregate declared file bytes per check | 16 MiB | 256 MiB |

All configured limits must be positive. Manifest JSON refuses NUL and nesting over
eight levels before DOM parsing. Empty file sets, exact duplicate paths and unknown
fields/shapes are refused. Paths have at most 1024 bytes, use `/`, and reject
absolute/empty/dot/dot-dot components, backslash, drive/ADS colon, controls and
trailing dot/space components. Salts resolves components through retained handles
and rejects symlink/reparse traversal below the admitted root. The adapter never
emulates this with root + untrusted path concatenation. This is not an inode or
Unicode-alias uniqueness claim. File bounds exclude the already supplied canonical
result's JSON serialization allocation and do not bound filesystem latency.

## Qualification and remaining scope

Unit tests use real separate directories with spaces, binary/empty files, same-size
replacements, raw manifest-byte pinning, every evidence/reference identity, missing,
future, malformed, deep and NUL manifests, path rejection, duplicates and all four
limits. POSIX additionally exercises a real file symlink; platform-specific
link/reparse behavior remains that of the released Salts facade.

Nine existing delivery crash cuts also run with this adapter. Ten fresh processes
per case include source-byte replacement, an unlinked evidence manifest and
same-size report replacement; only test Host restores its external files. The
Inspect callback reads the actual source file, Runtime evidence stays in SQLite,
and an independent test destination retains send counters/acknowledgements.
Recovery keeps one original Inspect callback and one Run; a post-send crash adds
no send request. This qualifies the local adapter and Host composition, not a
production remote connector or arbitrary report's semantic quality.

The macOS SDK profile now enables `TURBOAGENT_JOB_FILE_CONFORMANCE` while the full
suite stays OFF. It builds the real file unit executable and compiled process
fixture alongside the existing independent 58-case SQLite VFS gate. A required
`ci-macos-job-files` step runs on actual Darwin arm64 and checks the build's arm64
architecture and clean checkout commit. Android/cross builds cannot enable this
native conformance option.

`cmake/ci/run-job-file-conformance.py` requires an exact independent ten-test
inventory, refuses disabled/missing/duplicate/extra tests, validates CTest exit and
JUnit without skips/failures, and audits the nine cut traces for all ten ordered
child exits (including 86/87 crash/lost-ack exits). Every child emits a separate
log and stage exit record; the gate validates 90 application-process records.
Fresh per-run/attempt evidence contains inventory, JUnit, CTest log, child logs,
cache fingerprint, checkout/host identity and SHA-256 evidence fingerprints.
Successful old artifacts cannot be reused; failed cases/logs remain available.
CI uploads the evidence even on failure. SDK compilation or a Linux simulation
cannot satisfy the macOS host/architecture requirement. The first macOS result
for this added gate must be observed before claiming macOS adapter runtime coverage.

Runtime schemas remain unchanged. Rollback retains manifests and all canonical
Job/attempt/Run/review/delivery data; older/manual constructors retain their own
verification responsibilities. M1, cancellation supervision, amendment/replanning,
semantic quality qualification and production connectors remain open. Real-model
quality/cost/latency stays in #278; these tests make no paid model calls.
