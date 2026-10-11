#ifndef TURBO_RUNTIME_STORE_SQLITE_H
#define TURBO_RUNTIME_STORE_SQLITE_H

#include "turbo_runtime_store.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RUNTIME_SQLITE_ABI_VERSION 1U

typedef struct turbo_runtime_sqlite_config_s {
  size_t struct_size;
  uint32_t abi_version;
  const char *path; /* Borrowed UTF-8 filesystem path; parent must exist. */
  size_t max_record_bytes;
  size_t max_list_bytes;
  size_t max_list_records; /* Maximum scanned rows, including filtered rows. */
  uint64_t max_database_bytes; /* Main database only, excludes rollback journal. */
  uint32_t busy_timeout_ms;
  int create_if_missing;
} turbo_runtime_sqlite_config_t;

/** Defaults: 16 MiB/record, 64 MiB/list, 4096 scanned rows, 256 MiB database,
 * 250 ms lock wait, create enabled, NULL path. All byte limits must be nonzero. */
CXX_C_API void turbo_runtime_sqlite_config_init(turbo_runtime_sqlite_config_t *config);

/**
 * Open a bounded SQLite record store. Clear out_store on entry. Return SALTS_OK
 * or a Salts error (EINVAL, ENOMEM, EBUSY, ENOENT, ENOSPC, EPROTO, ENOTSUP, EIO).
 * Nonempty foreign/unversioned databases and unsupported versions are rejected;
 * no existing data is migrated or overwritten. Memory/URI paths are not accepted.
 *
 * Uses DELETE journal mode and synchronous=FULL, with an exclusive connection
 * lock held until destruction. Concurrent owners fail after busy_timeout_ms;
 * native filesystem locking/sync support is required. WAL databases are rejected.
 * A successful put is one committed transaction. No transaction spans callbacks;
 * this does not make an external tool's side effects transactional.
 * Collection/id are nonempty, at most 1024 UTF-8 bytes; they are SQL values,
 * never paths. JSON is validated on write/read, and list fails without partial
 * output when a scan/byte limit is exceeded. Missing get returns SALTS_ENOENT.
 * Legacy runtime get/list wrappers still map callback errors to -1.
 *
 * On success the caller owns out_store->user_data until runtime_create consumes
 * it. On runtime_create failure call user_data_free. Serialize standalone callback
 * use; when bound, Runtime provides serialization. Destroy only after quiescence.
 * Example: config_init(&cfg); cfg.path = "session.db"; open(&cfg, &store);
 * check the status before passing store to Runtime or Session (no memory fallback).
 */
CXX_C_API int turbo_runtime_store_sqlite_open(
    const turbo_runtime_sqlite_config_t *config, turbo_agent_runtime_store_t *out_store);

#ifdef __cplusplus
}
#endif
#endif
