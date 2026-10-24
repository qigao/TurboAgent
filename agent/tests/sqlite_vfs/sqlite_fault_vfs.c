#include "sqlite_fault_vfs.h"

#include <stdlib.h>
#include <string.h>

typedef struct fault_file_s {
  sqlite3_file base;
  sqlite3_file *real;
  sqlite3_io_methods methods;
  int kind, flags;
} fault_file_t;

static struct {
  sqlite3_vfs vfs;
  sqlite3_vfs *native;
  char *path;
  int installed, armed, kind, operation, error;
  sqlite_fault_stats_t stats;
} fault;

static fault_file_t *file_cast(sqlite3_file *file) { return (fault_file_t *)file; }

static int inject(fault_file_t *file, int operation, int bytes, sqlite3_int64 offset) {
  if (!fault.armed || file->kind != fault.kind || operation != fault.operation) return 0;
  fault.armed = 0; /* Restore native I/O before SQLite attempts rollback. */
  ++fault.stats.failures;
  fault.stats.error = fault.error;
  fault.stats.flags = file->flags;
  fault.stats.bytes = bytes;
  fault.stats.offset = offset;
  return fault.error;
}

static int io_close(sqlite3_file *base) {
  fault_file_t *file = file_cast(base);
  int rc = file->real->pMethods->xClose(file->real);
  sqlite3_free(file->real);
  file->real = NULL;
  base->pMethods = NULL;
  --fault.stats.live_files;
  return rc;
}
static int io_read(sqlite3_file *base, void *buffer, int bytes, sqlite3_int64 offset) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xRead(f->real, buffer, bytes, offset);
}
static int io_write(sqlite3_file *base, const void *buffer, int bytes, sqlite3_int64 offset) {
  fault_file_t *f = file_cast(base);
  int rc = inject(f, VFS_WRITE, bytes, offset);
  if (rc) return rc;
  rc = f->real->pMethods->xWrite(f->real, buffer, bytes, offset);
  if (f->kind >= 0 && fault.stats.failures && rc == SQLITE_OK)
    ++fault.stats.writes_after_fault;
  return rc;
}
static int io_sync(sqlite3_file *base, int flags) {
  fault_file_t *f = file_cast(base);
  int rc = inject(f, VFS_SYNC, 0, 0);
  if (rc) return rc;
  rc = f->real->pMethods->xSync(f->real, flags);
  if (f->kind >= 0 && fault.stats.failures && rc == SQLITE_OK)
    ++fault.stats.syncs_after_fault;
  return rc;
}
static int io_truncate(sqlite3_file *base, sqlite3_int64 bytes) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xTruncate(f->real, bytes);
}
static int io_size(sqlite3_file *base, sqlite3_int64 *bytes) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xFileSize(f->real, bytes);
}
static int io_lock(sqlite3_file *base, int mode) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xLock(f->real, mode);
}
static int io_unlock(sqlite3_file *base, int mode) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xUnlock(f->real, mode);
}
static int io_reserved(sqlite3_file *base, int *locked) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xCheckReservedLock(f->real, locked);
}
static int io_control(sqlite3_file *base, int op, void *argument) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xFileControl(f->real, op, argument);
}
static int io_sector(sqlite3_file *base) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xSectorSize(f->real);
}
static int io_characteristics(sqlite3_file *base) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xDeviceCharacteristics(f->real);
}
static int io_shm_map(sqlite3_file *base, int page, int size, int extend, void volatile **out) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xShmMap(f->real, page, size, extend, out);
}
static int io_shm_lock(sqlite3_file *base, int offset, int count, int flags) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xShmLock(f->real, offset, count, flags);
}
static void io_shm_barrier(sqlite3_file *base) {
  fault_file_t *f = file_cast(base);
  f->real->pMethods->xShmBarrier(f->real);
}
static int io_shm_unmap(sqlite3_file *base, int remove) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xShmUnmap(f->real, remove);
}
static int io_fetch(sqlite3_file *base, sqlite3_int64 offset, int bytes, void **out) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xFetch(f->real, offset, bytes, out);
}
static int io_unfetch(sqlite3_file *base, sqlite3_int64 offset, void *buffer) {
  fault_file_t *f = file_cast(base);
  return f->real->pMethods->xUnfetch(f->real, offset, buffer);
}

