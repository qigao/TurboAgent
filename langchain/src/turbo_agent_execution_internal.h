#ifndef TURBO_AGENT_EXECUTION_INTERNAL_H
#define TURBO_AGENT_EXECUTION_INTERNAL_H

#include "turbo_agent_execution.h"

typedef int (*turbo_agent_execution_complete_fn)(void *user_data, turbo_graph_t *graph,
                                                 const turbo_graph_run_options_t *graph_options,
                                                 turbo_cancel_token_t *cancel_token,
                                                 json_value_t **summary, json_value_t **state);

typedef void (*turbo_agent_execution_terminal_fn)(void *user_data,
                                                  turbo_agent_execution_t *execution);

typedef struct turbo_agent_execution_hooks_s {
  turbo_agent_execution_complete_fn complete;
  turbo_agent_execution_terminal_fn terminal;
  void *user_data;
} turbo_agent_execution_hooks_t;

int turbo_agent_execution_start_internal(turbo_threadpool_t *executor,
                                         turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                         const json_value_t *state,
                                         const turbo_graph_run_options_t *graph_options,
                                         const turbo_agent_runtime_exec_options_t *runtime_options,
                                         const turbo_agent_execution_options_t *execution_options,
                                         const turbo_agent_execution_hooks_t *hooks,
                                         turbo_agent_execution_t **out_execution);

int turbo_agent_execution_resume_internal(turbo_threadpool_t *executor,
                                          turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                          const json_value_t *input,
                                          const turbo_graph_run_options_t *graph_options,
                                          const turbo_agent_runtime_exec_options_t *runtime_options,
                                          const turbo_agent_execution_options_t *execution_options,
                                          const turbo_agent_execution_hooks_t *hooks,
                                          turbo_agent_execution_t **out_execution);

int turbo_agent_execution_fork_internal(turbo_threadpool_t *executor,
                                        turbo_agent_runtime_t *runtime, turbo_graph_t *graph,
                                        const json_value_t *input,
                                        const turbo_graph_run_options_t *graph_options,
                                        const turbo_agent_runtime_exec_options_t *runtime_options,
                                        const turbo_agent_execution_options_t *execution_options,
                                        const turbo_agent_execution_hooks_t *hooks,
                                        turbo_agent_execution_t **out_execution);

#endif
