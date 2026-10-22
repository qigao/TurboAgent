#ifndef TURBO_CLI_H
#define TURBO_CLI_H
#include "turbo_agent.h"
#include "turbo_agent_harness.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>

/* Private synchronous Host seam, not an installed API or a command-line flag.
 * Request strings are borrowed only for this call. 1 approves this exact call,
 * 0 rejects, negative aborts. NULL selects the stdin challenge/response UI. */
typedef int (*turbo_cli_approval_fn)(const json_value_t *request, void *user_data);

typedef struct turbo_cli_options_s {
  const char *workspace;
  const char *prompt;
  const char *model;
  int read_only;
  const char *session_db;
  int resume;
  const char *diagram; /* NULL, "mermaid" or "json": read a saved session and exit. */
  const char *diagram_focus;
  const char *diagram_direction; /* NULL defaults to both when focus is set. */
  const char *diagram_depth; /* NULL defaults to one hop; exact decimal 0..127. */
  const char *diagram_journals; /* Optional SHA-256 key list, or all for published-run discovery. */
  int diagram_origins; /* Resolve scoped calls against the published snapshot. */
  const char *diagram_to; /* Optional target ID for a shortest path from diagram_focus. */
  int diagram_plan_steps; /* Include recorded workflow step origins; implies call origins. */
  const char *mode; /* NULL/coding preserves the existing workspace agent. */
  const char *instructions;
  const char *knowledge_db; /* Dedicated SQLite KnowledgeStore, separate from session_db. */
  const char *index_dir; /* Explicit startup import; requires knowledge_db or vector_db. */
  const char *vector_db;
  const char *embedding_url; /* Full OpenAI-compatible embeddings endpoint. */
  const char *embedding_model;
  const char *workflow; /* NULL/loop, review, engineering; independent of mode. */
  /* Private CLI-only trust choice; NULL defaults to inspect (no shell/write). */
  const char *trust_profile; /* inspect | approval (coding loop only) | trusted-local. */
  turbo_cli_approval_fn approval;
  void *approval_user_data;
} turbo_cli_options_t;

/* Process entry only: CmdParser prints help/errors to stdout and exits with 1.
 * Parsed strings borrow argv or CmdParser's process-lifetime response storage. */
void turbo_cli_parse_options(int argc, char **argv, turbo_cli_options_t *options);

/* Private driver seam for formal tests. No installed header or public ABI.
 * Options, streams, model overrides and cancellation flag are borrowed until return.
 * A concurrent control/signal owner may set the atomic flag; the driver only reads it. */
int turbo_cli_run(const turbo_cli_options_t *options, FILE *input, FILE *output,
                  FILE *diagnostics, const turbo_agent_config_t *model_override,
                  const atomic_int *interrupted);

int turbo_cli_export_diagram(const turbo_cli_options_t *options, FILE *output, FILE *diagnostics);

/* Consume a quiescent execution's result once. The caller retains the execution
 * and streams. Only success prints an answer and transfers the owned state to
 * out_state; all other paths free the result and return a nonzero CLI exit code. */
int turbo_cli_complete_execution(turbo_agent_harness_execution_t *execution,
                                 int wait_status, int interrupted, FILE *output,
                                 FILE *diagnostics, json_value_t **out_state);
#endif
