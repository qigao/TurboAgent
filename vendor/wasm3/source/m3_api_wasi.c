//
//  m3_api_wasi.c
//
//  Created by Volodymyr Shymanskyy on 11/20/19.
//  Copyright © 2019 Volodymyr Shymanskyy. All rights reserved.
//

#define _POSIX_C_SOURCE 200809L

#include "m3_api_wasi.h"

#include "m3_env.h"
#include "m3_exception.h"

#if defined(d_m3HasWASI)

// Fixup wasi_core.h
#if defined (M3_COMPILER_MSVC)
#  define _Static_assert(...)
#  define __attribute__(...)
#  define _Noreturn
#endif

#include "extra/wasi_core.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

#include "platform.h"
#include "turbo_fs.h"

#if defined(APE)
// Actually Portable Executable
// All functions are already included in cosmopolitan.h
#elif defined(__wasi__) || defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__) || defined(__linux__) || defined(__EMSCRIPTEN__) || defined(__CYGWIN__)
#  include <dirent.h>
#  include <sched.h>
#  include <unistd.h>
#  include <sys/uio.h>
#  if defined(__APPLE__)
#      include <crt_externs.h>
#      include <TargetConditionals.h>
#      if TARGET_OS_OSX // TARGET_OS_MAC includes iOS
#          include <sys/random.h>
#      else // iOS / Simulator
#          include <Security/Security.h>
#      endif
#  else
#      include <sys/random.h>
#  endif
#  define HAS_IOVEC
#elif defined(_WIN32)
#  include <Windows.h>
#  include <io.h>
// See http://msdn.microsoft.com/en-us/library/windows/desktop/aa387694.aspx
#  define SystemFunction036 NTAPI SystemFunction036
#  include <NTSecAPI.h>
#  undef SystemFunction036
#  define ssize_t SSIZE_T

#  define open  _open
#  define read  _read
#  define write _write
#  define close _close
#endif

#ifdef __APPLE__
# define environ (*_NSGetEnviron())
#elif defined(_MSC_VER)
# ifdef environ
#  undef environ
# endif
# define environ (*__p__environ())
#elif !defined(_WIN32)
extern char** environ;
#endif

static m3_wasi_context_t* s_default_wasi_context;
enum { M3_WASI_DEFAULT_PREOPEN_COUNT = 5 };

static
char* m3_wasi_strdup(const char* value)
{
    size_t len;
    char* copy;

    if (!value)
        return NULL;

    len = strlen(value) + 1;
    copy = (char*)malloc(len);
    if (!copy)
        return NULL;

    memcpy(copy, value, len);
    return copy;
}

static
int m3_wasi_context_reserve_preopens(m3_wasi_context_t* context, uint32_t required)
{
    m3_wasi_preopen_t* grown;
    uint32_t old_capacity;
    uint32_t new_capacity;

    if (!context)
        return -1;
    if (required <= context->preopen_capacity)
        return 0;

    old_capacity = context->preopen_capacity;
    new_capacity = old_capacity ? old_capacity : M3_WASI_DEFAULT_PREOPEN_COUNT;
    while (new_capacity < required)
        new_capacity *= 2;

    grown = (m3_wasi_preopen_t*)realloc(context->preopen,
                                        sizeof(m3_wasi_preopen_t) * new_capacity);
    if (!grown)
        return -1;

    context->preopen = grown;
    memset(&context->preopen[old_capacity], 0,
           sizeof(m3_wasi_preopen_t) * (new_capacity - old_capacity));
    for (uint32_t i = old_capacity; i < new_capacity; ++i)
        context->preopen[i].fd = -1;

    context->preopen_capacity = new_capacity;
    return 0;
}

static
void m3_wasi_context_clear_preopen_slot(m3_wasi_context_t* context, uint32_t fd)
{
    if (!context || fd >= context->preopen_count)
        return;

    if (fd >= 3 && context->preopen[fd].fd >= 0) {
        turbo_fs_close((turbo_file_t)context->preopen[fd].fd);
        context->preopen[fd].fd = -1;
    }

    free((void*)context->preopen[fd].path);
    free((void*)context->preopen[fd].real_path);
    context->preopen[fd].path = NULL;
    context->preopen[fd].real_path = NULL;
}

static
int m3_wasi_context_set_preopen_slot(m3_wasi_context_t* context,
                                     uint32_t fd,
                                     const char* guest_path,
                                     const char* host_path)
{
    char* guest_copy;
    char* host_copy;

    if (!context || !guest_path || !host_path)
        return -1;
    if (m3_wasi_context_reserve_preopens(context, fd + 1) != 0)
        return -1;

    guest_copy = m3_wasi_strdup(guest_path);
    if (!guest_copy)
        return -1;

    host_copy = m3_wasi_strdup(host_path);
    if (!host_copy) {
        free(guest_copy);
        return -1;
    }

    m3_wasi_context_clear_preopen_slot(context, fd);
    context->preopen[fd].fd = (fd < 3) ? (int)fd : -1;
    context->preopen[fd].path = guest_copy;
    context->preopen[fd].real_path = host_copy;
    if (context->preopen_count < fd + 1)
        context->preopen_count = fd + 1;
    return 0;
}

static
void m3_wasi_context_init_stdio(m3_wasi_context_t* context)
{
    if (!context)
        return;

    m3_wasi_context_set_preopen_slot(context, 0, "<stdin>", "");
    m3_wasi_context_set_preopen_slot(context, 1, "<stdout>", "");
    m3_wasi_context_set_preopen_slot(context, 2, "<stderr>", "");
}

static
void m3_wasi_context_init_preopens(m3_wasi_context_t* context)
{
    if (!context)
        return;

    m3_wasi_context_init_stdio(context);
    m3_wasi_context_set_preopen_slot(context, 3, "/", ".");
    m3_wasi_context_set_preopen_slot(context, 4, "./", ".");
}

static
void m3_wasi_context_close_preopens(m3_wasi_context_t* context)
{
    uint32_t i;

    if (!context)
        return;

    for (i = 0; i < context->preopen_count; ++i)
        m3_wasi_context_clear_preopen_slot(context, i);

    free(context->preopen);
    context->preopen = NULL;
    context->preopen_count = 0;
    context->preopen_capacity = 0;
}

static
const m3_wasi_preopen_t* m3_wasi_context_get_preopen(const m3_wasi_context_t* context,
                                                     __wasi_fd_t fd)
{
    if (!context || fd >= context->preopen_count || context->preopen[fd].path == NULL)
        return NULL;

    return &context->preopen[fd];
}

m3_wasi_context_t* m3_NewWasiContext(void)
{
    m3_wasi_context_t* context = (m3_wasi_context_t*)calloc(1, sizeof(m3_wasi_context_t));

    if (context)
        m3_wasi_context_init_preopens(context);

    return context;
}

void m3_FreeWasiContext(m3_wasi_context_t* context)
{
    m3_wasi_context_close_preopens(context);
    free(context);
}

void m3_wasi_context_reset_preopens(m3_wasi_context_t* context)
{
    m3_wasi_context_close_preopens(context);
    m3_wasi_context_init_stdio(context);
}

int m3_wasi_context_set_preopen(m3_wasi_context_t* context,
                                uint32_t fd,
                                const char* guest_path,
                                const char* host_path)
{
    if (fd < 3)
        return -1;

    return m3_wasi_context_set_preopen_slot(context, fd, guest_path, host_path);
}

int m3_wasi_context_remove_preopen(m3_wasi_context_t* context, uint32_t fd)
{
    if (!context || fd < 3 || fd >= context->preopen_count)
        return -1;

    m3_wasi_context_clear_preopen_slot(context, fd);
    context->preopen[fd].fd = -1;
    while (context->preopen_count > 0 &&
           context->preopen[context->preopen_count - 1].path == NULL) {
        context->preopen_count--;
    }
    return 0;
}

static
m3_wasi_context_t* m3_GetOrCreateDefaultWasiContext(void)
{
    if (!s_default_wasi_context)
        s_default_wasi_context = m3_NewWasiContext();

    return s_default_wasi_context;
}

typedef struct wasi_iovec_t
{
    __wasi_size_t buf;
    __wasi_size_t buf_len;
} wasi_iovec_t;

#if defined(APE)
#  define APE_SWITCH_BEG
#  define APE_SWITCH_END          {}
#  define APE_CASE_RET(e1,e2)     if (errnum == e1)    return e2;   else
#else
#  define APE_SWITCH_BEG          switch (errnum) {
#  define APE_SWITCH_END          }
#  define APE_CASE_RET(e1,e2)     case e1:   return e2;   break;
#endif

