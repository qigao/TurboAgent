# Host-selected Session context for model tasks

`turbo_agent_session_create_task_input_state_json_value()` creates a Session-compatible
input for an explicitly selected Host goal, memory keys and KnowledgeStore query. It
is an installed API, usable with ordinary Session/Execution graphs or the Router.
It starts no graph, model or tool and writes no Runtime, memory or knowledge record.

The existing memory input factories load context layers. The existing knowledge
context builder queries once for layers and again for evidence. This task factory
instead reads each exact selected memory key once, queries knowledge once, and keeps
source identities in a versioned **user-message data packet**. This avoids a second
retrieval being mistaken for the evidence underlying the first, and lets a Host
pass the same observed data to different model workers without installing another
state/result store. Existing factories and query behavior are unchanged.

## Selection and bounds

Initialize `turbo_agent_task_context_options_t`, select 0..8 distinct memory keys in
one exact explicit namespace, and choose `knowledge_limit=0..8`. Zero disables that
source even if the Session has stores configured. No namespace prefix scan, implicit
memory namespace, conversation-history lookup, generic Retriever, source substitution,
or read/list/query fallback is installed. A runtime-only Session is sufficient.

Memory reads use the existing Session exact get API. The stored value must be a formal
context object with `scope`, `path`, `text` and optional `created_at`, with no other or
duplicate fields. Scope/text are nonempty strings; path can be empty/null; created_at
can be a nonempty string/null. Original parsed strings/keys are checked for embedded
NUL before normalization, so truncated canonical memory text cannot conceal a rejected
raw value. The packet retains the exact requested namespace/key and the checked value.
Unsupported arbitrary JSON memories are rejected rather than interpreted as context.

Knowledge selection uses the Session's configured KnowledgeStore, query, kind and URI
prefix, all required and nonempty. Query once with the explicit bound, require matches,
and retain observed order plus document_id/chunk_id/uri/kind/title/text. No score,
document hash or source freshness is invented. The factory checks every result's exact
kind and literal case-sensitive URI prefix, independently of the backend's SQL LIKE
matching. A wildcard/case-expanded match outside that literal prefix fails the whole
operation; it is not silently filtered or retried under another query.

Goal and each selected text are at most 64 KiB. Selectors and identity/metadata fields
are at most 1024 bytes. Serialized task data, including escaping, is at most 1 MiB.
Selected raw memory JSON has a conservative escaped-record bound before parsing.
Excess fails without truncation. These are projection bounds: custom stores may block
or allocate before returning; SQLite query work and database size are not bounded by
the output limit. No transaction spans memory keys or stores. The packet explicitly
says `source_semantics: selected_read_observation`, not a globally atomic or current
source snapshot. Revision-sensitive applications must arrange their own immutable
source/version selection before calling.

All Session/backend use, updates and close must be serialized at a quiescent safe
point. The function borrows dependencies only for its synchronous call. All selection
and configuration validation precedes reads; later read/validation/allocation failures
discard the entire intermediate packet. Successful state owns its data independently
of Session and stores and has one canonical user message, zero events and no memory
context layers. Its shape is:

```json
{
  "schema_version": 1,
  "host_goal": "Host-selected goal",
  "source_semantics": "selected_read_observation",
  "memory_records": [{"namespace": "project/demo", "key": "notes", "value": {
    "scope": "project", "path": "notes.md", "text": "Selected memory"
  }}],
  "knowledge_matches": [{"document_id": "readme", "chunk_id": "stored-chunk-id",
    "uri": "memory://project/readme", "kind": "note", "title": "Readme", "text": "Indexed text"}],
  "knowledge_selection": {"query": "router", "kind": "note", "uri_prefix": "memory://project/", "limit": 2}
}
```

Disabled sources produce empty arrays and null knowledge_selection. Host goal/data
never becomes system instructions, Tool Authority, a review flag or Runtime control.
The Host is responsible for authorizing retrieval of its selected sources. Ownership
transfer does not prove semantic answer quality or completed external work.

```c
turbo_agent_task_context_options_t selection;
const char *keys[] = {"notes"};
json_value_t *input = NULL;
turbo_agent_task_context_options_init(&selection);
selection.memory_namespace = "project/demo";
selection.memory_keys = keys;
selection.memory_key_count = 1;
selection.knowledge_limit = 2; /* configured Session query/kind/URI prefix */
int rc = turbo_agent_session_create_task_input_state_json_value(
    session, host_goal, &selection, &input);
/* Check rc, then submit input through existing execution APIs. */
json_free(input);
```

The public header documents exact errors. In particular unavailable selected memory
uses EIO because the existing get API cannot distinguish missing data from read failure;
no knowledge hits uses ENOENT. All failures clear output. The factory creates no parent
task/run identity, supervision audit, lease, recovery operation or automatic memory write.
Rollback is to stop calling the additive API; existing Session/config ABI is unchanged.

## One Agent report integration

The [report example](../examples/single-agent-report.md) optionally takes a context Session
and paired selection options. It calls this factory once before admitting any worker,
then carries that identical serialized user packet to both workers, aggregation and
the tool model. Existing per-stage user data remains separate. Combined user payloads
are bounded to 1 MiB at every handoff; an otherwise valid context can therefore fail
when combined with a large brief or group evidence. No worker may choose memory keys,
URI scope, source backend or additional reads. The whole Host deadline includes context
preparation, but cannot forcibly interrupt synchronous store calls.

The rendering catalog separately binds `brief @ host-v1` to the manual brief and,
when context is enabled, `context @ host-v1` to the selected packet for this invocation.
These are Host packet aliases, not underlying document revision hashes or claim support.
The same canonical approval, strict tool result and receipt-based delivery gates apply.
The source Session and stores are not shared with worker Runtime/store owners during
execution; only the copied input data crosses those boundaries.

`test_turbo_agent_task_input` uses real Session memory and SQLite knowledge APIs for
selection, independent ownership, disabled sources, invalid selectors, read failure,
raw NUL rejection, literal URI filtering and escaped aggregate bounds. The report
integration tests use actual offline Responses/compatible-Chat Runtime execution to
check context at all four model endpoints, unchanged Host approval, no model admission
on source failure and combined input bounds. Neither test claims live-model quality,
paid cost, durability, freshness, content verification or crash recovery.
