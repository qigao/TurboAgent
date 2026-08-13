#ifndef TURBO_AGENT_INBOX_INTERNAL_H
#define TURBO_AGENT_INBOX_INTERNAL_H

#include "turbo_agent_inbox.h"
#include "turbo_agent_runtime.h"

typedef struct turbo_agent_inbox_s turbo_agent_inbox_t;

int turbo_agent_inbox_create(turbo_agent_runtime_t *runtime, const char *thread_id,
                             const turbo_agent_inbox_config_t *config,
                             turbo_agent_inbox_t **out_inbox);
void turbo_agent_inbox_destroy(turbo_agent_inbox_t *inbox);
int turbo_agent_inbox_enqueue(turbo_agent_inbox_t *inbox, turbo_agent_inbox_kind_t kind,
                              const json_value_t *message, uint64_t timeout_ms,
                              char **out_inbox_id);
int turbo_agent_inbox_status(turbo_agent_inbox_t *inbox, const char *inbox_id,
                             json_value_t **out_status);
int turbo_agent_inbox_claim(turbo_agent_inbox_t *inbox, turbo_agent_inbox_kind_t kind,
                            const char *run_id, json_value_t **out_record);
int turbo_agent_inbox_mark_applied(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                   const char *applied_event_id);
int turbo_agent_inbox_requeue(turbo_agent_inbox_t *inbox, const char *inbox_id);
int turbo_agent_inbox_bind_applied_event(turbo_agent_inbox_t *inbox, const char *inbox_id,
                                         const char *event_id);
int turbo_agent_inbox_commit_bound_claim(turbo_agent_inbox_t *inbox);
int turbo_agent_inbox_close(turbo_agent_inbox_t *inbox);

#endif