static
__wasi_errno_t errno_to_wasi(int errnum) {
    APE_SWITCH_BEG
    APE_CASE_RET( EPERM   , __WASI_ERRNO_PERM   )
    APE_CASE_RET( ENOENT  , __WASI_ERRNO_NOENT  )
    APE_CASE_RET( ESRCH   , __WASI_ERRNO_SRCH   )
    APE_CASE_RET( EINTR   , __WASI_ERRNO_INTR   )
    APE_CASE_RET( EIO     , __WASI_ERRNO_IO     )
    APE_CASE_RET( ENXIO   , __WASI_ERRNO_NXIO   )
    APE_CASE_RET( E2BIG   , __WASI_ERRNO_2BIG   )
    APE_CASE_RET( ENOEXEC , __WASI_ERRNO_NOEXEC )
    APE_CASE_RET( EBADF   , __WASI_ERRNO_BADF   )
    APE_CASE_RET( ECHILD  , __WASI_ERRNO_CHILD  )
    APE_CASE_RET( EAGAIN  , __WASI_ERRNO_AGAIN  )
    APE_CASE_RET( ENOMEM  , __WASI_ERRNO_NOMEM  )
    APE_CASE_RET( EACCES  , __WASI_ERRNO_ACCES  )
    APE_CASE_RET( EFAULT  , __WASI_ERRNO_FAULT  )
    APE_CASE_RET( EBUSY   , __WASI_ERRNO_BUSY   )
    APE_CASE_RET( EEXIST  , __WASI_ERRNO_EXIST  )
    APE_CASE_RET( EXDEV   , __WASI_ERRNO_XDEV   )
    APE_CASE_RET( ENODEV  , __WASI_ERRNO_NODEV  )
    APE_CASE_RET( ENOTDIR , __WASI_ERRNO_NOTDIR )
    APE_CASE_RET( EISDIR  , __WASI_ERRNO_ISDIR  )
    APE_CASE_RET( EINVAL  , __WASI_ERRNO_INVAL  )
    APE_CASE_RET( ENFILE  , __WASI_ERRNO_NFILE  )
    APE_CASE_RET( EMFILE  , __WASI_ERRNO_MFILE  )
    APE_CASE_RET( ENOTTY  , __WASI_ERRNO_NOTTY  )
    APE_CASE_RET( ETXTBSY , __WASI_ERRNO_TXTBSY )
    APE_CASE_RET( EFBIG   , __WASI_ERRNO_FBIG   )
    APE_CASE_RET( ENOSPC  , __WASI_ERRNO_NOSPC  )
    APE_CASE_RET( ESPIPE  , __WASI_ERRNO_SPIPE  )
    APE_CASE_RET( EROFS   , __WASI_ERRNO_ROFS   )
    APE_CASE_RET( EMLINK  , __WASI_ERRNO_MLINK  )
    APE_CASE_RET( EPIPE   , __WASI_ERRNO_PIPE   )
    APE_CASE_RET( EDOM    , __WASI_ERRNO_DOM    )
    APE_CASE_RET( ERANGE  , __WASI_ERRNO_RANGE  )
    APE_SWITCH_END
    return __WASI_ERRNO_INVAL;
}

static inline
__wasi_errno_t turbo_fs_result_to_wasi(int ret)
{
    if (ret >= 0)
        return __WASI_ERRNO_SUCCESS;

    return errno_to_wasi(-ret);
}

static inline
__wasi_timestamp_t convert_timespec(const struct timespec *ts);

static inline
int convert_clockid(__wasi_clockid_t in);

#if defined(_WIN32) && !defined(__MINGW32__)
static inline
int clock_gettime(int clk_id, struct timespec *spec);

static inline
int clock_getres(int clk_id, struct timespec *spec);
#endif

static inline
__wasi_filetype_t stat_mode_to_wasi_filetype(unsigned int mode)
{
#ifdef _WIN32
    if ((mode & _S_IFMT) == _S_IFDIR)
        return __WASI_FILETYPE_DIRECTORY;
    if ((mode & _S_IFMT) == _S_IFREG)
        return __WASI_FILETYPE_REGULAR_FILE;
    return __WASI_FILETYPE_UNKNOWN;
#else
    if (S_ISBLK(mode)) return __WASI_FILETYPE_BLOCK_DEVICE;
    if (S_ISCHR(mode)) return __WASI_FILETYPE_CHARACTER_DEVICE;
    if (S_ISDIR(mode)) return __WASI_FILETYPE_DIRECTORY;
    if (S_ISREG(mode)) return __WASI_FILETYPE_REGULAR_FILE;
    if (S_ISLNK(mode)) return __WASI_FILETYPE_SYMBOLIC_LINK;
    if (S_ISSOCK(mode)) return __WASI_FILETYPE_SOCKET_STREAM;
    return __WASI_FILETYPE_UNKNOWN;
#endif
}

static inline
__wasi_timestamp_t seconds_to_wasi_timestamp(uint64_t seconds)
{
    return (__wasi_timestamp_t) seconds * UINT64_C(1000000000);
}

static inline
void write_wasi_filestat(uint8_t* buf,
                         size_t layout_size,
                         uint64_t dev,
                         uint64_t ino,
                         __wasi_filetype_t filetype,
                         uint64_t nlink,
                         uint64_t size,
                         __wasi_timestamp_t atim,
                         __wasi_timestamp_t mtim,
                         __wasi_timestamp_t ctim)
{
    memset(buf, 0, layout_size);
    m3ApiWriteMem64(buf + 0, dev);
    m3ApiWriteMem64(buf + 8, ino);
    m3ApiWriteMem8(buf + 16, filetype);
    if (layout_size == 56) {
        m3ApiWriteMem32(buf + 20, (uint32_t)nlink);
    } else {
        m3ApiWriteMem64(buf + 24, nlink);
    }
    m3ApiWriteMem64(buf + (layout_size == 56 ? 24 : 32), size);
    m3ApiWriteMem64(buf + (layout_size == 56 ? 32 : 40), atim);
    m3ApiWriteMem64(buf + (layout_size == 56 ? 40 : 48), mtim);
    m3ApiWriteMem64(buf + (layout_size == 56 ? 48 : 56), ctim);
}

static
__wasi_errno_t fill_fd_filestat(__wasi_fd_t fd, uint8_t* buf, size_t layout_size)
{
#ifdef _WIN32
    struct __stat64 st;
    if (_fstat64(fd, &st) != 0)
        return errno_to_wasi(errno);

    write_wasi_filestat(buf,
                        layout_size,
                        0,
                        0,
                        stat_mode_to_wasi_filetype(st.st_mode),
                        1,
                        (uint64_t)st.st_size,
                        seconds_to_wasi_timestamp((uint64_t)st.st_atime),
                        seconds_to_wasi_timestamp((uint64_t)st.st_mtime),
                        seconds_to_wasi_timestamp((uint64_t)st.st_ctime));
#else
    struct stat st;
    if (fstat(fd, &st) != 0)
        return errno_to_wasi(errno);

    write_wasi_filestat(buf,
                        layout_size,
                        (uint64_t)st.st_dev,
                        (uint64_t)st.st_ino,
                        stat_mode_to_wasi_filetype(st.st_mode),
                        (uint64_t)st.st_nlink,
                        (uint64_t)st.st_size,
                        convert_timespec(&st.st_atim),
                        convert_timespec(&st.st_mtim),
                        convert_timespec(&st.st_ctim));
#endif
    return __WASI_ERRNO_SUCCESS;
}

static
__wasi_errno_t guest_path_to_host(const m3_wasi_context_t* context,
                                  __wasi_fd_t dirfd,
                                  const char* path,
                                  __wasi_size_t path_len,
                                  char* out,
                                  size_t out_len)
{
    const m3_wasi_preopen_t* preopen;
    const char* base;
    size_t relative_len;
    const char* relative_path;

    preopen = m3_wasi_context_get_preopen(context, dirfd);
    if (!preopen)
        return __WASI_ERRNO_BADF;
    if (path == NULL || out == NULL || out_len == 0)
        return __WASI_ERRNO_INVAL;
    if (path_len >= out_len)
        return __WASI_ERRNO_NAMETOOLONG;

    base = preopen->real_path;
    if (base == NULL)
        return __WASI_ERRNO_BADF;

    relative_path = path;
    relative_len = path_len;
    while (relative_len > 0 &&
           (*relative_path == '/' || *relative_path == '\\')) {
        relative_path++;
        relative_len--;
    }

    if (relative_len == 0) {
        size_t base_len = strlen(base);
        if (base_len + 1 > out_len)
            return __WASI_ERRNO_NAMETOOLONG;
        memcpy(out, base, base_len + 1);
        return __WASI_ERRNO_SUCCESS;
    }

#if defined(M3_COMPILER_MSVC)
    char relative_buf[1024];
#else
    char relative_buf[path_len + 1];
#endif
    memcpy(relative_buf, relative_path, relative_len);
    relative_buf[relative_len] = '\0';

    if (turbo_fs_path_join(out, out_len, base, relative_buf) != 0)
        return __WASI_ERRNO_NAMETOOLONG;
    return __WASI_ERRNO_SUCCESS;
}

static
__wasi_errno_t fill_path_filestat(const m3_wasi_context_t* context,
                                  __wasi_fd_t dirfd,
                                  __wasi_lookupflags_t flags,
                                  const char* path,
                                  __wasi_size_t path_len,
                                  uint8_t* buf,
                                  size_t layout_size)
{
#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err = guest_path_to_host(context, dirfd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        return err;

#ifdef _WIN32
    struct __stat64 st;
    if (_stat64(host_path, &st) != 0)
        return errno_to_wasi(errno);

    UNUSED(flags);
    write_wasi_filestat(buf,
                        layout_size,
                        0,
                        0,
                        stat_mode_to_wasi_filetype(st.st_mode),
                        1,
                        (uint64_t)st.st_size,
                        seconds_to_wasi_timestamp((uint64_t)st.st_atime),
                        seconds_to_wasi_timestamp((uint64_t)st.st_mtime),
                        seconds_to_wasi_timestamp((uint64_t)st.st_ctime));
#else
    struct stat st;
    int rc;
    if (flags & __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW)
        rc = stat(host_path, &st);
    else
        rc = lstat(host_path, &st);
    if (rc != 0)
        return errno_to_wasi(errno);

    write_wasi_filestat(buf,
                        layout_size,
                        (uint64_t)st.st_dev,
                        (uint64_t)st.st_ino,
                        stat_mode_to_wasi_filetype(st.st_mode),
                        (uint64_t)st.st_nlink,
                        (uint64_t)st.st_size,
                        convert_timespec(&st.st_atim),
                        convert_timespec(&st.st_mtim),
                        convert_timespec(&st.st_ctim));
#endif
    return __WASI_ERRNO_SUCCESS;
}

static
__wasi_errno_t wasi_clock_now(__wasi_clockid_t wasi_clk_id, __wasi_timestamp_t* out)
{
    if (out == NULL)
        return __WASI_ERRNO_INVAL;

    switch (wasi_clk_id) {
    case __WASI_CLOCKID_REALTIME:
        *out = turbo_ms_to_ns(turbo_realtime_ms());
        return __WASI_ERRNO_SUCCESS;
    case __WASI_CLOCKID_MONOTONIC:
        *out = turbo_hrtime();
        return __WASI_ERRNO_SUCCESS;
    default: {
        int clk = convert_clockid(wasi_clk_id);
        struct timespec tp;
        if (clk < 0)
            return __WASI_ERRNO_INVAL;
        if (clock_gettime(clk, &tp) != 0)
            return errno_to_wasi(errno);
        *out = convert_timespec(&tp);
        return __WASI_ERRNO_SUCCESS;
    }
    }
}

