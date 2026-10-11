#ifndef TURBO_CLI_H
#define TURBO_CLI_H
#include "turbo_agent.h"
#include "turbo_agent_harness.h"
#include <signal.h>
#include <stdio.h>

typedef struct turbo_cli_options_s {
  const char *workspace;
  const char *prompt;
  const char *model;
  int read_only;
} turbo_cli_options_t;

/* Process entry only: CmdParser prints help/errors to stdout and exits with 1.
 * Parsed strings borrow argv or CmdParser's process-lifetime response storage. */
void turbo_cli_parse_options(int argc, char **argv, turbo_cli_options_t *options);

/* Private driver seam for formal tests. No installed header or public ABI.
 * Options, streams, model overrides and cancellation flag are borrowed until return. */
int turbo_cli_run(const turbo_cli_options_t *options, FILE *input, FILE *output,
                  FILE *diagnostics, const turbo_agent_config_t *model_override,
                  volatile sig_atomic_t *interrupted);

/* Consume a quiescent execution's result once. The caller retains the execution
 * and streams. Only success prints an answer and transfers the owned state to
 * out_state; all other paths free the result and return a nonzero CLI exit code. */
int turbo_cli_complete_execution(turbo_agent_harness_execution_t *execution,
                                 int wait_status, int interrupted, FILE *output,
                                 FILE *diagnostics, json_value_t **out_state);
#endif
