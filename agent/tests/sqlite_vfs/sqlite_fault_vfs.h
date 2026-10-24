#ifndef TURBOAGENT_TEST_SQLITE_FAULT_VFS_H
#define TURBOAGENT_TEST_SQLITE_FAULT_VFS_H

#include <sqlite3.h>

enum { VFS_MAIN = 0, VFS_JOURNAL = 1, VFS_WRITE = 0, VFS_SYNC = 1 };
enum {
  VFS_FAIL_BEFORE = 0, VFS_FAIL_AFTER, VFS_PARTIAL_ERROR,
  VFS_EXIT_BEFORE, VFS_EXIT_AFTER, VFS_PARTIAL_EXIT
};

typedef struct sqlite_fault_stats_s {
  int target_opens, live_files, failures, error, flags;
  int writes_after_fault, syncs_after_fault;
  int main_writes, main_syncs;
  int mode, native_completed, native_bytes, changed_bytes, readback_bytes;
  sqlite3_int64 offset;
  sqlite3_int64 native_offset;
  int bytes;
} sqlite_fault_stats_t;

/* Single-owner test process only. Other paths and every noninjected operation
 * delegate to the actual platform VFS. All connections must close before remove. */
int sqlite_fault_vfs_install(const char *database_path);
int sqlite_fault_vfs_remove(void);
const char *sqlite_fault_vfs_native_name(void);
void sqlite_fault_vfs_arm(int file_kind, int operation, int error, int mode);
void sqlite_fault_vfs_disarm(void);
sqlite_fault_stats_t sqlite_fault_vfs_stats(void);

#endif