static void wasi_sleep_ns(__wasi_timestamp_t duration_ns)
{
    if (duration_ns == 0)
        return;
#ifdef _WIN32
    DWORD ms = (DWORD)((duration_ns + 999999ULL) / 1000000ULL);
    if (ms == 0)
        ms = 1;
    Sleep(ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(duration_ns / 1000000000ULL);
    req.tv_nsec = (long)(duration_ns % 1000000000ULL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
#endif
}

static
__wasi_filesize_t estimate_fd_ready_bytes(__wasi_fd_t fd, __wasi_eventtype_t type)
{
    if (type == __WASI_EVENTTYPE_FD_WRITE)
        return 1;

#ifdef _WIN32
    {
        struct __stat64 st;
        int64_t pos;
        if (_fstat64(fd, &st) != 0)
            return 0;
        pos = turbo_fs_tell((turbo_file_t)fd);
        if (pos < 0 || (uint64_t)pos >= (uint64_t)st.st_size)
            return 0;
        return (uint64_t)st.st_size - (uint64_t)pos;
    }
#else
    {
        struct stat st;
        int64_t pos;
        if (fstat(fd, &st) != 0)
            return 0;
        pos = turbo_fs_tell((turbo_file_t)fd);
        if (pos < 0 || (uint64_t)pos >= (uint64_t)st.st_size)
            return 0;
        return (uint64_t)st.st_size - (uint64_t)pos;
    }
#endif
}

static
__wasi_errno_t validate_filestat_set_times_flags(__wasi_fstflags_t fst_flags)
{
    const __wasi_fstflags_t known =
        __WASI_FSTFLAGS_ATIM |
        __WASI_FSTFLAGS_ATIM_NOW |
        __WASI_FSTFLAGS_MTIM |
        __WASI_FSTFLAGS_MTIM_NOW;

    if ((fst_flags & ~known) != 0)
        return __WASI_ERRNO_INVAL;
    if ((fst_flags & __WASI_FSTFLAGS_ATIM) &&
        (fst_flags & __WASI_FSTFLAGS_ATIM_NOW))
        return __WASI_ERRNO_INVAL;
    if ((fst_flags & __WASI_FSTFLAGS_MTIM) &&
        (fst_flags & __WASI_FSTFLAGS_MTIM_NOW))
        return __WASI_ERRNO_INVAL;

    return __WASI_ERRNO_SUCCESS;
}

#ifdef _WIN32
static
__wasi_errno_t win32_error_to_wasi(DWORD err)
{
    switch (err) {
    case ERROR_SUCCESS:
        return __WASI_ERRNO_SUCCESS;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_INVALID_NAME:
        return __WASI_ERRNO_NOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return __WASI_ERRNO_ACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return __WASI_ERRNO_EXIST;
    case ERROR_INVALID_HANDLE:
        return __WASI_ERRNO_BADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return __WASI_ERRNO_NOMEM;
    case ERROR_DIR_NOT_EMPTY:
        return __WASI_ERRNO_NOTEMPTY;
    case ERROR_DIRECTORY:
        return __WASI_ERRNO_NOTDIR;
    case ERROR_NOT_SAME_DEVICE:
        return __WASI_ERRNO_XDEV;
    case ERROR_BROKEN_PIPE:
        return __WASI_ERRNO_PIPE;
    case ERROR_DISK_FULL:
        return __WASI_ERRNO_NOSPC;
    case ERROR_INVALID_PARAMETER:
        return __WASI_ERRNO_INVAL;
    case ERROR_BUFFER_OVERFLOW:
    case ERROR_FILENAME_EXCED_RANGE:
        return __WASI_ERRNO_NAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES:
        return __WASI_ERRNO_NFILE;
    case ERROR_BUSY:
        return __WASI_ERRNO_BUSY;
    case ERROR_OPERATION_ABORTED:
        return __WASI_ERRNO_INTR;
    case ERROR_TIMEOUT:
    case ERROR_SEM_TIMEOUT:
        return __WASI_ERRNO_TIMEDOUT;
    case ERROR_CALL_NOT_IMPLEMENTED:
        return __WASI_ERRNO_NOSYS;
    case ERROR_NOT_SUPPORTED:
        return __WASI_ERRNO_NOTSUP;
    default:
        return __WASI_ERRNO_IO;
    }
}

static
void wasi_timestamp_to_filetime(__wasi_timestamp_t timestamp, FILETIME* out)
{
    const uint64_t unix_epoch_to_filetime = UINT64_C(116444736000000000);
    uint64_t ticks = timestamp / 100ULL;
    uint64_t filetime = unix_epoch_to_filetime + ticks;

    out->dwLowDateTime = (DWORD)(filetime & 0xffffffffu);
    out->dwHighDateTime = (DWORD)(filetime >> 32);
}

static
void current_filetime(FILETIME* out)
{
    GetSystemTimeAsFileTime(out);
}

static
__wasi_errno_t set_handle_times(HANDLE handle,
                                __wasi_timestamp_t atim,
                                __wasi_timestamp_t mtim,
                                __wasi_fstflags_t fst_flags)
{
    FILETIME atime;
    FILETIME mtime;
    FILETIME* atime_ptr = NULL;
    FILETIME* mtime_ptr = NULL;
    __wasi_errno_t err = validate_filestat_set_times_flags(fst_flags);

    if (err != __WASI_ERRNO_SUCCESS)
        return err;

    if ((fst_flags & __WASI_FSTFLAGS_ATIM) != 0) {
        wasi_timestamp_to_filetime(atim, &atime);
        atime_ptr = &atime;
    } else if ((fst_flags & __WASI_FSTFLAGS_ATIM_NOW) != 0) {
        current_filetime(&atime);
        atime_ptr = &atime;
    }

    if ((fst_flags & __WASI_FSTFLAGS_MTIM) != 0) {
        wasi_timestamp_to_filetime(mtim, &mtime);
        mtime_ptr = &mtime;
    } else if ((fst_flags & __WASI_FSTFLAGS_MTIM_NOW) != 0) {
        current_filetime(&mtime);
        mtime_ptr = &mtime;
    }

    if (!SetFileTime(handle, NULL, atime_ptr, mtime_ptr))
        return win32_error_to_wasi(GetLastError());

    return __WASI_ERRNO_SUCCESS;
}
#else
static
__wasi_errno_t build_utimens_times(__wasi_timestamp_t atim,
                                   __wasi_timestamp_t mtim,
                                   __wasi_fstflags_t fst_flags,
                                   struct timespec times[2])
{
    __wasi_errno_t err = validate_filestat_set_times_flags(fst_flags);

    if (err != __WASI_ERRNO_SUCCESS)
        return err;

    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = 0;
    times[1].tv_nsec = UTIME_OMIT;

    if ((fst_flags & __WASI_FSTFLAGS_ATIM) != 0) {
        times[0].tv_sec = (time_t)(atim / UINT64_C(1000000000));
        times[0].tv_nsec = (long)(atim % UINT64_C(1000000000));
    } else if ((fst_flags & __WASI_FSTFLAGS_ATIM_NOW) != 0) {
        times[0].tv_nsec = UTIME_NOW;
    }

    if ((fst_flags & __WASI_FSTFLAGS_MTIM) != 0) {
        times[1].tv_sec = (time_t)(mtim / UINT64_C(1000000000));
        times[1].tv_nsec = (long)(mtim % UINT64_C(1000000000));
    } else if ((fst_flags & __WASI_FSTFLAGS_MTIM_NOW) != 0) {
        times[1].tv_nsec = UTIME_NOW;
    }

    return __WASI_ERRNO_SUCCESS;
}
#endif

#if defined(_WIN32)

#if !defined(__MINGW32__)

static inline
int clock_gettime(int clk_id, struct timespec *spec)
{
    __int64 wintime; GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime      -= 116444736000000000i64;           //1jan1601 to 1jan1970
    spec->tv_sec  = wintime / 10000000i64;           //seconds
    spec->tv_nsec = wintime % 10000000i64 *100;      //nano-seconds
    return 0;
}

static inline
int clock_getres(int clk_id, struct timespec *spec) {
    return -1; // Defaults to 1000000
}

#endif

static inline
int convert_clockid(__wasi_clockid_t in) {
    return 0;
}

#else // _WIN32

static inline
int convert_clockid(__wasi_clockid_t in) {
    switch (in) {
    case __WASI_CLOCKID_MONOTONIC:            return CLOCK_MONOTONIC;
    case __WASI_CLOCKID_PROCESS_CPUTIME_ID:   return CLOCK_PROCESS_CPUTIME_ID;
    case __WASI_CLOCKID_REALTIME:             return CLOCK_REALTIME;
    case __WASI_CLOCKID_THREAD_CPUTIME_ID:    return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

#endif // _WIN32

static inline
__wasi_timestamp_t convert_timespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((__wasi_timestamp_t)ts->tv_sec >= UINT64_MAX / 1000000000)
        return UINT64_MAX;
    return (__wasi_timestamp_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

#if defined(HAS_IOVEC)

static inline
const void* copy_iov_to_host(IM3Runtime runtime, void* _mem, struct iovec* host_iov, wasi_iovec_t* wasi_iov, int32_t iovs_len)
{
    // Convert wasi memory offsets to host addresses
    for (int i = 0; i < iovs_len; i++) {
        host_iov[i].iov_base = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iov[i].buf));
        host_iov[i].iov_len  = m3ApiReadMem32(&wasi_iov[i].buf_len);
        m3ApiCheckMem(host_iov[i].iov_base,     host_iov[i].iov_len);
    }
    m3ApiSuccess();
}

#endif

/*
 * WASI API implementation
 */

m3ApiRawFunction(m3_wasi_generic_args_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *           , argv)
    m3ApiGetArgMem   (char *               , argv_buf)

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context == NULL) { m3ApiReturn(__WASI_ERRNO_INVAL); }

    m3ApiCheckMem(argv, context->argc * sizeof(uint32_t));

    for (u32 i = 0; i < context->argc; ++i)
    {
        m3ApiWriteMem32(&argv[i], m3ApiPtrToOffset(argv_buf));

        size_t len = strlen (context->argv[i]);

        m3ApiCheckMem(argv_buf, len);
        memcpy (argv_buf, context->argv[i], len);
        argv_buf += len;
        * argv_buf++ = 0;
    }

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_args_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t *      , argc)
    m3ApiGetArgMem   (__wasi_size_t *      , argv_buf_size)

    m3ApiCheckMem(argc,             sizeof(__wasi_size_t));
    m3ApiCheckMem(argv_buf_size,    sizeof(__wasi_size_t));

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context == NULL) { m3ApiReturn(__WASI_ERRNO_INVAL); }

    __wasi_size_t buf_len = 0;
    for (u32 i = 0; i < context->argc; ++i)
    {
        buf_len += strlen (context->argv[i]) + 1;
    }

    m3ApiWriteMem32(argc, context->argc);
    m3ApiWriteMem32(argv_buf_size, buf_len);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_environ_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *           , env)
    m3ApiGetArgMem   (char *               , env_buf)

    char** host_env = environ;
    uint32_t env_buf_offset = m3ApiPtrToOffset(env_buf);
    char* cursor = env_buf;
    __wasi_size_t count = 0;
    __wasi_size_t buf_size = 0;

    while (host_env != NULL && host_env[count] != NULL) {
        buf_size += (__wasi_size_t)strlen(host_env[count]) + 1;
        count++;
    }

    m3ApiCheckMem(env, count * sizeof(uint32_t));
    m3ApiCheckMem(env_buf, buf_size);

    for (__wasi_size_t i = 0; i < count; ++i)
    {
        size_t len = strlen(host_env[i]) + 1;
        m3ApiWriteMem32(&env[i], env_buf_offset + (uint32_t)(cursor - env_buf));
        memcpy(cursor, host_env[i], len);
        cursor += len;
    }
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t *      , env_count)
    m3ApiGetArgMem   (__wasi_size_t *      , env_buf_size)

    m3ApiCheckMem(env_count,    sizeof(__wasi_size_t));
    m3ApiCheckMem(env_buf_size, sizeof(__wasi_size_t));

    char** host_env = environ;
    __wasi_size_t count = 0;
    __wasi_size_t buf_size = 0;

    while (host_env != NULL && host_env[count] != NULL) {
        buf_size += (__wasi_size_t)strlen(host_env[count]) + 1;
        count++;
    }

    m3ApiWriteMem32(env_count,    count);
    m3ApiWriteMem32(env_buf_size, buf_size);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_prestat_dir_name)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (char *               , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)
    m3_wasi_context_t* context = m3_GetWasiContextForRuntime(runtime);
    const m3_wasi_preopen_t* preopen;

    m3ApiCheckMem(path, path_len);

    preopen = m3_wasi_context_get_preopen(context, fd);
    if (fd < 3 || !preopen) { m3ApiReturn(__WASI_ERRNO_BADF); }
    size_t slen = strlen(preopen->path);
    memcpy(path, preopen->path, M3_MIN(slen, path_len));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_prestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint8_t *            , buf)
    m3_wasi_context_t* context = m3_GetWasiContextForRuntime(runtime);
    const m3_wasi_preopen_t* preopen;

    m3ApiCheckMem(buf, 8);

    preopen = m3_wasi_context_get_preopen(context, fd);
    if (fd < 3 || !preopen) { m3ApiReturn(__WASI_ERRNO_BADF); }

    m3ApiWriteMem32(buf+0, __WASI_PREOPENTYPE_DIR);
    m3ApiWriteMem32(buf+4, strlen(preopen->path));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_fdstat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (__wasi_fdstat_t *    , fdstat)

    m3ApiCheckMem(fdstat, sizeof(__wasi_fdstat_t));

