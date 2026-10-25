#ifndef TURBO_AGENT_ROUTER_INTERNAL_H
#define TURBO_AGENT_ROUTER_INTERNAL_H

#include "turbo_agent_router.h"
#include <tstr.h>

typedef struct router_endpoint_s {
  turbo_agent_router_endpoint_t config;
  tstr id;
  int busy;
} router_endpoint_t;

struct turbo_agent_router_s {
  turbo_agent_t *authority_agent;
  size_t count;
  unsigned tool_task_limit, tool_task_count;
  cmeta_mutex_t tool_mutex;
  cmeta_cond_t tool_changed;
  int tool_active;
  router_endpoint_t endpoints[TURBO_AGENT_ROUTER_MAX_ENDPOINTS];
};

struct turbo_agent_router_task_s {
  turbo_agent_router_t *router;
  router_endpoint_t *endpoint;
  turbo_graph_t *graph;
  turbo_agent_execution_t *execution, *identity_execution;
  tstr review_checkpoint, review_run_id;
  uint64_t deadline_ms;
  unsigned max_model_turns, model_turns, review_resumes;
  int pending_review, review_cancelled;
  int tool_waiting; /* guarded by router->tool_mutex */
  turbo_agent_tool_authority_snapshot_t parent_authority, child_authority;
  tstr thread_id, parent_run_id, admission_id;
  uint64_t started_ms, observed_ms, heartbeat_ms, estimated_micros;
  uint64_t budget_micros;
  unsigned heartbeat_timeout_ms, difficulty, input_tokens, output_tokens, required_capabilities;
  turbo_agent_router_role_t role;
  int unresponsive, taken;
  turbo_agent_router_group_t *group;
};

struct turbo_agent_router_group_s {
  tstr id;
  size_t count;
  uint64_t observed_ms;
  int failure_code, input_built, results_taken;
  turbo_agent_router_task_t *tasks[TURBO_AGENT_ROUTER_MAX_ENDPOINTS];
};

int turbo_agent_router_task_observation_internal(const turbo_agent_router_task_t *task,
    const char *id, json_value_t **out_record);
int turbo_agent_router_observation_validate_internal(const json_value_t *record, const char *id);
int turbo_agent_router_group_observation_destination_internal(const turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id);
int turbo_agent_router_group_observation_write_internal(const turbo_agent_router_group_t *group,
    turbo_agent_runtime_t *host_runtime, const char *id, const char *kind);

typedef struct router_admission_s {
  const turbo_agent_router_task_t *task;
  turbo_agent_runtime_t *host_runtime;
  const turbo_agent_runtime_parent_link_t *parent_link;
} router_admission_t;

int turbo_agent_router_admission_write_internal(void *user_data, const char *execution_id);

typedef struct router_review_intent_s {
  const turbo_agent_router_task_t *task;
  turbo_agent_runtime_t *host_runtime;
  const char *id, *checkpoint_id;
  uint64_t requested_ms;
  int approved;
} router_review_intent_t;

int turbo_agent_router_review_intent_write_internal(void *user_data, const char *execution_id);

int turbo_agent_router_id_valid_internal(const char *text);
int turbo_agent_router_authority_current_internal(const turbo_agent_router_task_t *task);
int turbo_agent_router_task_poll_internal(turbo_agent_router_task_t *task, uint64_t now_ms);

#endif
