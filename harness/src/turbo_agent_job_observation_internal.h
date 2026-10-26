#ifndef TURBO_AGENT_JOB_OBSERVATION_INTERNAL_H
#define TURBO_AGENT_JOB_OBSERVATION_INTERNAL_H

#include "turbo_agent_job_observation.h"

/* Caller owns the serialized Harness reservation and a settled local operation.
 * Private DSO composition. Does not execute, retry or read back a write error. */
int turbo_agent_job_observation_write(turbo_agent_runtime_t *runtime, const char *job_id,
    uint32_t revision, const char *observation_id, const char *supervisor_id, int dispatched,
    const turbo_agent_job_execution_status_t *status, const char *request_id,
    const char *reason_ref, const json_value_t *reference);

#endif