#ifdef _WIN32

    // TODO: This needs a proper implementation
    if (m3_wasi_context_get_preopen(m3_GetWasiContextForRuntime(runtime), fd)) {
        fdstat->fs_filetype= __WASI_FILETYPE_DIRECTORY;
    } else {
        fdstat->fs_filetype= __WASI_FILETYPE_REGULAR_FILE;
    }

    fdstat->fs_flags = 0;
    fdstat->fs_rights_base = (uint64_t)-1; // all rights
    fdstat->fs_rights_inheriting = (uint64_t)-1; // all rights
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#else
    struct stat fd_stat;

#if !defined(APE) // TODO: not implemented in Cosmopolitan
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { m3ApiReturn(errno_to_wasi(errno)); }
#endif

    fstat(fd, &fd_stat);
    int mode = fd_stat.st_mode;
    fdstat->fs_filetype = (S_ISBLK(mode)   ? __WASI_FILETYPE_BLOCK_DEVICE     : 0) |
                          (S_ISCHR(mode)   ? __WASI_FILETYPE_CHARACTER_DEVICE : 0) |
                          (S_ISDIR(mode)   ? __WASI_FILETYPE_DIRECTORY        : 0) |
                          (S_ISREG(mode)   ? __WASI_FILETYPE_REGULAR_FILE     : 0) |
                          //(S_ISSOCK(mode)  ? __WASI_FILETYPE_SOCKET_STREAM    : 0) |
                          (S_ISLNK(mode)   ? __WASI_FILETYPE_SYMBOLIC_LINK    : 0);
#if !defined(APE)
    m3ApiWriteMem16(&fdstat->fs_flags,
                       ((fl & O_APPEND)    ? __WASI_FDFLAGS_APPEND    : 0) |
                       ((fl & O_DSYNC)     ? __WASI_FDFLAGS_DSYNC     : 0) |
                       ((fl & O_NONBLOCK)  ? __WASI_FDFLAGS_NONBLOCK  : 0) |
                       //((fl & O_RSYNC)     ? __WASI_FDFLAGS_RSYNC     : 0) |
                       ((fl & O_SYNC)      ? __WASI_FDFLAGS_SYNC      : 0));
#endif // APE

    fdstat->fs_rights_base = (uint64_t)-1; // all rights

    // Make descriptors 0,1,2 look like a TTY
    if (fd <= 2) {
        fdstat->fs_rights_base &= ~(__WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL);
    }

    fdstat->fs_rights_inheriting = (uint64_t)-1; // all rights
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_generic_fd_fdstat_set_flags)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_fdflags_t     , flags)

#ifdef _WIN32
    UNUSED(fd);
    UNUSED(flags);
#else
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) { m3ApiReturn(errno_to_wasi(errno)); }

    if (flags & __WASI_FDFLAGS_APPEND)
        fl |= O_APPEND;
    else
        fl &= ~O_APPEND;

    if (flags & __WASI_FDFLAGS_NONBLOCK)
        fl |= O_NONBLOCK;
    else
        fl &= ~O_NONBLOCK;

    if (fcntl(fd, F_SETFL, fl) < 0)
        m3ApiReturn(errno_to_wasi(errno));
#endif
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_fdstat_set_rights)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_base)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_inheriting)

    /*
     * simple WASI does not maintain a capability table for host descriptors.
     * Keep this as a validated no-op so callers can degrade cleanly.
     */
    UNUSED(fd);
    UNUSED(fs_rights_base);
    UNUSED(fs_rights_inheriting);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_fd_filestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint8_t *            , buf)

    m3ApiCheckMem(buf, 56);
    m3ApiReturn(fill_fd_filestat(fd, buf, 56));
}

m3ApiRawFunction(m3_wasi_snapshot_preview1_fd_filestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint8_t *            , buf)

    m3ApiCheckMem(buf, 64);
    m3ApiReturn(fill_fd_filestat(fd, buf, 64));
}

m3ApiRawFunction(m3_wasi_unstable_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filedelta_t   , offset)
    m3ApiGetArg      (uint32_t             , wasi_whence)
    m3ApiGetArgMem   (__wasi_filesize_t *  , result)

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));

    int whence;

    switch (wasi_whence) {
    case 0: whence = SEEK_CUR; break;
    case 1: whence = SEEK_END; break;
    case 2: whence = SEEK_SET; break;
    default:                m3ApiReturn(__WASI_ERRNO_INVAL);
    }

    int64_t ret = turbo_fs_seek((turbo_file_t)fd, offset, whence);
    if (ret < 0) { m3ApiReturn(errno_to_wasi((int)-ret)); }
    m3ApiWriteMem64(result, ret);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_snapshot_preview1_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filedelta_t   , offset)
    m3ApiGetArg      (uint32_t             , wasi_whence)
    m3ApiGetArgMem   (__wasi_filesize_t *  , result)

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));

    int whence;

    switch (wasi_whence) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:                m3ApiReturn(__WASI_ERRNO_INVAL);
    }

    int64_t ret = turbo_fs_seek((turbo_file_t)fd, offset, whence);
    if (ret < 0) { m3ApiReturn(errno_to_wasi((int)-ret)); }
    m3ApiWriteMem64(result, ret);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_advise)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filesize_t    , offset)
    m3ApiGetArg      (__wasi_filesize_t    , length)
    m3ApiGetArg      (__wasi_advice_t      , advice)

    UNUSED(fd);
    UNUSED(offset);
    UNUSED(length);
    UNUSED(advice);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_allocate)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filesize_t    , offset)
    m3ApiGetArg      (__wasi_filesize_t    , length)

    uint64_t requested;
    uint64_t current_size;

    if (UINT64_MAX - offset < length)
        m3ApiReturn(__WASI_ERRNO_FBIG);
    requested = offset + length;

