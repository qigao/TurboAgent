#ifndef TURBO_AGENT_JOB_DELIVERY_H
#define TURBO_AGENT_JOB_DELIVERY_H

#include "turbo_agent_harness.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Prepare one immutable delivery intent for an explicitly accepted bound Job.
 * Revalidates the Host acceptance and all canonical execution evidence under
 * current admission, without executing tools/models or sending anything.
 * delivery_id is 1..64 ASCII letters/digits/_/-. destination_ref and payload_ref
 * are 1..1024-byte Host identifiers, never fetched or interpreted. No result is
 * copied. One agent_job_delivery_intents record is atomically claimed per Job.
 * Its stable idempotency_key is "<job_id>:<delivery_id>"; Host must namespace it
 * across stores/tenants and use it with its separately authorized destination.
 * A rejected decision returns EPERM; missing evidence retains ENOENT.
 *
 * Host must persist/reconcile this intent before initiating external delivery.
 * An intent is neither external delivery authority nor evidence of sending.
 * A missing receipt means outcome unknown, including possibly not attempted.
 * Consult the destination using the stable key or obtain explicit Host review;
 * this API never retries an unknown external effect or promises exactly-once
 * delivery. Destination/payload validity, outbound authorization, source
 * freshness and remote idempotency/receipt verification remain Host duties.
 *
 * All writes use create-if-absent with no overwrite, fallback, retry or readback.
 * Duplicate intent/receipt returns EALREADY, even identical. A failed write may
 * have committed: preserve the original error and reconcile with get().
 * Successful outputs own independent JSON; *out clears on failure. Inputs must
 * stay borrowed/stable and distinct from output slots. Host serializes Job and
 * delivery access and keeps policy/registry/code quiescent through return.
 * Harness submission exclusion applies. Policy generation must remain current.
 */
CXX_C_API int turbo_agent_harness_prepare_job_delivery_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *delivery_id, const char *destination_ref, const char *payload_ref,
    turbo_agent_compile_diagnostic_t *out_diagnostic, json_value_t **out_intent);

/** Read and revalidate intent and optional receipt under current acceptance and
 * admission. Returns schema-1 job_delivery_state: status="outcome_unknown" and
 * intent, or status="delivered" with intent and receipt. No writes or delivery
 * occur. Missing intent is ENOENT; orphan/mismatched records EPROTO; unknown
 * schema ENOTSUP; other backend statuses are preserved. A failed/revoked read
 * withholds output and leaves historical records untouched.
 */
CXX_C_API int turbo_agent_harness_get_job_delivery_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    turbo_agent_compile_diagnostic_t *out_diagnostic, json_value_t **out_state);

/** Record one immutable Host-attested successful receipt for the existing exact
 * intent. delivery_id must match it; receipt_ref is a 1..1024-byte Host evidence
 * identifier. Host must verify the destination acknowledged that intent/key and
 * payload before calling. No remote system is queried and no send is inferred
 * from local tool success. Returns a metadata-only agent_job_delivery_receipts
 * record. Publication/ownership/recovery follow prepare().
 */
CXX_C_API int turbo_agent_harness_record_job_delivery_receipt_json(
    turbo_agent_harness_t *harness, const char *job_id, uint32_t expected_job_revision,
    const turbo_agent_compiler_config_t *compiler_config,
    const char *delivery_id, const char *receipt_ref,
    turbo_agent_compile_diagnostic_t *out_diagnostic, json_value_t **out_receipt);

#ifdef __cplusplus
}
#endif
#endif
