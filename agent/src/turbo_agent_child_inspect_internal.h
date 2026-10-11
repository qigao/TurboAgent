#ifndef TURBO_AGENT_CHILD_INSPECT_INTERNAL_H
#define TURBO_AGENT_CHILD_INSPECT_INTERNAL_H

#include "turbo_runtime_json.h"

/* Query adapters own these independent results. Optional fields may be NULL. */
typedef struct turbo_agent_child_inspect_parts_s {
  json_value_t *run;
  json_value_t *checkpoints;
  json_value_t *latest_checkpoint;
  json_value_t *checkpoint_context;
  json_value_t *history_events;
  json_value_t *trace_events;
  json_value_t *thread_timeline;
  json_value_t *branch_tree;
} turbo_agent_child_inspect_parts_t;

void turbo_agent_child_inspect_parts_clear_internal(turbo_agent_child_inspect_parts_t *parts);

/* Consumes and clears all parts on every outcome. Publishes only a complete report. */
int turbo_agent_child_inspect_build_internal(turbo_agent_child_inspect_parts_t *parts,
                                             json_value_t **out_inspect);

/* Borrows output_item for this call; consumes *child_inspect only on success. */
int turbo_agent_child_orchestration_build_internal(const json_value_t *output_item,
                                                   json_value_t **child_inspect,
                                                   json_value_t **out_inspect);

#endif