#ifdef _WIN32
    struct __stat64 st;
    if (_fstat64(fd, &st) != 0)
        m3ApiReturn(errno_to_wasi(errno));
    current_size = (uint64_t)st.st_size;
#else
    struct stat st;
    if (fstat(fd, &st) != 0)
        m3ApiReturn(errno_to_wasi(errno));
    current_size = (uint64_t)st.st_size;
#endif

    if (current_size >= requested)
        m3ApiReturn(__WASI_ERRNO_SUCCESS);

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_ftruncate((turbo_file_t)fd,
                                                           (int64_t)requested)));
}

m3ApiRawFunction(m3_wasi_generic_fd_filestat_set_size)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filesize_t    , size)

    if (size > INT64_MAX)
        m3ApiReturn(__WASI_ERRNO_FBIG);

    m3ApiReturn(turbo_fs_result_to_wasi(
        turbo_fs_ftruncate((turbo_file_t)fd, (int64_t)size)));
}

m3ApiRawFunction(m3_wasi_generic_fd_filestat_set_times)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_timestamp_t   , atim)
    m3ApiGetArg      (__wasi_timestamp_t   , mtim)
    m3ApiGetArg      (__wasi_fstflags_t    , fst_flags)

#ifdef _WIN32
    intptr_t handle_value = _get_osfhandle(fd);
    HANDLE handle;

    if (handle_value == -1)
        m3ApiReturn(errno_to_wasi(errno));
    handle = (HANDLE)handle_value;
    m3ApiReturn(set_handle_times(handle, atim, mtim, fst_flags));
#else
    struct timespec times[2];
    __wasi_errno_t err = build_utimens_times(atim, mtim, fst_flags, times);

    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);
    if (futimens(fd, times) != 0)
        m3ApiReturn(errno_to_wasi(errno));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_unstable_path_filestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_lookupflags_t , flags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (uint32_t             , path_len)
    m3ApiGetArgMem   (uint8_t *            , buf)

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf, 56);
    m3ApiReturn(fill_path_filestat(m3_GetWasiContextForRuntime(runtime),
                                   fd, flags, path, path_len, buf, 56));
}

m3ApiRawFunction(m3_wasi_snapshot_preview1_path_filestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_lookupflags_t , flags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (uint32_t             , path_len)
    m3ApiGetArgMem   (uint8_t *            , buf)

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf, 64);
    m3ApiReturn(fill_path_filestat(m3_GetWasiContextForRuntime(runtime),
                                   fd, flags, path, path_len, buf, 64));
}

m3ApiRawFunction(m3_wasi_generic_path_create_directory)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(path, path_len);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_mkdir(host_path, 0755)));
}

m3ApiRawFunction(m3_wasi_generic_path_readlink)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)
    m3ApiGetArgMem   (char *               , buf)
    m3ApiGetArg      (__wasi_size_t        , buf_len)
    m3ApiGetArgMem   (__wasi_size_t *      , bufused)

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf, buf_len);
    m3ApiCheckMem(bufused, sizeof(__wasi_size_t));

    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

#ifdef _WIN32
    UNUSED(host_path);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    {
        ssize_t n = readlink(host_path, buf, buf_len);
        if (n < 0)
            m3ApiReturn(errno_to_wasi(errno));
        m3ApiWriteMem32(bufused, (uint32_t)n);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_path_filestat_set_times)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_lookupflags_t , flags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)
    m3ApiGetArg      (__wasi_timestamp_t   , atim)
    m3ApiGetArg      (__wasi_timestamp_t   , mtim)
    m3ApiGetArg      (__wasi_fstflags_t    , fst_flags)

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(path, path_len);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

#ifdef _WIN32
    {
        DWORD file_flags = FILE_FLAG_BACKUP_SEMANTICS;
        HANDLE handle;

        if ((flags & __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW) == 0)
            file_flags |= FILE_FLAG_OPEN_REPARSE_POINT;

        handle = CreateFileA(host_path,
                             FILE_WRITE_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             file_flags,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE)
            m3ApiReturn(win32_error_to_wasi(GetLastError()));

        err = set_handle_times(handle, atim, mtim, fst_flags);
        CloseHandle(handle);
        m3ApiReturn(err);
    }
#else
    {
        struct timespec times[2];
        int utimens_flags = 0;

        err = build_utimens_times(atim, mtim, fst_flags, times);
        if (err != __WASI_ERRNO_SUCCESS)
            m3ApiReturn(err);

#ifdef AT_SYMLINK_NOFOLLOW
        if ((flags & __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW) == 0)
            utimens_flags |= AT_SYMLINK_NOFOLLOW;
#else
        UNUSED(flags);
#endif

        if (utimensat(AT_FDCWD, host_path, times, utimens_flags) != 0)
            m3ApiReturn(errno_to_wasi(errno));
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_path_remove_directory)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(path, path_len);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_rmdir(host_path)));
}

m3ApiRawFunction(m3_wasi_generic_path_rename)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , old_fd)
    m3ApiGetArgMem   (const char *         , old_path)
    m3ApiGetArg      (__wasi_size_t        , old_path_len)
    m3ApiGetArg      (__wasi_fd_t          , new_fd)
    m3ApiGetArgMem   (const char *         , new_path)
    m3ApiGetArg      (__wasi_size_t        , new_path_len)

#if defined(M3_COMPILER_MSVC)
    char old_host[1024];
    char new_host[1024];
#else
    char old_host[old_path_len + TURBO_FS_MAX_PATH + 2];
    char new_host[new_path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(old_path, old_path_len);
    m3ApiCheckMem(new_path, new_path_len);

    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             old_fd, old_path, old_path_len, old_host, sizeof(old_host));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             new_fd, new_path, new_path_len, new_host, sizeof(new_host));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_rename(old_host, new_host)));
}

m3ApiRawFunction(m3_wasi_generic_path_symlink)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (const char *         , old_path)
    m3ApiGetArg      (__wasi_size_t        , old_path_len)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (const char *         , new_path)
    m3ApiGetArg      (__wasi_size_t        , new_path_len)

#if defined(M3_COMPILER_MSVC)
    char link_target[1024];
    char new_host[1024];
#else
    char link_target[old_path_len + 1];
    char new_host[new_path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(old_path, old_path_len);
    m3ApiCheckMem(new_path, new_path_len);

    memcpy(link_target, old_path, old_path_len);
    link_target[old_path_len] = '\0';

    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, new_path, new_path_len, new_host, sizeof(new_host));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

#ifdef _WIN32
    UNUSED(link_target);
    UNUSED(new_host);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    if (symlink(link_target, new_host) != 0)
        m3ApiReturn(errno_to_wasi(errno));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_generic_path_link)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , old_fd)
    m3ApiGetArg      (__wasi_lookupflags_t , old_flags)
    m3ApiGetArgMem   (const char *         , old_path)
    m3ApiGetArg      (__wasi_size_t        , old_path_len)
    m3ApiGetArg      (__wasi_fd_t          , new_fd)
    m3ApiGetArgMem   (const char *         , new_path)
    m3ApiGetArg      (__wasi_size_t        , new_path_len)

#if defined(M3_COMPILER_MSVC)
    char old_host[1024];
    char new_host[1024];
#else
    char old_host[old_path_len + TURBO_FS_MAX_PATH + 2];
    char new_host[new_path_len + TURBO_FS_MAX_PATH + 2];
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(old_path, old_path_len);
    m3ApiCheckMem(new_path, new_path_len);

    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             old_fd, old_path, old_path_len, old_host, sizeof(old_host));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             new_fd, new_path, new_path_len, new_host, sizeof(new_host));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

#ifdef _WIN32
    UNUSED(old_flags);
    if (!CreateHardLinkA(new_host, old_host, NULL))
        m3ApiReturn(win32_error_to_wasi(GetLastError()));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#else
    {
        int link_flags = 0;
#ifdef AT_SYMLINK_FOLLOW
        if (old_flags & __WASI_LOOKUPFLAGS_SYMLINK_FOLLOW)
            link_flags |= AT_SYMLINK_FOLLOW;
#else
        UNUSED(old_flags);
#endif
        if (linkat(AT_FDCWD, old_host, AT_FDCWD, new_host, link_flags) != 0)
            m3ApiReturn(errno_to_wasi(errno));
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_path_unlink_file)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
    struct __stat64 st;
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
    struct stat st;
#endif
    __wasi_errno_t err;

    m3ApiCheckMem(path, path_len);
    err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                             fd, path, path_len, host_path, sizeof(host_path));
    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

#ifdef _WIN32
    if (_stat64(host_path, &st) == 0 && ((st.st_mode & _S_IFMT) == _S_IFDIR))
        m3ApiReturn(__WASI_ERRNO_ISDIR);
#else
    if (lstat(host_path, &st) == 0 && S_ISDIR(st.st_mode))
        m3ApiReturn(__WASI_ERRNO_ISDIR);
#endif

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_unlink(host_path)));
}


m3ApiRawFunction(m3_wasi_generic_path_open)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , dirfd)
    m3ApiGetArg      (__wasi_lookupflags_t , dirflags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)
    m3ApiGetArg      (__wasi_oflags_t      , oflags)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_base)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_inheriting)
    m3ApiGetArg      (__wasi_fdflags_t     , fs_flags)
    m3ApiGetArgMem   (__wasi_fd_t *        , fd)

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(fd,   sizeof(__wasi_fd_t));

#if defined(M3_COMPILER_MSVC)
    char host_path[1024];
#else
    char host_path[path_len + TURBO_FS_MAX_PATH + 2];