static int vfs_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *base, int flags, int *out) {
  fault_file_t *file = file_cast(base);
  const sqlite3_io_methods *m;
  size_t length = strlen(fault.path);
  int rc;
  (void)vfs;
  memset(file, 0, sizeof(*file));
  file->kind = -1;
  file->flags = flags;
  /* A separate SQLite allocation preserves native file alignment on every ABI. */
  file->real = sqlite3_malloc(fault.native->szOsFile);
  if (!file->real) return SQLITE_NOMEM;
  memset(file->real, 0, (size_t)fault.native->szOsFile);
  rc = fault.native->xOpen(fault.native, name, file->real, flags, out);
  if (rc != SQLITE_OK) {
    if (file->real->pMethods) file->real->pMethods->xClose(file->real);
    sqlite3_free(file->real);
    file->real = NULL;
    return rc;
  }
  m = file->real->pMethods;
  if (name && !strncmp(name, fault.path, length)) {
    if ((flags & SQLITE_OPEN_MAIN_DB) && name[length] == 0) file->kind = VFS_MAIN;
    if ((flags & SQLITE_OPEN_MAIN_JOURNAL) && !strcmp(name + length, "-journal"))
      file->kind = VFS_JOURNAL;
  }
  if (file->kind >= 0) ++fault.stats.target_opens;
  file->methods = (sqlite3_io_methods){0};
  file->methods.iVersion = m->iVersion > 3 ? 3 : m->iVersion;
  file->methods.xClose = io_close; file->methods.xRead = io_read;
  file->methods.xWrite = io_write; file->methods.xTruncate = io_truncate;
  file->methods.xSync = io_sync; file->methods.xFileSize = io_size;
  file->methods.xLock = io_lock; file->methods.xUnlock = io_unlock;
  file->methods.xCheckReservedLock = io_reserved; file->methods.xFileControl = io_control;
  file->methods.xSectorSize = io_sector; file->methods.xDeviceCharacteristics = io_characteristics;
  if (m->iVersion >= 2) {
    file->methods.xShmMap = m->xShmMap ? io_shm_map : NULL;
    file->methods.xShmLock = m->xShmLock ? io_shm_lock : NULL;
    file->methods.xShmBarrier = m->xShmBarrier ? io_shm_barrier : NULL;
    file->methods.xShmUnmap = m->xShmUnmap ? io_shm_unmap : NULL;
  }
  if (m->iVersion >= 3) {
    file->methods.xFetch = m->xFetch ? io_fetch : NULL;
    file->methods.xUnfetch = m->xUnfetch ? io_unfetch : NULL;
  }
  base->pMethods = &file->methods;
  ++fault.stats.live_files;
  return SQLITE_OK;
}

/* Never pass our sqlite3_vfs to a native callback: it may depend on pAppData or
 * on its own VFS identity. This includes optional v2/v3 system interfaces. */
static int vfs_delete(sqlite3_vfs *v, const char *name, int sync) {
  (void)v; return fault.native->xDelete(fault.native, name, sync);
}
static int vfs_access(sqlite3_vfs *v, const char *name, int flags, int *out) {
  (void)v; return fault.native->xAccess(fault.native, name, flags, out);
}
static int vfs_path(sqlite3_vfs *v, const char *name, int size, char *out) {
  (void)v; return fault.native->xFullPathname(fault.native, name, size, out);
}
static void *vfs_dl_open(sqlite3_vfs *v, const char *name) {
  (void)v; return fault.native->xDlOpen(fault.native, name);
}
static void vfs_dl_error(sqlite3_vfs *v, int size, char *out) {
  (void)v; fault.native->xDlError(fault.native, size, out);
}
static void (*vfs_dl_sym(sqlite3_vfs *v, void *handle, const char *name))(void) {
  (void)v; return fault.native->xDlSym(fault.native, handle, name);
}
static void vfs_dl_close(sqlite3_vfs *v, void *handle) {
  (void)v; fault.native->xDlClose(fault.native, handle);
}
static int vfs_random(sqlite3_vfs *v, int size, char *out) {
  (void)v; return fault.native->xRandomness(fault.native, size, out);
}
static int vfs_sleep(sqlite3_vfs *v, int us) {
  (void)v; return fault.native->xSleep(fault.native, us);
}
static int vfs_time(sqlite3_vfs *v, double *out) {
  (void)v; return fault.native->xCurrentTime(fault.native, out);
}
static int vfs_error(sqlite3_vfs *v, int size, char *out) {
  (void)v; return fault.native->xGetLastError(fault.native, size, out);
}
static int vfs_time64(sqlite3_vfs *v, sqlite3_int64 *out) {
  (void)v; return fault.native->xCurrentTimeInt64(fault.native, out);
}
static int vfs_set_call(sqlite3_vfs *v, const char *name, sqlite3_syscall_ptr call) {
  (void)v; return fault.native->xSetSystemCall(fault.native, name, call);
}
static sqlite3_syscall_ptr vfs_get_call(sqlite3_vfs *v, const char *name) {
  (void)v; return fault.native->xGetSystemCall(fault.native, name);
}
static const char *vfs_next_call(sqlite3_vfs *v, const char *name) {
  (void)v; return fault.native->xNextSystemCall(fault.native, name);
}

