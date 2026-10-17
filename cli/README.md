# TurboAgent CLI

The installed `turboagent` executable uses the existing Harness, Session workflows,
Provider/CHttp transport and CodingTools pack. Model settings come from the
existing `OPENAI_*` / `ANTHROPIC_*` environment configuration and optional `.env`.

```sh
turboagent -C /absolute/workspace -p "Explain why the tests fail" # inspect by default
turboagent --trust-profile trusted-local -C /absolute/workspace -p "Fix the failing tests"
turboagent --read-only -p "Explain this repository" # legacy inspect alias
```

`-C` / `--workspace` defaults to the current directory and accepts relative paths.
`-p` / `--prompt` runs one task; otherwise each input line starts a user turn.
`--model` overrides the environment model, `/quit` or EOF exits, and Ctrl-C requests
cooperative cancellation. Agent output goes to stdout; interactive prompts and
execution failures go to stderr.
The default HTTP transport checks cancellation and the execution deadline between
poll intervals of at most 50 ms, then cancels and drains the request on its owner
thread. A server response is not required to finish cancellation. This is not a
hard real-time latency guarantee: connection cleanup can take additional time.
Explicitly supplied blocking HTTP clients and custom transport functions still
depend on their callback returning; the CLI does not forcibly interrupt them.

Failed runs keep exit code 1 (130 for Ctrl-C) and now append a bounded diagnostic
with the known failure phase, run code, execution status, HTTP status and transport status. HTTP
401/403/404/429/5xx include a fixed configuration/access/rate-limit/server hint.
Provider response bodies, credentials and arbitrary phase text are not printed.
The default HTTP path records each call in the existing `provider_attempts`
state array and remains single-attempt; it does not automatically replay a task.
Custom v1 callbacks have no structured HTTP metadata, so their HTTP status is 0.

Only a `completed` execution can print an answer or become conversation history.
Runtime return code 0 may also represent a successfully recorded cancellation,
deadline expiry or workflow pause; those outcomes exit with 1 even if their state
contains model text. Ctrl-C exits with 130, including a signal observed after the
worker completes but before its answer is printed. Diagnostics distinguish these
outcomes without printing partial model text.
Plan-based workflows must also finish every step: exhausting the replan budget
exits nonzero and leaves the unfinished session marker intact, even when the
SDK reports that the graph execution terminated normally.

Argument parsing and generated help use the SDK's `Salts::CmdParser`
(`cmd_arger.h`). Long options accept both `--model name` and `--model=name`;
use the short aliases separately (`-C path`, `-p text`). CmdParser also expands
`@file` arguments and loads the current directory's optional `.env` before
parsing. Use `--prompt=@text` for a literal prompt starting with `@`.
The CLI adopts CmdParser's current process behavior: `--help` / `-h` and syntax
errors print to **stdout and exit with 1**, replacing the earlier help exit 0
and syntax-error exit 2/stderr behavior. Driver validation errors (such as an
empty prompt or invalid workspace) still exit with 2 and report to stderr.
Parsing occurs before any Agent resources are created; the execution driver
receives borrowed parsed options and does not call the terminating parser API.

CLI coding mode defaults to **`--trust-profile inspect`**, which registers only
`fs.read`, `fs.list` and `fs.search` and denies shell/patch authorization.
`--read-only` is an alias for this limited tool profile. To enable `fs.write`,
`fs.edit` and `shell.exec`, the host must explicitly pass
**`--trust-profile trusted-local`** (and not combine it with `--read-only`).
There is no interactive Host approval callback yet: `--trust-profile approval`
is rejected, not treated as implicit approval or an execution fallback.
File tools use retained workspace root capabilities; `fs.list` and `fs.search` accept `.`.
Search takes a literal `pattern` and a file/directory `path`, returns matching
lines, and reports limits/skips through `truncated` and metrics.
**Trusted-local shell commands have full host privileges**, inherited environment
and filesystem/network access. Changing `cwd` does not sandbox the process.
The trust selection comes only from host CLI arguments, never AGENTS.md,
Skills, retrieved content or the model output. Model-generated calls to tools
missing from the selected registry cannot execute.
The `inspect` guarantee concerns model-exposed *coding tools*. Explicit
`--session-db`, `--knowledge-db`, `--vector-db` and `--index-dir` parameters
still authorize their documented CLI-owned persistent storage/index I/O.
See [tool ownership, limits and failure semantics](../coding_tools/README.md).