#endif
    int turbo_flags = 0;
    __wasi_errno_t err = guest_path_to_host(m3_GetWasiContextForRuntime(runtime),
                                            dirfd, path, path_len, host_path, sizeof(host_path));
    turbo_file_t host_fd;

    UNUSED(dirflags);
    UNUSED(fs_rights_inheriting);

    if (err != __WASI_ERRNO_SUCCESS)
        m3ApiReturn(err);

    if ((fs_rights_base & __WASI_RIGHTS_FD_READ) &&
        (fs_rights_base & __WASI_RIGHTS_FD_WRITE))
        turbo_flags |= TURBO_FS_O_RDWR;
    else if (fs_rights_base & __WASI_RIGHTS_FD_WRITE)
        turbo_flags |= TURBO_FS_O_WRONLY;
    else
        turbo_flags |= TURBO_FS_O_RDONLY;

    if (oflags & __WASI_OFLAGS_CREAT)
        turbo_flags |= TURBO_FS_O_CREAT;
    if (oflags & __WASI_OFLAGS_TRUNC)
        turbo_flags |= TURBO_FS_O_TRUNC;
    if (fs_flags & __WASI_FDFLAGS_APPEND)
        turbo_flags |= TURBO_FS_O_APPEND;

    if ((oflags & __WASI_OFLAGS_EXCL) != 0) {
#ifdef _WIN32
        struct __stat64 st;
        if (_stat64(host_path, &st) == 0)
            m3ApiReturn(__WASI_ERRNO_EXIST);
#else
        struct stat st;
        if (stat(host_path, &st) == 0)
            m3ApiReturn(__WASI_ERRNO_EXIST);
#endif
    }

    host_fd = turbo_fs_open(host_path, turbo_flags, TURBO_FS_DEFAULT_MODE);
    if (host_fd == TURBO_INVALID_FILE)
        m3ApiReturn(errno_to_wasi(errno));

    if (oflags & __WASI_OFLAGS_DIRECTORY) {
        turbo_fs_stat_t st;
        if (turbo_fs_stat(host_path, &st) != 0 || !st.is_directory) {
            turbo_fs_close(host_fd);
            m3ApiReturn(__WASI_ERRNO_NOTDIR);
        }
    }

    m3ApiWriteMem32(fd, host_fd);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_read)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t *      , nread)

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nread,        sizeof(__wasi_size_t));

    ssize_t res = 0;
    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        void* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;
        m3ApiCheckMem(addr,     len);
        int ret = turbo_fs_read((turbo_file_t)fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        if ((size_t)ret < len) break;
    }
    m3ApiWriteMem32(nread, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_pread)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArg      (__wasi_filesize_t    , offset)
    m3ApiGetArgMem   (__wasi_size_t *      , nread)

    ssize_t res = 0;
    int64_t current_offset = (int64_t)offset;

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nread,        sizeof(__wasi_size_t));

    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        void* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;
        m3ApiCheckMem(addr, len);
        int ret = turbo_fs_pread((turbo_file_t)fd, addr, len, current_offset);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        current_offset += ret;
        if ((size_t)ret < len) break;
    }

    m3ApiWriteMem32(nread, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_write)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t *      , nwritten)

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nwritten,     sizeof(__wasi_size_t));

    ssize_t res = 0;
    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        void* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;
        m3ApiCheckMem(addr,     len);
        int ret = turbo_fs_write((turbo_file_t)fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        if ((size_t)ret < len) break;
    }
    m3ApiWriteMem32(nwritten, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_pwrite)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArg      (__wasi_filesize_t    , offset)
    m3ApiGetArgMem   (__wasi_size_t *      , nwritten)

    ssize_t res = 0;
    int64_t current_offset = (int64_t)offset;

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nwritten,     sizeof(__wasi_size_t));

    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        const char* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;
        m3ApiCheckMem(addr, len);
        int ret = turbo_fs_pwrite((turbo_file_t)fd, addr, len, current_offset);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        current_offset += ret;
        if ((size_t)ret < len) break;
    }

    m3ApiWriteMem32(nwritten, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_close)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    int ret = turbo_fs_close((turbo_file_t)fd);
    m3ApiReturn(turbo_fs_result_to_wasi(ret));
}

m3ApiRawFunction(m3_wasi_generic_fd_datasync)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    int ret = turbo_fs_fsync((turbo_file_t)fd);
    m3ApiReturn(turbo_fs_result_to_wasi(ret));
}

m3ApiRawFunction(m3_wasi_generic_fd_readdir)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint8_t *            , buf)
    m3ApiGetArg      (__wasi_size_t        , buf_len)
    m3ApiGetArg      (__wasi_dircookie_t   , cookie)
    m3ApiGetArgMem   (__wasi_size_t *      , bufused)

    m3ApiCheckMem(buf, buf_len);
    m3ApiCheckMem(bufused, sizeof(__wasi_size_t));

#ifdef _WIN32
    UNUSED(fd);
    UNUSED(cookie);
    m3ApiWriteMem32(bufused, 0);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    {
        __wasi_size_t used = 0;
        __wasi_dircookie_t index = 0;
        int dupfd = dup(fd);
        DIR* dir;
        struct dirent* entry;

        if (dupfd < 0)
            m3ApiReturn(errno_to_wasi(errno));
        dir = fdopendir(dupfd);
        if (dir == NULL) {
            close(dupfd);
            m3ApiReturn(errno_to_wasi(errno));
        }

        rewinddir(dir);
        while (index < cookie && (entry = readdir(dir)) != NULL) {
            index++;
        }

        while ((entry = readdir(dir)) != NULL) {
            __wasi_dirent_t dirent;
            size_t name_len = strlen(entry->d_name);
            size_t copy_len;

            memset(&dirent, 0, sizeof(dirent));
            dirent.d_next = ++index;
            dirent.d_ino = (uint64_t)entry->d_ino;
            dirent.d_namlen = (__wasi_dirnamlen_t)name_len;
#ifdef DT_BLK
            switch (entry->d_type) {
            case DT_BLK: dirent.d_type = __WASI_FILETYPE_BLOCK_DEVICE; break;
            case DT_CHR: dirent.d_type = __WASI_FILETYPE_CHARACTER_DEVICE; break;
            case DT_DIR: dirent.d_type = __WASI_FILETYPE_DIRECTORY; break;
            case DT_REG: dirent.d_type = __WASI_FILETYPE_REGULAR_FILE; break;
            case DT_LNK: dirent.d_type = __WASI_FILETYPE_SYMBOLIC_LINK; break;
            case DT_SOCK: dirent.d_type = __WASI_FILETYPE_SOCKET_STREAM; break;
            default: dirent.d_type = __WASI_FILETYPE_UNKNOWN; break;
            }
#else
            dirent.d_type = __WASI_FILETYPE_UNKNOWN;
#endif

            if (used + sizeof(dirent) > buf_len)
                break;

            memcpy(buf + used, &dirent, sizeof(dirent));
            used += (__wasi_size_t)sizeof(dirent);

            copy_len = M3_MIN(name_len, (size_t)(buf_len - used));
            memcpy(buf + used, entry->d_name, copy_len);
            used += (__wasi_size_t)copy_len;

            if (copy_len < name_len)
                break;
        }

        closedir(dir);
        m3ApiWriteMem32(bufused, used);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_fd_renumber)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , from)
    m3ApiGetArg      (__wasi_fd_t          , to)

#ifdef _WIN32
    if (_dup2(from, to) != 0)
        m3ApiReturn(errno_to_wasi(errno));
#else
    if (dup2(from, to) < 0)
        m3ApiReturn(errno_to_wasi(errno));
#endif
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_sync)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    m3ApiReturn(turbo_fs_result_to_wasi(turbo_fs_fsync((turbo_file_t)fd)));
}

m3ApiRawFunction(m3_wasi_generic_fd_tell)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (__wasi_filesize_t *  , result)

    int64_t offset;

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));
    offset = turbo_fs_tell((turbo_file_t)fd);
    if (offset < 0)
        m3ApiReturn(errno_to_wasi((int)-offset));

    m3ApiWriteMem64(result, (uint64_t)offset);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_sock_recv)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArg      (__wasi_riflags_t     , ri_flags)
    m3ApiGetArgMem   (__wasi_size_t *      , ro_datalen)
    m3ApiGetArgMem   (__wasi_roflags_t *   , ro_flags)

    m3ApiCheckMem(wasi_iovs, iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(ro_datalen, sizeof(__wasi_size_t));
    m3ApiCheckMem(ro_flags, sizeof(__wasi_roflags_t));

    {
        m3_wasi_context_t *context = (m3_wasi_context_t *)(_ctx->userdata);

        if (context && context->socket_ops && context->socket_ops->recv) {
            uint64_t total_len = 0;
            uint32_t recv_len = 0;
            uint16_t recv_flags = 0;
            uint8_t *tmp = NULL;
            __wasi_errno_t err;

            for (__wasi_size_t i = 0; i < iovs_len; ++i) {
                void *addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
                size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);

                if (len != 0)
                    m3ApiCheckMem(addr, len);
                total_len += len;
                if (total_len > UINT32_MAX)
                    m3ApiReturn(__WASI_ERRNO_INVAL);
            }

            if (total_len != 0) {
                tmp = (uint8_t *)malloc((size_t)total_len);
                if (!tmp)
                    m3ApiReturn(__WASI_ERRNO_NOMEM);
            }

            err = context->socket_ops->recv(context->socket_user_data,
                                            fd,
                                            tmp,
                                            (uint32_t)total_len,
                                            ri_flags,
                                            &recv_len,
                                            &recv_flags);
            if (err != __WASI_ERRNO_BADF) {
                if (err == __WASI_ERRNO_SUCCESS) {
                    uint32_t remaining = recv_len;
                    uint8_t *cursor = tmp;

                    for (__wasi_size_t i = 0; i < iovs_len && remaining != 0; ++i) {
                        uint8_t *addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
                        uint32_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
                        uint32_t copy_len = remaining < len ? remaining : len;

                        if (copy_len != 0)
                            memcpy(addr, cursor, copy_len);
                        cursor += copy_len;
                        remaining -= copy_len;
                    }
                    m3ApiWriteMem32(ro_datalen, recv_len);
                    m3ApiWriteMem16(ro_flags, recv_flags);
                }
                free(tmp);
                m3ApiReturn(err);
            }

            free(tmp);
        }
    }

