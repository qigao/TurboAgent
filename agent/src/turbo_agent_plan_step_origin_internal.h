#ifndef TURBO_AGENT_PLAN_STEP_ORIGIN_INTERNAL_H
#define TURBO_AGENT_PLAN_STEP_ORIGIN_INTERNAL_H
#include <cmeta/pp.h>

/* Workflow-owned provenance, never a compiler step ID or execution grant. */
#define TURBO_AGENT_PLAN_STEP_ORIGIN_KEY "plan_step_origin"
#define TURBO_AGENT_PLAN_STEP_ORIGIN_VERSION 1
#define TURBO_AGENT_PLAN_STEP_ORIGIN_FIELDS(M) Schema(M, (plan_version), (step_index))
#endif
