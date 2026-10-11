#ifndef TURBO_CLI_KNOWLEDGE_H
#define TURBO_CLI_KNOWLEDGE_H
#include "turbo_agent_knowledge_store.h"
#include <stdatomic.h>

/* Input-thread owner. Index/query only between quiescent runs; context is copied
 * into the turn before submission. Close after the Harness worker has drained. */
int turbo_cli_knowledge_open(const char *path, const char *index_dir,
    const atomic_int *interrupted, turbo_agent_knowledge_store_t **out);
int turbo_cli_knowledge_prepare(turbo_agent_knowledge_store_t *store,
    const char *query, json_value_t *state);

typedef int (*turbo_cli_index_text_fn)(void *owner, const char *uri,
    const char *title, const char *text);
/* Synchronous bounded scanner; callback arguments borrow this invocation. */
int turbo_cli_index_directory(const char *directory, const atomic_int *interrupted,
    turbo_cli_index_text_fn ingest, void *owner);
/* Borrowed results; copies bounded, source-labelled text into state. */
int turbo_cli_retrieval_context(const json_value_t *results, json_value_t *state);
/* SDK byte chunks can split UTF-8. Returns an owned, boundary-trimmed snippet. */
int turbo_cli_retrieval_snippet(const char *text, size_t length, tstr *out);
#endif