#ifdef _WIN32
    UNUSED(fd);
    UNUSED(ri_flags);
    m3ApiWriteMem32(ro_datalen, 0);
    m3ApiWriteMem16(ro_flags, 0);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    {
        struct iovec host_iov[iovs_len];
        struct msghdr msg;
        int msg_flags = 0;
        ssize_t recv_len;

        if (ri_flags & __WASI_RIFLAGS_RECV_PEEK)
            msg_flags |= MSG_PEEK;
        if (ri_flags & __WASI_RIFLAGS_RECV_WAITALL)
            msg_flags |= MSG_WAITALL;

        copy_iov_to_host(runtime, _mem, host_iov, wasi_iovs, iovs_len);

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = host_iov;
        msg.msg_iovlen = (int)iovs_len;

        recv_len = recvmsg(fd, &msg, msg_flags);
        if (recv_len < 0)
            m3ApiReturn(errno_to_wasi(errno));

        m3ApiWriteMem32(ro_datalen, (uint32_t)recv_len);
        m3ApiWriteMem16(ro_flags, (msg.msg_flags & MSG_TRUNC) != 0
                                      ? __WASI_ROFLAGS_RECV_DATA_TRUNCATED
                                      : 0);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_sock_send)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArg      (__wasi_siflags_t     , si_flags)
    m3ApiGetArgMem   (__wasi_size_t *      , so_datalen)

    m3ApiCheckMem(wasi_iovs, iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(so_datalen, sizeof(__wasi_size_t));

    if (si_flags != 0)
        m3ApiReturn(__WASI_ERRNO_INVAL);

    {
        m3_wasi_context_t *context = (m3_wasi_context_t *)(_ctx->userdata);

        if (context && context->socket_ops && context->socket_ops->send) {
            uint64_t total_len = 0;
            uint32_t sent_len = 0;
            uint8_t *tmp = NULL;
            __wasi_errno_t err;

            for (__wasi_size_t i = 0; i < iovs_len; ++i) {
                const uint8_t *addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
                size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);

                if (len != 0)
                    m3ApiCheckMem(addr, len);
                total_len += len;
                if (total_len > UINT32_MAX)
                    m3ApiReturn(__WASI_ERRNO_INVAL);
            }

            if (total_len == 0) {
                m3ApiWriteMem32(so_datalen, 0);
                m3ApiReturn(__WASI_ERRNO_SUCCESS);
            }

            tmp = (uint8_t *)malloc((size_t)total_len);
            if (!tmp)
                m3ApiReturn(__WASI_ERRNO_NOMEM);

            {
                uint8_t *cursor = tmp;
                for (__wasi_size_t i = 0; i < iovs_len; ++i) {
                    const uint8_t *addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
                    uint32_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);

                    if (len != 0)
                        memcpy(cursor, addr, len);
                    cursor += len;
                }
            }

            err = context->socket_ops->send(context->socket_user_data,
                                            fd,
                                            tmp,
                                            (uint32_t)total_len,
                                            si_flags,
                                            &sent_len);
            free(tmp);

            if (err != __WASI_ERRNO_BADF) {
                if (err == __WASI_ERRNO_SUCCESS)
                    m3ApiWriteMem32(so_datalen, sent_len);
                m3ApiReturn(err);
            }
        }
    }

#ifdef _WIN32
    UNUSED(fd);
    m3ApiWriteMem32(so_datalen, 0);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    {
        struct iovec host_iov[iovs_len];
        struct msghdr msg;
        ssize_t sent_len;
        int send_flags = 0;

#ifdef MSG_NOSIGNAL
        send_flags |= MSG_NOSIGNAL;
#endif

        copy_iov_to_host(runtime, _mem, host_iov, wasi_iovs, iovs_len);

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = host_iov;
        msg.msg_iovlen = (int)iovs_len;

        sent_len = sendmsg(fd, &msg, send_flags);
        if (sent_len < 0)
            m3ApiReturn(errno_to_wasi(errno));

        m3ApiWriteMem32(so_datalen, (uint32_t)sent_len);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_sock_shutdown)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_sdflags_t     , how)

    {
        m3_wasi_context_t *context = (m3_wasi_context_t *)(_ctx->userdata);

        if (context && context->socket_ops && context->socket_ops->shutdown) {
            __wasi_errno_t err = context->socket_ops->shutdown(context->socket_user_data,
                                                               fd,
                                                               how);
            if (err != __WASI_ERRNO_BADF)
                m3ApiReturn(err);
        }
    }

#ifdef _WIN32
    UNUSED(fd);
    UNUSED(how);
    m3ApiReturn(__WASI_ERRNO_NOSYS);
#else
    {
        int native_how;

        switch (how) {
        case __WASI_SDFLAGS_RD:
            native_how = SHUT_RD;
            break;
        case __WASI_SDFLAGS_WR:
            native_how = SHUT_WR;
            break;
        case (__WASI_SDFLAGS_RD | __WASI_SDFLAGS_WR):
            native_how = SHUT_RDWR;
            break;
        default:
            m3ApiReturn(__WASI_ERRNO_INVAL);
        }

        if (shutdown(fd, native_how) != 0)
            m3ApiReturn(errno_to_wasi(errno));
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
#endif
}

m3ApiRawFunction(m3_wasi_generic_random_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint8_t *            , buf)
    m3ApiGetArg      (__wasi_size_t        , buf_len)

    m3ApiCheckMem(buf, buf_len);

    while (1) {
        ssize_t retlen = 0;

#if defined(__wasi__) || defined(__APPLE__) || defined(__ANDROID_API__) || defined(__OpenBSD__) || defined(__EMSCRIPTEN__)
        size_t reqlen = M3_MIN (buf_len, 256);
#   if defined(__APPLE__) && (TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR)
        retlen = SecRandomCopyBytes(kSecRandomDefault, reqlen, buf) < 0 ? -1 : reqlen;
#   else
        retlen = getentropy(buf, reqlen) < 0 ? -1 : reqlen;
#   endif
#elif defined(__FreeBSD__) || defined(__linux__)
        retlen = getrandom(buf, buf_len, 0);
#elif defined(_WIN32)
        if (RtlGenRandom(buf, buf_len) == TRUE) {
            m3ApiReturn(__WASI_ERRNO_SUCCESS);
        }
#else
        m3ApiReturn(__WASI_ERRNO_NOSYS);
#endif
        if (retlen < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            m3ApiReturn(errno_to_wasi(errno));
        } else if (retlen == buf_len) {
            m3ApiReturn(__WASI_ERRNO_SUCCESS);
        } else {
            buf     += retlen;
            buf_len -= retlen;
        }
    }
}

m3ApiRawFunction(m3_wasi_generic_clock_res_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArgMem   (__wasi_timestamp_t * , resolution)

    m3ApiCheckMem(resolution, sizeof(__wasi_timestamp_t));

    switch (wasi_clk_id) {
    case __WASI_CLOCKID_REALTIME:
        m3ApiWriteMem64(resolution, 1000000);
        break;
    case __WASI_CLOCKID_MONOTONIC:
        m3ApiWriteMem64(resolution, 1);
        break;
    default: {
        int clk = convert_clockid(wasi_clk_id);
        if (clk < 0) m3ApiReturn(__WASI_ERRNO_INVAL);

        struct timespec tp;
        if (clock_getres(clk, &tp) != 0) {
            m3ApiWriteMem64(resolution, 1000000);
        } else {
            m3ApiWriteMem64(resolution, convert_timespec(&tp));
        }
        break;
    }
    }

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_clock_time_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArg      (__wasi_timestamp_t   , precision)
    m3ApiGetArgMem   (__wasi_timestamp_t * , time)

    m3ApiCheckMem(time, sizeof(__wasi_timestamp_t));
    UNUSED(precision);

    switch (wasi_clk_id) {
    case __WASI_CLOCKID_REALTIME:
        m3ApiWriteMem64(time, turbo_ms_to_ns(turbo_realtime_ms()));
        break;
    case __WASI_CLOCKID_MONOTONIC:
        m3ApiWriteMem64(time, turbo_hrtime());
        break;
    default: {
        int clk = convert_clockid(wasi_clk_id);
        if (clk < 0) m3ApiReturn(__WASI_ERRNO_INVAL);

        struct timespec tp;
        if (clock_gettime(clk, &tp) != 0) {
            m3ApiReturn(errno_to_wasi(errno));
        }

        m3ApiWriteMem64(time, convert_timespec(&tp));
        break;
    }
    }
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_poll_oneoff)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (const __wasi_subscription_t * , in)
    m3ApiGetArgMem   (__wasi_event_t *              , out)
    m3ApiGetArg      (__wasi_size_t                 , nsubscriptions)
    m3ApiGetArgMem   (__wasi_size_t *               , nevents)

    __wasi_size_t count = 0;
    __wasi_timestamp_t earliest_wait = UINT64_MAX;
    bool have_clock = false;

    m3ApiCheckMem(in,       nsubscriptions * sizeof(__wasi_subscription_t));
    m3ApiCheckMem(out,      nsubscriptions * sizeof(__wasi_event_t));
    m3ApiCheckMem(nevents,  sizeof(__wasi_size_t));

    for (__wasi_size_t i = 0; i < nsubscriptions; ++i) {
        const __wasi_subscription_t* sub = &in[i];

        if (sub->type == __WASI_EVENTTYPE_FD_READ ||
            sub->type == __WASI_EVENTTYPE_FD_WRITE) {
            out[count].userdata = sub->userdata;
            out[count].error = __WASI_ERRNO_SUCCESS;
            out[count].type = sub->type;
            out[count].u.fd_readwrite.nbytes =
                estimate_fd_ready_bytes(sub->u.fd_readwrite.file_descriptor, sub->type);
            out[count].u.fd_readwrite.flags = 0;
            count++;
            continue;
        }

        if (sub->type == __WASI_EVENTTYPE_CLOCK) {
            __wasi_timestamp_t now;
            __wasi_timestamp_t wait_ns;
            __wasi_errno_t err;

            have_clock = true;
            err = wasi_clock_now(sub->u.clock.id, &now);
            if (err != __WASI_ERRNO_SUCCESS) {
                out[count].userdata = sub->userdata;
                out[count].error = err;
                out[count].type = sub->type;
                memset(&out[count].u, 0, sizeof(out[count].u));
                count++;
                continue;
            }

            if (sub->u.clock.flags & __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME)
                wait_ns = (sub->u.clock.timeout > now) ? (sub->u.clock.timeout - now) : 0;
            else
                wait_ns = sub->u.clock.timeout;

            if (wait_ns == 0) {
                out[count].userdata = sub->userdata;
                out[count].error = __WASI_ERRNO_SUCCESS;
                out[count].type = sub->type;
                memset(&out[count].u, 0, sizeof(out[count].u));
                count++;
            } else if (wait_ns < earliest_wait) {
                earliest_wait = wait_ns;
            }
            continue;
        }

        out[count].userdata = sub->userdata;
        out[count].error = __WASI_ERRNO_INVAL;
        out[count].type = sub->type;
        memset(&out[count].u, 0, sizeof(out[count].u));
        count++;
    }

    if (count == 0 && have_clock && earliest_wait != UINT64_MAX) {
        __wasi_timestamp_t slept_wait = earliest_wait;
        wasi_sleep_ns(earliest_wait);

        for (__wasi_size_t i = 0; i < nsubscriptions; ++i) {
            const __wasi_subscription_t* sub = &in[i];
            __wasi_timestamp_t now;
            __wasi_errno_t err;

            if (sub->type != __WASI_EVENTTYPE_CLOCK)
                continue;

            err = wasi_clock_now(sub->u.clock.id, &now);
            if (err != __WASI_ERRNO_SUCCESS) {
                out[count].userdata = sub->userdata;
                out[count].error = err;
                out[count].type = sub->type;
                memset(&out[count].u, 0, sizeof(out[count].u));
                count++;
                continue;
            }

            if (((sub->u.clock.flags & __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME) &&
                 sub->u.clock.timeout <= now) ||
                (!(sub->u.clock.flags & __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME) &&
                 sub->u.clock.timeout <= slept_wait)) {
                out[count].userdata = sub->userdata;
                out[count].error = __WASI_ERRNO_SUCCESS;
                out[count].type = sub->type;
                memset(&out[count].u, 0, sizeof(out[count].u));
                count++;
            }
        }
    }

    m3ApiWriteMem32(nevents, count);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_proc_raise)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_signal_t, sig)

    if (raise((int)sig) != 0)
        m3ApiReturn(errno_to_wasi(errno));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_sched_yield)
{
    m3ApiReturnType  (uint32_t)
#ifdef _WIN32
    Sleep(0);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#else
    if (sched_yield() != 0)
        m3ApiReturn(errno_to_wasi(errno));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
#endif
}

