#ifndef TURBOAGENT_TEST_PRODUCTION_SQLITE_STORE_H
#define TURBOAGENT_TEST_PRODUCTION_SQLITE_STORE_H
#include "turbo_runtime_store_sqlite.h"
void vfs_test_sqlite_config_init(turbo_runtime_sqlite_config_t *config);
int vfs_test_sqlite_store_open(const turbo_runtime_sqlite_config_t *config,
                              turbo_agent_runtime_store_t *out);
#endif
