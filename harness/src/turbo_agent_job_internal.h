#ifndef TURBO_AGENT_JOB_INTERNAL_H
#define TURBO_AGENT_JOB_INTERNAL_H

#include "turbo_agent_job.h"

/* Caller has validated and serialized the exact Job head. Read only the
 * cancellation record: absence -> OK, valid request -> ECANCELED, errors retain
 * their status. Private DSO composition; never installed as public ABI. */
int turbo_agent_job_check_active(turbo_agent_runtime_t *runtime, const char *id,
    uint32_t revision);

#endif