m3ApiRawFunction(m3_wasi_generic_proc_exit)
{
    m3ApiGetArg      (uint32_t, code)

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context) {
        context->exit_code = code;
    }

    m3ApiTrap(m3Err_trapExit);
}


static
M3Result SuppressLookupFailure(M3Result i_result)
{
    if (i_result == m3Err_functionLookupFailed)
        return m3Err_none;
    else
        return i_result;
}

m3_wasi_context_t* m3_GetWasiContext()
{
    return s_default_wasi_context;
}

m3_wasi_context_t* m3_GetWasiContextForRuntime(IM3Runtime runtime)
{
    return runtime ? (m3_wasi_context_t*)runtime->wasi_context : NULL;
}

m3_wasi_context_t* m3_GetWasiContextForModule(IM3Module module)
{
    return (module && module->runtime)
        ? (m3_wasi_context_t*)module->runtime->wasi_context
        : NULL;
}

M3Result  m3_LinkWASIWithContext  (IM3Module module, m3_wasi_context_t* wasi_context)
{
    M3Result result = m3Err_none;

    if (!module)
        return m3Err_moduleNotLinked;

    if (!wasi_context)
        return "wasi context is null";

    if (module->runtime) {
        if (module->runtime->wasi_context && module->runtime->wasi_context != wasi_context)
            return "runtime already bound to a different wasi context";

        module->runtime->wasi_context = wasi_context;
    }

#ifdef _WIN32
    setmode(fileno(stdin),  O_BINARY);
    setmode(fileno(stdout), O_BINARY);
    setmode(fileno(stderr), O_BINARY);

#else
    // Preopen dirs
    for (u32 i = 3; i < wasi_context->preopen_count; i++) {
        if (wasi_context->preopen[i].path &&
            wasi_context->preopen[i].real_path &&
            wasi_context->preopen[i].fd < 0) {
            wasi_context->preopen[i].fd =
                turbo_fs_open(wasi_context->preopen[i].real_path,
                              TURBO_FS_O_RDONLY,
                              0);
        }
    }
#endif

    static const char* namespaces[2] = { "wasi_unstable", "wasi_snapshot_preview1" };

    // Some functions are incompatible between WASI versions
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_unstable",          "fd_seek",     "i(iIi*)", &m3_wasi_unstable_fd_seek)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_snapshot_preview1", "fd_seek",     "i(iIi*)", &m3_wasi_snapshot_preview1_fd_seek)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_unstable",          "fd_filestat_get",   "i(i*)",     &m3_wasi_unstable_fd_filestat_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_snapshot_preview1", "fd_filestat_get",   "i(i*)",     &m3_wasi_snapshot_preview1_fd_filestat_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_unstable",          "path_filestat_get", "i(ii*i*)",  &m3_wasi_unstable_path_filestat_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, "wasi_snapshot_preview1", "path_filestat_get", "i(ii*i*)",  &m3_wasi_snapshot_preview1_path_filestat_get)));

    for (int i=0; i<2; i++)
    {
        const char* wasi = namespaces[i];

_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "args_get",           "i(**)",   &m3_wasi_generic_args_get, wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "args_sizes_get",     "i(**)",   &m3_wasi_generic_args_sizes_get, wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "clock_res_get",        "i(i*)",   &m3_wasi_generic_clock_res_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "clock_time_get",       "i(iI*)",  &m3_wasi_generic_clock_time_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "environ_get",          "i(**)",   &m3_wasi_generic_environ_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "environ_sizes_get",    "i(**)",   &m3_wasi_generic_environ_sizes_get)));

_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_advise",            "i(iIIi)", &m3_wasi_generic_fd_advise)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_allocate",          "i(iII)",  &m3_wasi_generic_fd_allocate)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_close",             "i(i)",    &m3_wasi_generic_fd_close)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_datasync",          "i(i)",    &m3_wasi_generic_fd_datasync)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_get",        "i(i*)",   &m3_wasi_generic_fd_fdstat_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_set_flags",  "i(ii)",   &m3_wasi_generic_fd_fdstat_set_flags)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_set_rights", "i(iII)",  &m3_wasi_generic_fd_fdstat_set_rights)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_filestat_set_size", "i(iI)",   &m3_wasi_generic_fd_filestat_set_size)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_filestat_set_times","i(iIIi)", &m3_wasi_generic_fd_filestat_set_times)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_pread",             "i(i*iI*)",&m3_wasi_generic_fd_pread)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_prestat_get",       "i(i*)",   &m3_wasi_generic_fd_prestat_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_prestat_dir_name",  "i(i*i)",  &m3_wasi_generic_fd_prestat_dir_name)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_pwrite",            "i(i*iI*)",&m3_wasi_generic_fd_pwrite)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_read",              "i(i*i*)", &m3_wasi_generic_fd_read)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_readdir",           "i(i*iI*)",&m3_wasi_generic_fd_readdir)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_renumber",          "i(ii)",   &m3_wasi_generic_fd_renumber)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_sync",              "i(i)",    &m3_wasi_generic_fd_sync)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_tell",              "i(i*)",   &m3_wasi_generic_fd_tell)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_write",             "i(i*i*)", &m3_wasi_generic_fd_write)));

_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_create_directory",    "i(i*i)",       &m3_wasi_generic_path_create_directory)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_filestat_set_times",  "i(ii*iIIi)",   &m3_wasi_generic_path_filestat_set_times)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_link",                "i(ii*ii*i)",   &m3_wasi_generic_path_link)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_open",                "i(ii*iiIIi*)", &m3_wasi_generic_path_open)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_readlink",            "i(i*i*i*)",    &m3_wasi_generic_path_readlink)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_remove_directory",    "i(i*i)",       &m3_wasi_generic_path_remove_directory)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_rename",              "i(i*ii*i)",    &m3_wasi_generic_path_rename)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_symlink",             "i(*ii*i)",     &m3_wasi_generic_path_symlink)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_unlink_file",         "i(i*i)",       &m3_wasi_generic_path_unlink_file)));

_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "poll_oneoff",          "i(**i*)", &m3_wasi_generic_poll_oneoff)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "proc_exit",          "v(i)",    &m3_wasi_generic_proc_exit, wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "proc_raise",           "i(i)",    &m3_wasi_generic_proc_raise)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "random_get",           "i(*i)",   &m3_wasi_generic_random_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "sched_yield",          "i()",     &m3_wasi_generic_sched_yield)));

_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "sock_recv",         "i(i*ii**)", &m3_wasi_generic_sock_recv, wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "sock_send",         "i(i*ii*)",  &m3_wasi_generic_sock_send, wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "sock_shutdown",     "i(ii)",     &m3_wasi_generic_sock_shutdown, wasi_context)));
    }

_catch:
    return result;
}

M3Result  m3_LinkWASI  (IM3Module module)
{
    m3_wasi_context_t* wasi_context = m3_GetOrCreateDefaultWasiContext();

    if (!wasi_context)
        return m3Err_mallocFailed;

    return m3_LinkWASIWithContext(module, wasi_context);
}

#endif // d_m3HasWASI
