#ifndef TURBO_CLI_STORAGE_H
#define TURBO_CLI_STORAGE_H
#include "turbo_agent_session.h"

enum { TURBO_CLI_MAX_TURNS = 64 };

/* Private, input-thread owner. Open before constructing Harness, close after
 * Harness destruction and worker drain. Session borrows callbacks without a
 * destructor; no callbacks or metadata mutations may overlap a running turn. */
typedef struct turbo_cli_storage_s {
  turbo_agent_runtime_store_t store;
  json_value_t *metadata;
  size_t turns;
} turbo_cli_storage_t;

int turbo_cli_storage_open(turbo_cli_storage_t *storage, const char *path,
                          const char *workspace, int resume);
void turbo_cli_storage_close(turbo_cli_storage_t *storage);
int turbo_cli_storage_load(turbo_cli_storage_t *storage, turbo_agent_session_t *session,
                          json_value_t **out_state);
/* Commit the active marker before submitting work. Failed/interrupted work
 * deliberately leaves it active; resume must not replay unknown side effects. */
int turbo_cli_storage_begin(turbo_cli_storage_t *storage, turbo_agent_session_t *session);
int turbo_cli_storage_complete(turbo_cli_storage_t *storage, turbo_agent_session_t *session);
#endif
