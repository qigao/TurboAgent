#ifndef TURBO_AGENT_JOB_ADMISSION_INTERNAL_H
#define TURBO_AGENT_JOB_ADMISSION_INTERNAL_H
#include "turbo_agent_job_admission.h"

/* Caller retains the exclusive Harness reservation; no worker has been queued. */
int turbo_agent_job_admission_write(turbo_agent_runtime_t *runtime, const char *job_id,
    uint32_t revision, const char *admission_id, const char *supervisor_id);
#endif
