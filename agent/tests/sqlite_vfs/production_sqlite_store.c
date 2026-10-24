#include "production_sqlite_store.h"

/* SQLite may be static in both the Runtime DLL and this executable on Windows.
 * Compile the unchanged production Store here so registration and all Store I/O
 * use this executable's SQLite instance. The public Store ABI is untouched. */
#define turbo_runtime_sqlite_config_init vfs_test_sqlite_config_init
#define turbo_runtime_store_sqlite_open vfs_test_sqlite_store_open
#include "../../../runtime/src/turbo_runtime_store_sqlite.c"