In both coding and general mode, each user turn loads the selected workspace's root `AGENTS.md` and selects
matching entries from its `skills/` catalog using the existing Agent Workspace
module. The catalog is refreshed between turns, so later prompts see updated
instructions and select skills for the new task. This does not search parent
directories or apply every nested directory's instructions globally.
Missing `AGENTS.md` or the default `skills/` directory is allowed. Malformed
skill metadata, oversized files, unavailable declared tools, and denied skill
capabilities stop the turn before a model request. The default `inspect` profile also
denies skills requiring patch or shell capabilities.
General mode has an empty tool registry and denies patch/shell capabilities;
instruction-only skills work, while selected skills requiring coding tools fail
before a model request. Skill selection never executes bundled scripts.

For example, create `skills/release-summary/SKILL.md`:

```markdown
---
name: release-summary
description: Summarize release readiness
triggers: [release-summary, 发布摘要]
---
In the final answer, list the release owner, evidence, and unresolved questions.
Do not invent missing evidence.
```

Then run `turboagent --mode general -C /workspace -p "release-summary: summarize the supplied release notes"`.
Skills may declare `tools` and `capabilities`; these are checked against the
mode's available tools and policy, and do not grant new permissions.

The CLI owns the workspace catalog on its input thread. Preparation validates
skill tools against the same registry used for execution; it cannot expand
or override the chosen `inspect` / `trusted-local` tool set. The selected
text is copied into a project memory context layer on the fresh turn state,
then the temporary selection is freed.
Harness copies that state for its worker. After the worker drains, the next
turn projects conversation history without carrying forward old instruction
layers and refreshes the catalog. Workspace defaults bound composed text to
256 KiB, each skill to 128 KiB, the catalog to 128 skills, selection to 16 skills,
and discovery depth to 4. No second session store or instruction parser is added.

The CLI defaults to an in-memory session with bounded input and
execution: 64 KiB per prompt, 1 MiB projected conversation input, 64 user turns,
128 graph steps and 10 minutes per turn. Exceeding a bound fails explicitly.
It prints completed answers rather than incremental tokens. It does not expose
an approval UI or interactive TUI controls. When building with
CodingTools disabled, the CLI remains available without workspace tools.

Follow-up turns use `turbo_agent_session_create_follow_up_state_json_value` to
project the completed model/tool history before appending the new user message.
This is a new run of the selected workflow, not an interrupted-workflow resume. It retains full
history under the host limit rather than reusing a stale compaction event offset.
The Session's Runtime remains the record owner; the CLI retains only the current
completed state. After the single worker drains, the CLI queries its terminal
status and moves the summary/state exactly once. It frees the summary and every
rejected result; only a completed answer transfers state into the next turn.
The execution handle remains retained through this consumption boundary.

## Workflow selection

`--workflow` is independent of `--mode`; both general and coding support:

| Value | Execution |
| --- | --- |
| `loop` (default) | Model/tool loop |
| `review` | Plan, review gate, execute each step |
| `engineering` | The same plan/execute path plus failure detection and up to three replans |

`review` names the SDK's review gate, not a second model performing a critique.
The gate is open unless review is requested. The CLI does not automatically
approve requests or provide an approval UI: a paused run exits nonzero and is
not committed as a completed conversation turn.

```sh
turboagent --mode general --workflow review -C /workspace -p "release-summary: summarize these notes"
turboagent --mode coding --workflow engineering --trust-profile trusted-local -C /repo -p "Fix the failing test and verify the result"
turboagent --mode general --workflow engineering --knowledge-db knowledge.db -p "Summarize the Orion release policy"
```

