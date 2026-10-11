#ifndef TURBO_AGENT_KNOWLEDGE_STORE_INTERNAL_H
#define TURBO_AGENT_KNOWLEDGE_STORE_INTERNAL_H
#include "turbo_agent_knowledge_store.h"
#include <sqlite3.h>

/* One connection owner shared by the store implementation and its read adapter.
 * Callers serialize operations; no borrowed statement survives an API call. */
struct turbo_agent_knowledge_store_s {
  sqlite3 *db;
  int fts5_enabled;
};

extern const char turbo_knowledge_diagram_schema[];
int turbo_knowledge_diagram_tool_value(const json_value_t *arguments,
    json_value_t **out_result, void *user_data);
int turbo_knowledge_diagram_database_status(int status);
/* Called only with a validated, stable view inside the adapter's read transaction. */
int turbo_knowledge_diagram_add_uri_mentions(sqlite3 *db, json_value_t *view);
#endif