int sqlite_fault_vfs_install(const char *path) {
  int rc;
  if (fault.installed) return SQLITE_MISUSE;
  memset(&fault, 0, sizeof(fault));
  fault.native = sqlite3_vfs_find(NULL);
  if (!fault.native) return SQLITE_NOTFOUND;
  fault.path = sqlite3_malloc(fault.native->mxPathname + 1);
  if (!fault.path) return SQLITE_NOMEM;
  rc = fault.native->xFullPathname(fault.native, path, fault.native->mxPathname + 1, fault.path);
  if (rc != SQLITE_OK) { sqlite3_free(fault.path); fault.path = NULL; return rc; }
  fault.vfs = *fault.native;
  fault.vfs.iVersion = fault.native->iVersion > 3 ? 3 : fault.native->iVersion;
  fault.vfs.szOsFile = (int)sizeof(fault_file_t);
  fault.vfs.zName = "turboagent-test-fault-vfs";
  fault.vfs.pNext = NULL; fault.vfs.pAppData = NULL;
  fault.vfs.xOpen = vfs_open; fault.vfs.xDelete = vfs_delete;
  fault.vfs.xAccess = vfs_access; fault.vfs.xFullPathname = vfs_path;
  fault.vfs.xDlOpen = fault.native->xDlOpen ? vfs_dl_open : NULL;
  fault.vfs.xDlError = fault.native->xDlError ? vfs_dl_error : NULL;
  fault.vfs.xDlSym = fault.native->xDlSym ? vfs_dl_sym : NULL;
  fault.vfs.xDlClose = fault.native->xDlClose ? vfs_dl_close : NULL;
  fault.vfs.xRandomness = vfs_random; fault.vfs.xSleep = vfs_sleep;
  fault.vfs.xCurrentTime = vfs_time; fault.vfs.xGetLastError = vfs_error;
  if (fault.vfs.iVersion >= 2)
    fault.vfs.xCurrentTimeInt64 = fault.native->xCurrentTimeInt64 ? vfs_time64 : NULL;
  if (fault.vfs.iVersion >= 3) {
    fault.vfs.xSetSystemCall = fault.native->xSetSystemCall ? vfs_set_call : NULL;
    fault.vfs.xGetSystemCall = fault.native->xGetSystemCall ? vfs_get_call : NULL;
    fault.vfs.xNextSystemCall = fault.native->xNextSystemCall ? vfs_next_call : NULL;
  }
  rc = sqlite3_vfs_register(&fault.vfs, 1);
  if (rc == SQLITE_OK) fault.installed = 1;
  else { sqlite3_free(fault.path); fault.path = NULL; }
  return rc;
}

int sqlite_fault_vfs_remove(void) {
  int rc;
  if (!fault.installed || fault.stats.live_files) return SQLITE_MISUSE;
  rc = sqlite3_vfs_unregister(&fault.vfs);
  if (rc == SQLITE_OK) rc = sqlite3_vfs_register(fault.native, 1);
  if (rc == SQLITE_OK) { sqlite3_free(fault.path); memset(&fault, 0, sizeof(fault)); }
  return rc;
}
const char *sqlite_fault_vfs_native_name(void) { return fault.native->zName; }
void sqlite_fault_vfs_arm(int kind, int operation, int error) {
  fault.kind = kind; fault.operation = operation; fault.error = error; fault.armed = 1;
}
void sqlite_fault_vfs_disarm(void) { fault.armed = 0; }
sqlite_fault_stats_t sqlite_fault_vfs_stats(void) { return fault.stats; }