Each planning turn seeds a separate planner input asking for JSON steps and a
final answer step. The planner prompt does not enter executor input or persisted
conversation replay. Skills and retrieved context are copied into planner,
executor, and replan substates, so all phases receive the current turn's evidence.
Substates own their copies; the input thread prepares context before submission,
and one worker runs the bounded graph. The 128-step and ten-minute limits still apply.
See the [composition and ownership decision](../agent/docs/workspace-skills-tools.md#cli-skills-and-workflow-composition).

FTS/vector retrieval stays on the existing input-thread preparation path for
all three workflows. The SDK's `KNOWLEDGE_ENGINEERING` and
`RETRIEVER_ENGINEERING` presets are not additional CLI choices: those graphs
query their own context backend inside execution. Reusing CLI pre-turn retrieval
preserves its HTTP ownership and avoids querying twice.

With `--resume`, history is restored and a fresh plan is made for the new task.
Pass `--workflow` on each invocation; it defaults to `loop` and is not saved as
a preference. Completed conversations may change workflow between invocations.
The session database format is unchanged; interrupted checkpoint resume/fork
remains an SDK feature.

## General assistant and retrieval

`--mode coding` is the default. `--mode general` uses the same Session/Harness
conversation and persistence path with general assistant instructions, without
registering coding tools. Both modes load workspace `AGENTS.md`/skills. `--instructions`
accepts nonempty UTF-8 text (up to 64 KiB) to override the mode's instructions.
It does not change tool permissions. Workspace instructions are still included;
coding mode defaults to `inspect`; `--read-only` retains that restricted
profile, and only explicit `trusted-local` enables editing and shell tools.

```powershell
turboagent --mode general -p "Explain how a hash table works"
turboagent --mode general --instructions "Answer in concise Chinese" -p "Explain indexing"

# Existing directories are required for the database parents.
turboagent --mode general --knowledge-db C:/private/knowledge.db --index-dir C:/docs --session-db C:/private/chat.db -p "What is the release process?"
turboagent --mode general --knowledge-db C:/private/knowledge.db --session-db C:/private/chat.db --resume -p "Explain the verification steps"

# The default coding agent can retrieve from the same kind of index.
turboagent -C C:/projects/example --knowledge-db C:/private/code.db --index-dir C:/projects/example -p "Explain the parser implementation"
```

`--knowledge-db` opens a dedicated persistent KnowledgeStore, separate from
`--session-db`. Before each turn, the CLI uses the current user message for
FTS5/BM25 retrieval and copies up to eight matching chunks with their source URIs
into that turn's context. Empty results are valid. Retrieved material is marked
as reference data with unverified source freshness. Old retrieval layers are not
carried into the next turn. The model still receives normal conversation history.
FTS5 must be available; this entry point refuses the SDK's LIKE fallback. It does
not need an embedding provider or enable vector/hybrid retrieval automatically.
Knowledge mutation tools are not registered by these flags.

`--index-dir` requires `--knowledge-db` or `--vector-db` and imports at startup,
before any chat model call. Paths are relative to the process working directory, independently of `-C`.
It recursively reads regular UTF-8 files through Salts no-follow root handles.
Symlinks/reparse points, special files, hidden names, `build`, `node_modules`,
`vcpkg_installed`, `CMakeFiles` and `cmake-build-*` directories are skipped.
Supported lowercase extensions are `.txt`, `.md`, `.rst`, `.c`, `.h`, `.cc`, `.cpp`,
`.cxx`, `.hpp`, `.hxx`, `.py`, `.rs`, `.go`, `.java`, `.js`, `.ts`, `.jsx`, `.tsx`,
`.html`, `.css`, `.json`, `.yaml`, `.yml`, `.toml`, and `.cmake`. This is an explicit
directory import, not a Git-aware scan: `.gitignore` is not interpreted.

An import admits at most 16,384 directory entries, 1,024 supported files, 256 KiB
per file, 16 MiB total text, 32 directory levels (including the root), and 4,096
bytes per relative path/source URI. Exceeding these limits, unreadable selected
files, binary text or invalid UTF-8 stops startup. KnowledgeStore commits each
document separately; earlier successful imports remain if a later file fails.
Reimport replaces each encountered document by its absolute source identity.
Deleted or renamed files are not automatically removed from an existing index.
Use the SDK to manage those records. There is no filesystem watcher; reimport
explicitly when sources change, and use the same source-path spelling.

Retrieval admits at most 64 KiB of matching text plus bounded source URIs; excess
or invalid records fail before model submission rather than produce a partial
answer. Existing byte chunks may split UTF-8 characters: incomplete boundary
characters are omitted from model snippets, without modifying indexed content.
The index stores plaintext source content and uses KnowledgeStore's existing
SQLite format. Use a dedicated database outside the imported tree; do not point
it at a session database or another application's database. Admission rejects
another application ID or a populated database without KnowledgeStore's core
tables before the SDK can create its schema. This is a format guard, not a
concurrent database replacement or corruption recovery protocol.

Mode, instructions and knowledge options are launch settings: pass them again
with `--resume`. The existing completed-turn-only resume rules and lifetime limits
still apply. None of these execution options can be combined with `--diagram`.
`test_turbo_cli_knowledge` covers real SQLite import/query and reopen, per-turn
context isolation, general/coding modes, UTF-8 boundaries and admission limits
using the formal injected model transport.

## Persistent vector retrieval

Both coding and general modes support `--vector-db FILE`, together with
`--embedding-url URL` and `--embedding-model ID`. This selects vector retrieval
instead of `--knowledge-db`; combining the two is rejected. The endpoint must be
the complete OpenAI-compatible embeddings URL, not a chat API base URL. HTTPS
uses the transport's default certificate verification. Plain HTTP is accepted
only for literal `127.0.0.1` or `::1`; userinfo, query strings and fragments are
rejected. Responses must identify exactly the configured model.

```powershell
# Configure the chat model as usual. Set TURBOAGENT_EMBEDDING_API_KEY in the
# process environment separately when the embedding service requires a key.
turboagent --mode general --vector-db C:/private/vectors.db --embedding-url https://api.openai.com/v1/embeddings --embedding-model text-embedding-3-small --index-dir C:/docs --session-db C:/private/chat.db -p "Explain the release process"

# Reuse the persisted vectors and resume the completed conversation.
turboagent --mode general --vector-db C:/private/vectors.db --embedding-url https://api.openai.com/v1/embeddings --embedding-model text-embedding-3-small --session-db C:/private/chat.db --resume -p "What checks are required?"
```

Embedding credentials are read only from the process environment variable
`TURBOAGENT_EMBEDDING_API_KEY`; chat credentials and `.env` loading are not reused
for this adapter. Imported text chunks and each query are sent to that endpoint,
which may charge for embedding requests. Reimport embeds every encountered file
again. Omitting `--index-dir` reuses stored vectors and only embeds each new query.
An empty index still embeds the query and contributes no source evidence.

The shared scanner and source limits above apply. Text uses a 2048-byte chunk
target with three bytes of overlap. Incomplete UTF-8 boundaries are trimmed from
embedding inputs and retrieved snippets; the overlap keeps every complete source
character in at least one chunk, including a final character crossing a boundary.
Existing indexes remain readable; explicit reimport applies the overlap and
replaces that document's old chunks. Each turn retrieves the top eight chunks
by exact cosine similarity, with the same context/source bounds as FTS retrieval.
This is a bounded exact scan, not an ANN or hybrid index. An embedding input is
limited to 8192 bytes, a response to 256 KiB, and a vector to 1..4096 finite
components with nonzero norm. Provider token limits still apply. Requests have a
60-second deadline; startup import has a ten-minute deadline. There is one
outstanding request, no automatic retry and no fallback to hashing or FTS.

The dedicated database stores plaintext source chunks, vectors, the exact
endpoint/model identity and the first accepted dimension count. It admits at
most 8192 chunks and 64 MiB of serialized row data; import reserves conservative
space for serialized numbers before embedding. Dimensions must remain stable.
On open, stored vectors are checked against the pinned dimension and the CLI's
finite, unit-norm representation before any embedding or chat request. Malformed
vectors fail admission; the CLI does not silently repair or discard records.
Changing endpoint/model requires a new database and explicit reimport; existing
session, FTS and unmanaged SDK vector databases are refused without migration.
Keep the vector database separate from the session database and imported tree.
Repeat all vector settings on `--resume`; they are launch settings.

A companion `FILE.lock` advisory lock is held until the CLI closes the index;
the empty lock file may remain after exit. Use one CLI owner and the same path
spelling. The lock does not coordinate path aliases or external SDK/SQLite
writers. Do not modify the database while the CLI is using it. Each document is
replaced transactionally after all its embeddings succeed; a failed replacement
preserves its previous chunks, while earlier successful documents remain.
Reimporting an empty file removes its chunks. Deleted/renamed source files are
not reconciled automatically; use a fresh database to rebuild from current files.
Request or index failures prevent chat submission. Ctrl-C is checked before
requests and while polling, followed by transport drain before cleanup. The
signal handler sets a lock-free atomic cancellation flag; startup verifies that
the flag is lock-free before installing the handler. Cancellation during query
embedding or before replacement embeddings finish exits with code 130, submits
no chat request, and keeps the previous completed session and document usable.

`test_turbo_cli_vector` uses a loopback HTTP fixture, real SQLite and an injected
chat transport. It covers semantic ranking, coding/general context, session and
index reopening, corrupt stored vectors, numeric normalization, failed
multi-chunk replacement, Unicode paths/chunk boundaries, identity/dimension
validation, provider errors and input/response/index bounds. Cancellation tests
inject SIGINT into the process while a local service holds an embedding response
open, then verify driver exit, old-document retrieval and completed-session
resume. They cover query embedding and the second chunk of a replacement import.
Subprocess tests verify that a competing owner is refused before HTTP or chat,
in both parent/child directions. Reopening retrieves committed documents after
orderly close, abrupt exit without cleanup, or forced termination. Terminating
an importer while its second replacement embedding is pending also preserves
the old document. These tests do not cover power loss or interruption inside a
SQLite commit, a terminal's physical Ctrl-C delivery, real embedding quality,
or a cloud deployment.

## Persistent conversations

Choose a session database in an existing private directory outside the workspace:

```sh
turboagent -C /absolute/workspace --session-db /private/agent/task.db -p "Inspect the failing test"
turboagent -C /absolute/workspace --session-db /private/agent/task.db --resume -p "Continue the fix"
```

`--session-db FILE` opts into the SQLite RuntimeStore. Relative database paths
are relative to the process working directory, independently of `-C`.
`--resume` requires an existing session file and the same resolved absolute
workspace string. Use the same workspace path spelling on subsequent launches.
Opening an initialized CLI session without `--resume` fails without resetting it.
An open database has one exclusive owner; a second process fails after a bounded
lock wait. Invalid files, incompatible versions and storage errors stop startup.

The file stores the existing thread, run, checkpoint and tool-journal records,
plus one versioned `cli_sessions/current` record with a thread ID, workspace,
completed-turn count, exact last-run ID and `ready`/`active` phase. Each new turn
commits `active` before submitting work. Only successful completion publishes
the exact run as `ready`, before printing its answer. `--resume` validates that
ID against the Runtime's published `thread_heads` snapshot and loads that snapshot
and projects the normal model/tool history; it starts a new turn. Workspace
instructions and model configuration are loaded again. The 64-turn limit applies
across launches, not separately to each process.

Head publication follows checkpoint/run persistence and summary construction.
It stores a full run snapshot so failed same-run updates cannot change the
previously published state. Timestamps and UUID ordering do not select the head.
Older files without a head record retain exact-last-run lookup; an empty,
corrupt or unreadable head is an error. Each head counts toward record/database
quotas and duplicates one run snapshot per thread.

A failed, cancelled or abruptly terminated turn leaves `active`. The CLI refuses
automatic continuation of that file, even when the failure occurred before a
tool ran. Inspect the records through the SDK before choosing recovery, or use
a new session file. Do not manually clear the marker to replay unknown effects.
Explicit SDK checkpoint resume remains available when a complete checkpoint was
published. This CLI flag does not resume an interrupted graph, and a crash does
not guarantee a checkpoint for every model response or tool boundary.

Defaults are 16 MiB per record, 64 MiB per list, 4096 scanned rows per list, and
256 MiB for the main database. Limits fail explicitly; no records are evicted.
The rollback journal requires additional disk space. SQLite uses rollback
journaling and full synchronization; no FTS5 tables are needed for these records.
The existing KnowledgeStore owns its search index separately.

Conversation text, tool results and workspace instructions may be sensitive.
The file is plaintext and uses the directory's filesystem access controls;
it is not a credential vault. Keep it outside source control and the coding
workspace. There is no automatic migration from memory/file stores, full-session export,
retention cleanup or CLI recovery editor. Close the owning process before copying
the database; preserve any SQLite recovery files after an abnormal exit.

### Diagram export

Export the existing session's published run/checkpoint lineage without a model
request, tool execution, workspace loading or session-state mutation:

```powershell
turboagent --session-db C:/private/session.db --diagram mermaid > session.mmd
turboagent --session-db C:/private/session.db --diagram json > session-graph.json
```

`mermaid` writes a flowchart body; wrap it in a Mermaid Markdown fence to display
it in a compatible viewer. `json` includes the schema version, view kind,
thread/session phase, nodes, edges and source record references. Generated
Mermaid node `n0` maps to JSON `nodes[0]`, and similarly for the remaining nodes.
The flowchart shows a published run snapshot and its checkpoint parent chain,
not every model/tool step or every conversation turn. A completed run without
checkpoints produces one node. An `active` session is explicitly labeled and
may show only the previously published snapshot; unpublished work is excluded
unless the current journal projection below is explicitly selected.

To discover tool journals for the published run, use `all`. JSON exposes exact
`agent_tool_journal` record keys for subsequent explicit selection:

```powershell
turboagent --session-db C:/private/session.db --diagram json --diagram-journals all > run-evidence.json
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-journals all > run-evidence.mmd
turboagent --session-db C:/private/session.db --diagram json --diagram-journals "$journalId1,$journalId2" > tool-evidence.json
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-journals "$journalId1" --diagram-focus "journal:$journalId1" --diagram-direction incoming > tool-evidence.mmd
```

`--diagram-journals` requires `--diagram` and accepts `all` or 1..32 distinct,
comma-separated 64-character lowercase hexadecimal keys, without spaces or empty
entries. Explicit selection performs point reads without a discovery scan. Every selected record must match the
published run's thread/run identities; missing, malformed or mismatching records
fail with empty stdout, including when focus would exclude them. Input order is
preserved. Selection and lineage share the existing graph limits.

`all` discovers records with a `run_id` matching the Runtime's actual published
head, even when the CLI's last-completed-run marker is stale. It does not include
other runs, unjournaled calls, or records whose run identity is absent or malformed.
Keys are sorted lexically for repeatable output, not execution order. More than
32 matches fail; no partial set is returned. The existing SQLite list operation
scans the journal collection under its 4096-row scan, 64 MiB result and 16 MiB
per-record limits, so other runs can still cause a scan-limit failure. The CLI
does not bypass these bounds or create a new index. JSON marks
`journal_scope: "published_run_records"` and `journal_count` before focus filtering.
Zero matches produce the published lineage with count zero. The database remains
exclusively owned throughout discovery and validation. This is a snapshot of
stored matching records, not a claim that every intended action was journaled.

These nodes are **current journal observations**, not frozen checkpoint contents
or proof of a compiled plan step. `same_run` edges express matching identities.
`started` leaves side effects unknown; `committed` may still have a failed status.
JSON retains the journal key, call/turn identity, arguments hash, phase, status,
policy and timestamp, with `record_scope: "published_and_current"`. Mermaid labels
retain that distinction and the `active`/`ready` session phase. Arguments, output
and detail payloads are excluded. This option invokes the Agent SDK's
`turbo_agent_thread_tool_diagram`; it neither executes nor retries tools, clears
the active marker, nor changes stored records.

Add `--diagram-origins` to resolve scoped planner/executor journals against the
published model requests, for example:

```powershell
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-journals all --diagram-origins > request-origins.mmd
```

This flag requires `--diagram-journals`. A verified request appears as
`event:<turn_key>` with `snapshot_contains` and `same_call_identity` edges.
JSON includes exact `thread_heads` source pointers and journal `origin_status`:
`matched`, `not_in_snapshot` (the current journal may be newer), or
`unresolved_identity` (ordinary/legacy unscoped keys). No root/nested ownership
is guessed for old keys. Malformed scopes, conflicting tool names or duplicate
matching call IDs fail before output, even outside the requested neighborhood.
At most 64 calls are inspected in each selected event; combined graph limits
still apply. This verifies call identity only: arguments may have changed in
middleware, and neither tool success nor a compiled DAG step is established.
Prompt, argument and result payloads remain excluded. Existing exports omit
origins by default; no records are migrated or changed.

`--diagram-plan-steps` additionally links explicitly recorded engineering
workflow steps to model requests and their selected journals. It requires
`--diagram-journals` and implies `--diagram-origins`:

```powershell
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-journals all --diagram-plan-steps > workflow-evidence.mmd
```

New workflow executor substates record their plan version and step index at
preparation, before model/tool execution, and retain them in checkpoints.
Matching step nodes use `workflow-step:<version>:<index>` and exact source
pointers. `prepared_for_step` edges retain the binding location. Model event
`step_origin_status` is `matched`, `unrecorded` for older executor states, or
`not_applicable` for planners. Invalid bindings on matched requests fail before stdout,
including outside a selected neighborhood/path; old bindings are never guessed.
No plan-step text is exported. A prepared workflow step is distinct from a
compiled DAG step and is not proof of approval or successful execution.
Ordinary CLI tool loops do not create engineering plan-step bindings.
Combine with `--diagram-focus "workflow-step:0:0" --diagram-to "journal:$journalId1"
--diagram-direction outgoing --diagram-depth 2` for one step-to-call path,
using exact IDs from JSON. Existing graph size limits still apply.

To inspect a local neighborhood, first get the exact `nodes[].id` from JSON,
then use it as the focus (replace `checkpoint:ID` below with that value):

```powershell
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-focus "checkpoint:ID" --diagram-direction incoming --diagram-depth 2 > checkpoint.mmd
```

`--diagram-direction` accepts `incoming`, `outgoing` or `both` (default).
Add `--diagram-to ID` to select one minimum-hop path from the focus to another
exact node ID instead of a neighborhood:

```powershell
turboagent --session-db C:/private/session.db --diagram mermaid --diagram-journals all --diagram-origins --diagram-focus "event:executor:0:0" --diagram-to "journal:$journalId1" --diagram-direction outgoing --diagram-depth 8 > call-path.mmd
```

Use IDs from the saved session's JSON; the example IDs are not guaranteed to
exist. Paths preserve original edge directions and evidence, including when
traversed backwards with `incoming` or `both`. Only the chosen path edges are
included. Equal-length alternatives use source edge order at each distance
layer for deterministic selection. This is an unweighted relationship path,
not an execution order or causality claim. Unknown endpoints or no path within
the direction/depth limit return exit code 1 with empty stdout; invalid options
return 2. The entire diagram and selected journal evidence are validated first.
Equal start/end IDs produce one node and no edges, including at depth zero.
`--diagram-to` requires `--diagram-focus` and `--diagram`.

Path JSON records `selection.mode: "shortest_path"`, `target_id`, `hop_count`,
input/omitted counts and `scope: "input_view"`. Array order remains source order;
node `selection_distance` gives its position along the path. Querying an already
filtered view cannot rediscover omitted nodes or prove global disconnection.

`--diagram-depth` accepts one to three decimal digits in the range 0..127
(default 1 for both queries). In a neighborhood, zero shows just the focus and
any self edges; a path at zero requires identical endpoints and has no edges. In session diagrams,
incoming follows older checkpoints; outgoing follows newer checkpoints toward
the published run. The SDK's `turbo_diagram_neighborhood` also accepts compiled
plan views: incoming finds prerequisites, outgoing finds dependent steps.
These are graph relationships, not proof that a planned tool actually ran.

Filtering preserves node/edge order, original edge directions and source
metadata. JSON nodes include `selection_distance` (minimum hops); root
`selection` reports the focus, direction, depth and omitted node/edge counts.
Counts refer only to the supplied view, never the entire repository or session
history. The Mermaid title marks focused views. Renderer IDs such as `n0` are
reassigned after filtering: use source IDs for queries. Direction/depth require
a focus; focus requires `--diagram`. An unknown focus exits 1 with empty stdout.
The complete published lineage is still loaded and validated before selection;
focusing cannot bypass a broken or oversized source graph.

Do not combine `--diagram` with `--resume`, `--prompt` or `--model`. Close the
process owning the database first: export uses the same exclusive SQLite
connection contract and does not open a second live-reader connection. Missing
databases are not created. Old sessions without an explicit published head,
empty first-run heads, broken ancestry and size limits return an error instead
of guessing a graph. Invalid options exit 2; export/storage/output errors exit 1.

Views are bounded to 128 nodes, 512 edges and 2048 UTF-8 bytes per ID/label;
Mermaid output is bounded to 1 MiB. Validation/render failures produce no partial
diagram; stdout I/O errors can still leave partial output. State, prompt and tool
payloads are excluded. The output contains no embedded click callbacks or HTML
viewer; applications can build navigation from JSON `collection`/`record_id`.
For SDK neighborhood queries, opaque input metadata is additionally bounded to
8192 JSON values, 32 nesting levels (root depth zero), and 1 MiB of total
key/string/number-token bytes before copying. A budget failure returns an error
instead of a truncated view. No persistent graph index or interactive editor is
introduced by these filters.

Formal tests inject the existing transport callback and execute actual tools in
a temporary workspace. They cover write/edit/read/command execution, second-turn
history order, read-only schema exposure, one-shot mode, model overrides, and
CmdParser defaults/aliases/equals syntax. Child-process tests verify generated
help, rejected arguments, output streams and exit codes
without making a paid model request.
Workspace tests cover instruction refresh, skill reselection without stale
layers, read-only capability checks, missing tools, malformed/oversized files,
and failure before model invocation, including a failed refresh on a later turn.
They also cover general-mode selection without tools, both modes' review and
engineering plans, skills/FTS propagation to every phase, failure replanning,
exhausted-replan refusal, malformed plans, and completed-history resume.
Invalid workflow names fail before opening storage.
`test_turbo_cli_diagnostics` uses a local CHttp server to check failure categories,
response-body and terminal-control-character isolation, and preserved stdout on
a failed second turn. These tests do not require a model or credentials.
`test_turbo_cli_completion` runs the real asynchronous Harness to cover late
model responses after cancellation, another run after cancellation, deadline
expiry, workflow pause, a signal observed at completion, and one-time result
consumption. `test_turbo_agent_http_cancel` holds a local server response open to
verify user/shutdown cancellation and in-flight deadlines, then resumes the same
Session on another worker. It checks that cancellation commits no model response
and admits no retry; a second POST occurs only after explicit resume.
`test_turbo_cli_tool_cancel` runs a real command through Harness, cancels it
after stdout/stderr and a side effect have been produced, and checks that the
child is gone before completion. User/shutdown/deadline cancellation retains
bounded process detail in the existing tool journal. Explicit checkpoint resume
does not launch the command again or expose host-only detail to the model.
Pre-spawn deadlines, binary capture and legacy context compatibility are also
covered by the CLI/CodingTools suites. The persistent CLI flag continues completed
conversations; these interrupted-tool resume cases remain explicit SDK operations.

`test_turbo_cli_storage` covers completed conversation continuation, workspace and
format admission, published-run validation, unfinished-run refusal, and lifetime
turn limits. Its subprocess fixture exits without cleanup during a model call
to verify that the active marker survives process termination. Runtime SQLite
tests cover checkpoint reload, exclusive ownership, record/list/database bounds,
schema admission and failed-update rollback. Tool journal tests reopen SQLite
after successful execution and failed result persistence to check that side
effects are not repeated. These are process/storage tests, not power-loss or
physical-device durability tests.

## Optional live model verification

`test_turbo_cli_live` is a separate CTest integration test. It is skipped unless
`TURBOAGENT_LIVE_TESTS=1`; having a key in `.env` alone never enables it. It launches
the real CLI with the repository root as its initial working directory so
CmdParser can load that `.env`. `OPENAI_BASE_URL`, model and provider settings
follow the CLI's normal environment configuration. For a local proxy, use the
client key accepted by that proxy. Python 3 must be available as `python` on PATH.
The suite first checks a general question through synchronous Session and
background Harness execution, then exercises both CLI modes with real tasks,
including a tool read whose answer is present only in a temporary file.

```powershell
$env:TURBOAGENT_LIVE_TESTS = "1"
# Example for an OpenAI-compatible local Chat Completions proxy:
$env:OPENAI_BASE_URL = "http://localhost:64263/v1"
$env:OPENAI_PROVIDER = "openai_compatible_chat_completions"
$env:OPENAI_ENDPOINT_PATH = "chat/completions"
# Select a tool-capable model listed by this proxy.
$env:OPENAI_MODEL = "gpt-6.1-sol"
ctest --preset win-release-user -R '^test_turbo_cli_live$' --output-on-failure
Remove-Item Env:TURBOAGENT_LIVE_TESTS
```

To run the same cases through Responses, set `OPENAI_PROVIDER=openai_responses`
and `OPENAI_ENDPOINT_PATH=responses`. The recognized path variable is
`OPENAI_ENDPOINT_PATH`; `OPENAI_API_ENDPOINT` is not read. Run each provider
separately; the suite never switches interfaces after a failed request.

Six skill/workflow cases exercise general and coding mode with each of `loop`,
`review`, and `engineering`. A temporary skill supplies a verification label
absent from the user prompt. Tests check the real answer, persisted context,
and completed plan/executor history for non-loop workflows.

Two additional vector cases (general and coding) are enabled by explicitly
setting both `TURBOAGENT_LIVE_EMBEDDING_URL` and
`TURBOAGENT_LIVE_EMBEDDING_MODEL`. Set the service's key separately in
`TURBOAGENT_EMBEDDING_API_KEY`. These cases use the real vector importer and
query embedding transport, then repeat the persisted-runbook/resume assertions
below. Missing both settings skips them; incomplete settings or service failure
fail the cases. No synthetic vectors or FTS fallback are used.

The embedding URL must be the complete service endpoint, such as
`http://127.0.0.1:64263/v1/embeddings` **if that client actually implements it**.
Plain HTTP requires the literal loopback address `127.0.0.1` or `[::1]`, as for
normal CLI vector retrieval. A client offering Responses/Chat, or a model
appearing in its chat model list, does not establish embedding support. Select
an embedding model exposed by that service rather than copying `OPENAI_MODEL`.

The general-mode case imports a synthetic Orion release runbook into FTS5 and
asks for its approval owner and canary duration, with a source citation. It then
removes the temporary source document and launches another CLI process with
`--resume`, without `--index-dir`. That turn must retrieve the persisted runbook,
decide whether an observed error rate requires rollback, report its rollback
code and recall a ticket supplied only in the first turn. Assertions check the
answers, both persisted completed turns, fresh retrieval layers and zero tool
calls. The facts come from the generated fixture, not a production runbook.

The coding repair cases run with both `loop` and `engineering`. Each creates a
temporary Python program with a known off-by-one bug and first confirms the
four-test suite fails. The model must inspect/edit the program and
run the tests through workspace tools. The test then checks that the test source
was not modified, checks the success artifact and independently reruns the tests.
It also verifies recorded `fs.read`, `fs.edit` and `shell.exec` calls in the
persisted completed run, resolving provider wire aliases through the registry.
Only generated fixtures are sent as task content; no repository source or
`.env` is copied into its workspace. The CLI retains its ordinary host-shell
capabilities. Process output is captured without printing it into CTest logs.
Each retrieval-case process is bounded to 90 seconds, each skill/workflow process
to two minutes, and each coding repair process to three minutes. The complete
CTest suite has a fifteen-minute deadline. Temporary files are
removed after the process tree stops on normal test completion or assertion
failure; external termination of the test runner can leave the fixture behind.

The live test can incur model usage charges and depends on endpoint availability,
credentials, model access and model behavior. A passing small repair task does
not establish success rates on larger repositories. Run normal regression without
`TURBOAGENT_LIVE_TESTS` to keep it offline.
