#ifndef TURBO_CLI_APPROVAL_H
#define TURBO_CLI_APPROVAL_H

#include "turbo_cli.h"
#include "turbo_agent_policy.h"
#include <tstr.h>

/* Private Host decision, never a second Runtime result/checkpoint store.
 * CLI owns this through worker drain; worker hooks and input-thread review never
 * overlap. Registry and policy stay immutable through the entire CLI lifetime. */
typedef struct turbo_cli_approval_s {
  turbo_agent_t *agent;
  const turbo_tool_registry_t *registry;
  turbo_agent_tool_authority_snapshot_t authority;
  tstr approved_note;
  turbo_tool_effect_flags_t effects;
  unsigned reviews;
} turbo_cli_approval_t;

int turbo_cli_approval_attach(turbo_cli_approval_t *approval, turbo_agent_t *agent,
                              const turbo_tool_registry_t *registry);
void turbo_cli_approval_clear(turbo_cli_approval_t *approval);
int turbo_cli_approval_read(const json_value_t *request, FILE *input, FILE *diagnostics);
/* Called after wait + complete pool drain. 1 replaces a paused execution with
 * an accepted resume, 0 leaves a non-paused result untaken, negative aborts.
 * Only a canonical Tool approval checkpoint can be resumed. */
int turbo_cli_approval_resume(turbo_cli_approval_t *approval,
    turbo_agent_harness_t *harness, turbo_agent_session_t *session,
    const turbo_agent_harness_run_options_t *options,
    turbo_agent_harness_execution_t **execution, const turbo_cli_options_t *cli,
    FILE *input, FILE *diagnostics, const atomic_int *interrupted);

#endif
