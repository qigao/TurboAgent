# Observed Router group handoffs

The Host can require acknowledgement of one atomic cohort observation before
publishing an aggregation input or transferring any canonical result pair:

    turbo_agent_router_group_build_input_with_observation(
        group, host_task, host_runtime, observation_id, &input);
    turbo_agent_router_group_take_results_with_observation(
        group, host_runtime, another_id, results, capacity, &count);

These opt-in paths retain the ordinary group barrier, authority, loss, validation,
bounds and ownership rules. The first constructs and validates its owned input
privately, then writes before publication. The second writes before the first
result move. A synchronous private member projection reuses the existing version 1
task-observation validation; no task/model/Runtime result payload is duplicated.
Existing group APIs continue to work without recording.

## One record, one atomic create

Each call creates one immutable record in router_group_observations using a
Host-selected 1..64-byte ASCII letter/digit/underscore/hyphen id. It does not write
one record per member. A later member-projection error therefore cannot leave a
partially persisted cohort, and a backend atomic-write failure withholds the
entire handoff.

The Host Runtime/store must be independent of every Router endpoint Runtime/store,
including endpoints outside the group. It is borrowed only during the synchronous
call. No worker, returned input, group handle or background writer retains it.
Host controls and store use remain serialized; backend callbacks must not reenter
Runtime/Router or replace policy. Policy/registry changes remain quiescent.

On write error, including commit followed by lost acknowledgement, outputs stay
unpublished: null input or zero moved pairs. The successful-input flag and result
ownership remain unconsumed. Atomic-create errors propagate unchanged. No get,
list, put fallback, overwrite or retry occurs. EALREADY is a conflict even for a
matching old record. Explicitly load the same id and compare expected group,
member/segment, route, thread, parent and handoff kind before choosing a fresh id
or an ordinary API. Building and collecting need different ids.

Normal operation errors retain ordinary group semantics. In particular, failed
aggregation validation can halt the group and request sibling cancellation before
any persistence attempt. After all members actually terminate, current-authority,
responsive diagnostic result collection remains possible, including null summaries
from early Runtime failure. Stale authority or sticky loss blocks both handoffs.

## Historical schema and bounds

Version 1 has exactly these outer fields:

| Field | Meaning |
| --- | --- |
| schema_version, observation_id | Version 1 and exact Host-known id. |
| semantics, clock_domain, recorded_at | pre_group_handoff_observation, origin_process_monotonic, origin UTC observation. |
| group_id, parent_run_id | Copied adoption label and common Host parent. A reused label alone is not a task identity. |
| observed_mono_ms, failure_code, input_built | Existing Host supervision timestamp, sticky group failure and prior successful input publication. |
| handoff_kind | aggregation_input or result_collection; the requested handoff is prospective. |
| members | 2..16 terminal task-observation metadata objects in adoption order. |

Nested objects use the same observation_id as the enclosing record. They are
embedded metadata, not separately written router_observations keys. Each preserves
the actual task/current Execution/endpoint/thread/parent identities, terminal
Execution status/operation code, available Runtime anchors, origin supervision and
budget metadata. Model-only groups have no review segments. Members have distinct
task, Execution, endpoint and thread ids, the same parent, and observed clocks no
later than the group observation. Captured authority is current and loss is false
at this handoff boundary. These historical facts grant no authority after restart.

An aggregation-input record also requires successful member Executions, no failure
kind, no sticky group failure and no prior successful input. The existing live
projection additionally validates nonempty canonical model text, pending proposals
and payload limits; the record contains no text and cannot verify answer quality.

Individual metadata limits remain unchanged: each nested object <=16 KiB,
model/provider <=1024 bytes, parent/run/checkpoint ids <=128, Router ids <=64.
The complete group record is <=128 KiB after JSON serialization, including escaping.
Oversize data fails without truncation or partial output. Load validates the exact
outer/member fields, types/NUL, identity relationships, clocks, handoff kind and
bounds. Invalid schema is EPROTO; oversize projections ENOBUFS. Backend read/parsing
can precede projection limits; use the backend's existing capacity limits.

    json_value_t *record = NULL;
    int rc = turbo_agent_router_load_group_observation(
        host_runtime, observation_id, &record);
    /* Check rc and expected exact cohort identities before reconciliation. */
    json_free(record);

Loading is exact-id/read-only, returns an owned JSON tree and clears output on every
failure. It needs no Router after restart. It starts no work and restores no result
owners, policy grants, group handles or leases. SQLite committed-record guarantees
apply; memory is volatile and the file backend lacks atomic create.

## What a surviving record proves

A record observes actual terminal metadata before a prospective handoff. It cannot
prove input publication, raw-result transfer, aggregator invocation or application
delivery. A crash can leave the same record before or after each of those events.
No cross-record/Runtime/tool/external-delivery transaction or exactly-once delivery
is introduced. Origin monotonic values cannot renew leases in a new process.
Existing canonical Runtime and Tool Journal evidence still owns outcomes; UNKNOWN
external effects must not be retried from this metadata.

Ten five-process cases cover both input and collection before/after atomic create,
after lost-ack return, after handoff/before delivery and after explicit delivery.
Each requires exact exit 86, matching boundary and empty-output evidence. Separate
Host, per-worker, aggregator and test-oracle SQLite stores retain model, handoff,
aggregation and delivery counters. Two real worker callbacks cross a barrier; only
the test-oracle counter is serialized by a fixture mutex. Input delivery explicitly
submits the derived input to a separately routed aggregation model.

Independent processes reopen stores, join original member/admission/observation
identities to exact canonical Runs and check counts. A fresh Host rejects work before
current delegation, rejects reused cohort ids without overwriting, and completes a
different explicitly authorized same-thread cohort once. Later thread heads do not
replace the original exact Run references. The fixture does not resume or redeliver
the old cohort. Controlled cuts retain OS/cache and do not qualify physical power loss.

This advances #213. Complete automatic lifecycle audit, remote-loss reconciliation,
shared-Host parallel effects and real-model quality/cost acceptance remain separate
work. No public struct layout, Router ABI version or existing observation/admission/
Tool Journal schema changes.
