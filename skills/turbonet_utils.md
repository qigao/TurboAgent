# TurboNet Utils API 参考

## 概述

TurboNet Utils 是 TurboScript 项目的首选工具库集合，提供高性能的内存管理、字符串处理、类型安全格式化、文件 I/O、日志系统、平台工具、并发原语和无锁数据结构。

**位置**：通过 `TURBONET_ROOT` 或项目构建配置定位，例如 `%TURBONET_ROOT%/turbonet/utils/include/`。

**优先级**：在所有依赖中优先级最高，优先于 vendor/、vcpkg、C 标准库和平台 API。

---

## 内存管理

### Slab 分配器 (`turbo_buffer.h`)

**核心类型**：
- `mem_pool_t` - Slab 内存池
- `mem_buffer_t` - 引用计数缓冲区
- `mem_slice_t` - 零拷贝切片

**主要 API**：
```c
// 池管理
int mem_init(mem_pool_t *pool, size_t initial_size);
mem_pool_t *mem_global(void);  // 全局共享池
void mem_destroy(mem_pool_t *pool);
void mem_reset(mem_pool_t *pool);  // 标记所有块为空闲
void mem_trim(mem_pool_t *pool);   // 释放空闲 slab
void mem_free(mem_pool_t *pool, void *ptr);

// 分配
void *mem_alloc(mem_pool_t *pool, size_t size);
char *mem_strdup(mem_pool_t *pool, const char *str);
char *mem_sprintf(mem_pool_t *pool, const char *fmt, ...);

// 缓冲区管理
mem_buffer_t *mem_get_buffer(mem_pool_t *pool, size_t min_size);
void mem_release(mem_buffer_t *buffer);
void mem_ref(mem_buffer_t *buffer);    // 增加引用计数
void mem_unref(mem_buffer_t *buffer);  // 减少引用计数
int mem_is_external(const mem_buffer_t *buffer);

// 零拷贝
mem_buffer_t *mem_wrap_external(void *data, size_t size,
                                void (*free_cb)(void*, void*),
                                void *user_data);
mem_slice_t mem_slice(mem_buffer_t *buffer, size_t offset, size_t length);
void mem_slice_release(mem_slice_t *slice);

// 缓冲区辅助
void mem_set_used(mem_buffer_t *buffer, size_t used);
size_t mem_remaining(const mem_buffer_t *buffer);
char *mem_write_ptr(mem_buffer_t *buffer);

// 宏辅助
#define MEM_ALLOC(pool, type)  // 分配单个对象
#define MEM_ALLOC_ARRAY(pool, type, count)  // 分配数组
```

**常量**：
- `MEM_RECV_BUFFER_SIZE` = 8192
- `MEM_SEND_BUFFER_SIZE` = 8192
- `MEM_HANDSHAKE_BUFFER_SIZE` = 4096
- `MEM_FRAME_BUFFER_SIZE` = 65536
- `MEM_FRAGMENT_BUFFER_SIZE` = 1MB
- `MEM_ARENA_CONTEXT_INIT_SIZE` = 128KB
- `MEM_ARENA_SERVER_INIT_SIZE` = 256KB
- `MEM_ARENA_CLIENT_INIT_SIZE` = 64KB
- `MEM_ARENA_POOL_INIT_SIZE` = 64KB

**使用场景**：
- ✅ 网络缓冲区（零拷贝、引用计数）
- ✅ 解析器临时分配（批量分配 + `mem_reset()`）
- ✅ 跨模块传递数据（`mem_ref`/`mem_unref` 管理生命周期）
- ❌ 替代全局 `malloc`（应使用专用池）

**示例**：
```c
mem_pool_t pool;
mem_init(&pool, 0);

// 临时分配
char *str = mem_strdup(&pool, "hello");
int *arr = MEM_ALLOC_ARRAY(&pool, int, 100);

// 零拷贝缓冲区
mem_buffer_t *buf = mem_get_buffer(&pool, 4096);
memcpy(mem_write_ptr(buf), data, len);
mem_set_used(buf, len);
mem_slice_t slice = mem_slice(buf, 10, 50);  // 引用 [10, 60)
// ... 使用 slice.data
mem_slice_release(&slice);  // 自动 unref buffer

// 批量重置
mem_reset(&pool);  // 所有分配失效，可重用内存
mem_destroy(&pool);
```

---

### 对象池 (`object_pool.h`)

**核心类型**：
- `object_pool_t` - 固定大小对象池（Free-list 实现）
- `object_pool_config_t` - 池配置

**主要 API**：
```c
object_pool_t *object_pool_create(const object_pool_config_t *config);
void object_pool_destroy(object_pool_t *pool);
void *object_pool_alloc(object_pool_t *pool);   // O(1)
void object_pool_free(object_pool_t *pool, void *obj);  // O(1)

// 统计
size_t object_pool_allocated_count(const object_pool_t *pool);
size_t object_pool_free_count(const object_pool_t *pool);
size_t object_pool_peak_usage(const object_pool_t *pool);
```

**配置**：
```c
object_pool_config_t config = {
    .object_size = sizeof(my_node_t),
    .initial_capacity = 1024,
    .max_capacity = 0,  // 无限制
    .zero_on_alloc = false
};
```

**性能**：
- **速度**：100M+ ops/s（比 malloc 快 10-100 倍）
- **内存**：连续分配，缓存友好

**使用场景**：
- ✅ AST 节点（固定大小，频繁创建/销毁）
- ✅ Token 对象（解析过程中高频分配）
- ✅ 连接对象（网络服务器）
- ❌ 可变大小对象（用 `mem_pool_t`）
- ❌ 长生命周期对象（直接 malloc）

**示例**：
```c
// AST 节点池
object_pool_config_t cfg = {
    .object_size = sizeof(ast_node_t),
    .initial_capacity = 10000,
    .max_capacity = 100000
};
object_pool_t *pool = object_pool_create(&cfg);

ast_node_t *node = object_pool_alloc(pool);
// 使用 node...
object_pool_free(pool, node);  // 返回池中复用

object_pool_destroy(pool);
```

---

### Arena 分配器 (`memory_pool.h`)

**核心类型**：
- `MemoryPool` - 栈式内存池

**主要 API**：
```c
MemoryPool *pool_create(size_t size);
void *pool_alloc(MemoryPool *pool, size_t size);
void pool_reset(MemoryPool *pool);  // 重置，可重用
void pool_destroy(MemoryPool *pool);

// Mark/Rewind（栈式分配）
size_t pool_mark(MemoryPool *pool);
void pool_rewind(MemoryPool *pool, size_t mark);

// 统计
size_t pool_get_used(MemoryPool *pool);
size_t pool_get_available(MemoryPool *pool);
size_t pool_get_peak(MemoryPool *pool);
```

**使用场景**：
- ✅ 请求级分配（处理完重置）
- ✅ 事务级临时内存（统一生命周期）
- ✅ 解析中间结果（mark/rewind 回滚）
- ❌ 跨请求共享数据

**示例**：
```c
MemoryPool *pool = pool_create(64 * 1024);  // 64KB

// 请求处理
size_t mark = pool_mark(pool);
char *buf = pool_alloc(pool, 1024);
// ... 处理请求
pool_rewind(pool, mark);  // 回滚到 mark 点

// 批量处理多个请求后重置
pool_reset(pool);
pool_destroy(pool);
```

---

## 字符串处理

### 动态字符串 (`turbo_str.h`)

**核心类型**：
- `tstr_t` = `char*`（可直接用于 `printf("%s", s)`）
- O(1) 长度查询、二进制安全

**创建/销毁**：
```c
tstr_t tstr_new(void);
tstr_t tstr_dup(const char *s);
tstr_t tstr_dup_len(const char *s, size_t n);
tstr_t tstr_new_len(const void *init, size_t n);
tstr_t tstr_from_v(tstr_v v);
tstr_t tstr_from_ll(long long value);
void tstr_free(tstr_t s);
```

**属性**：
```c
size_t tstr_len(tstr_t s);        // O(1)
size_t tstr_avail(tstr_t s);      // 可用空间
int tstr_empty(tstr_t s);
void tstr_set_len(tstr_t s, size_t n);  // 手动设置长度
void tstr_clear(tstr_t s);        // 清空但保留内存
```


**拼接**（必须重新赋值）：
```c
tstr_t tstr_cat(tstr_t s, const char *t);
tstr_t tstr_cat_len(tstr_t s, const char *t, size_t n);
tstr_t tstr_cat_str(tstr_t s, tstr_t t);
tstr_t tstr_cat_v(tstr_t s, tstr_v v);  // 拼接 view
tstr_t tstr_cat_fmt(tstr_t s, const char *fmt, ...);
tstr_t tstr_cat_typed(tstr_t s, const char *fmt, ...); // 需 fmt.h，{} 语法
```

**复制**：
```c
tstr_t tstr_cpy(tstr_t s, const char *t);
tstr_t tstr_cpy_len(tstr_t s, const char *t, size_t n);
tstr_t tstr_cpy_v(tstr_t s, tstr_v v);
```

**比较/查找**：
```c
int tstr_cmp(tstr_t s1, tstr_t s2);
int tstr_cmp_v(tstr_t s, tstr_v v);
int tstr_casecmp(const char *s1, const char *s2);
int tstr_ncasecmp(const char *s1, const char *s2, size_t n);
int tstr_eq_v(tstr_t s, tstr_v v);
int tstr_ieq_v(tstr_t s, tstr_v v);
int tstr_starts_with(const char *s, const char *prefix);
int tstr_starts_with_v(tstr_t s, tstr_v prefix);
int tstr_istarts_with(const char *s, const char *prefix);
int tstr_ends_with(const char *s, const char *suffix);
int tstr_ends_with_v(tstr_t s, tstr_v suffix);
int tstr_contains(const char *s, const char *substr);
int tstr_contains_v(tstr_t s, tstr_v needle);

size_t tstr_find_v(tstr_t s, tstr_v needle);      // 位置或 TSTR_V_NPOS
size_t tstr_find_char(tstr_t s, char c);
size_t tstr_rfind_v(tstr_t s, tstr_v needle);     // 反向查找
size_t tstr_rfind_char(tstr_t s, char c);
```

**转换**：
```c
void tstr_lower(tstr_t s);  // 小写
void tstr_upper(tstr_t s);  // 大写
tstr_t tstr_trim(tstr_t s, const char *cset);

// 与 view 互转
tstr_t tstr_from_v(tstr_v v);       // 拷贝
tstr_v tstr_to_v(tstr_t s);         // 零拷贝引用
char *tstr_to_cstr(tstr_t s);       // malloc 拷贝，需 free()
tstr_t tstr_reserve(tstr_t s, size_t addlen);
tstr_t tstr_shrink(tstr_t s);
```

**分割/连接**：
```c
tstr_t *tstr_split(tstr_t s, const char *sep, int *count);
void tstr_free_split(tstr_t *tokens, int count);
tstr_t tstr_join(char **argv, int argc, const char *sep);
```

**使用场景**：
- ✅ 配置文件解析
- ✅ SQL/命令构建
- ✅ 日志消息拼接
- ✅ 路径拼接
- ❌ 固定字符串（用 `const char*` 或 `tstr_v`）

**示例**：
```c
tstr_t path = tstr_new();
path = tstr_cat(path, "/usr/local");
path = tstr_cat(path, "/bin");
path = tstr_cat_fmt(path, "/%s", filename);
path = tstr_cat_typed(path, "?id={}", 42);
printf("Path: %s\n", path);  // 直接打印
tstr_free(path);
```

---

### String View (`turbo_str_view.h`)

**核心类型**：
- `tstr_v` - 零拷贝不可变字符串视图

**主要 API**：
```c
// 创建
tstr_v tstr_v_from_cstr(const char *s);
tstr_v tstr_v_from_buf(const char *s, size_t len);
tstr_v tstr_v_from_slice(const mem_slice_t *slice);

// 属性
size_t tstr_v_len(tstr_v v);
int tstr_v_empty(tstr_v v);

// 比较
int tstr_v_eq(tstr_v a, tstr_v b);
int tstr_v_ieq(tstr_v a, tstr_v b);
int tstr_v_starts_with(tstr_v v, tstr_v prefix);
int tstr_v_ends_with(tstr_v v, tstr_v suffix);
int tstr_v_contains(tstr_v v, tstr_v needle);

// 查找与切片
size_t tstr_v_find(tstr_v v, tstr_v needle);
size_t tstr_v_rfind(tstr_v v, tstr_v needle);
size_t tstr_v_find_char(tstr_v v, char c);
size_t tstr_v_rfind_char(tstr_v v, char c);
tstr_v tstr_v_sub(tstr_v v, size_t pos, size_t len);
tstr_v tstr_v_trim(tstr_v v, const char *cset);
tstr_v tstr_v_trim_left(tstr_v v, const char *cset);
tstr_v tstr_v_trim_right(tstr_v v, const char *cset);
tstr_v tstr_v_split_next(tstr_v *rest, tstr_v delim);

// 需要拷贝的转换
char *tstr_v_to_cstr(tstr_v v);
char *tstr_v_to_pool(tstr_v v, MemoryPool *pool);
char *tstr_v_to_arena(tstr_v v, mem_pool_t *arena);
```

**使用场景**：
- ✅ 解析中间结果（不拷贝原始数据）
- ✅ 函数参数（只读引用）
- ✅ 临时子串操作
- ❌ 需要修改内容（用 `tstr_t`）
- ❌ 跨函数保存（原始数据可能失效）

**示例**：
```c
const char *text = "key=value";
tstr_v v = tstr_v_from_cstr(text);
size_t pos = tstr_v_find_char(v, '=');
tstr_v key = tstr_v_sub(v, 0, pos);
tstr_v val = tstr_v_sub(v, pos + 1, tstr_v_len(v) - pos - 1);
// key 和 val 都是零拷贝引用 text
```

---

## 文件系统

### 跨平台文件 I/O (`turbo_fs.h`)

**核心类型**：
- `turbo_fs_buf_t` - 文件缓冲区
- `turbo_fs_stat_t` - 文件信息
- `turbo_file_t` - 文件句柄

**同步操作（整文件）**：
```c
int turbo_fs_read_file(const char *path, turbo_fs_buf_t *buf);
int turbo_fs_write_file(const char *path, const turbo_fs_buf_t *buf);
int turbo_fs_stat(const char *path, turbo_fs_stat_t *stat);
int turbo_fs_mkdir(const char *path, int mode);
int turbo_fs_rmdir(const char *path);
int turbo_fs_unlink(const char *path);
int turbo_fs_rename(const char *old_path, const char *new_path);
```

**流式操作**：
```c
turbo_file_t turbo_fs_open(const char *path, int flags, int mode);
int turbo_fs_read(turbo_file_t fd, char *buf, size_t len);
int turbo_fs_pread(turbo_file_t fd, char *buf, size_t len, int64_t offset);  // 线程安全
int turbo_fs_write(turbo_file_t fd, const char *data, size_t len);
int turbo_fs_pwrite(turbo_file_t fd, const char *data, size_t len, int64_t offset);
int turbo_fs_close(turbo_file_t fd);
int turbo_fs_fsync(turbo_file_t fd);
int turbo_fs_ftruncate(turbo_file_t fd, int64_t length);
int64_t turbo_fs_seek(turbo_file_t fd, int64_t offset, int whence);
int64_t turbo_fs_tell(turbo_file_t fd);
```


**路径操作**：
```c
int turbo_fs_path_join(char *result, size_t result_size, 
                       const char *base, const char *path);
int turbo_fs_path_dirname(const char *path, char *dirname, size_t dirname_size);
int turbo_fs_path_basename(const char *path, char *basename, size_t basename_size);
bool turbo_fs_path_is_absolute(const char *path);
int turbo_fs_get_tmpdir(char *buffer, size_t buffer_size);
```

**常量**：
```c
#define TURBO_FS_O_RDONLY  0x0001
#define TURBO_FS_O_WRONLY  0x0002
#define TURBO_FS_O_RDWR    0x0004
#define TURBO_FS_O_CREAT   0x0100
#define TURBO_FS_O_TRUNC   0x0200
#define TURBO_FS_O_APPEND  0x0400
#define TURBO_FS_MAX_PATH  260
```

**使用场景**：
- ✅ 加载脚本/配置文件（`turbo_fs_read_file`）
- ✅ 写入日志/结果（`turbo_fs_write_file`）
- ✅ 大文件流式处理（`turbo_fs_open` + `turbo_fs_read`）
- ✅ 多线程并发读取（`turbo_fs_pread` 不改变文件位置）
- ❌ 直接使用裸标准 I/O 或底层系统文件 API（失去跨平台性）

**示例**：
```c
// 读取整个文件
turbo_fs_buf_t buf;
if (turbo_fs_read_file("config.json", &buf) == 0) {
    // buf.base 包含文件内容，buf.len 是大小
    parse_json(buf.base, buf.len);
    turbo_fs_buf_free(&buf);
}

// 流式读取大文件
turbo_file_t fd = turbo_fs_open("data.txt", TURBO_FS_O_RDONLY, 0);
char buffer[4096];
int n;
while ((n = turbo_fs_read(fd, buffer, sizeof(buffer))) > 0) {
    process(buffer, n);
}
turbo_fs_close(fd);

// 多线程并发读取（线程安全）
int n = turbo_fs_pread(fd, buf, len, offset);  // 不改变 fd 位置
```

---

### 内存映射文件 (`turbo_mmap.h`)

**核心类型**：
- `turbo_mmap_t` - 内存映射句柄
- `turbo_mmap_group_t` - 多文件组映射

**主要 API**：
```c
void turbo_mmap_init(turbo_mmap_t *mmap);
int turbo_mmap_open(turbo_mmap_t *mmap, const char *path, int access);
int turbo_mmap_open_range(turbo_mmap_t *mmap, const char *path,
                          int64_t offset, size_t length, int access);
int turbo_mmap_from_fd(turbo_mmap_t *mmap, intptr_t fd,
                       int64_t offset, size_t length, int access);
int turbo_mmap_sync(turbo_mmap_t *mmap, bool async);
int turbo_mmap_sync_range(turbo_mmap_t *mmap, size_t offset, 
                          size_t length, bool async);
void turbo_mmap_unmap(turbo_mmap_t *mmap);
void turbo_mmap_close(turbo_mmap_t *mmap);

// 访问器
void *turbo_mmap_data(const turbo_mmap_t *mmap);
size_t turbo_mmap_size(const turbo_mmap_t *mmap);
bool turbo_mmap_is_open(const turbo_mmap_t *mmap);
uint8_t turbo_mmap_get(const turbo_mmap_t *mmap, size_t offset);
void turbo_mmap_set(turbo_mmap_t *mmap, size_t offset, uint8_t value);

// 优化
int turbo_mmap_advise(turbo_mmap_t *mmap, turbo_mmap_advice_t advice);
int turbo_mmap_lock(turbo_mmap_t *mmap);    // 锁定到物理内存
int turbo_mmap_unlock(turbo_mmap_t *mmap);
size_t turbo_mmap_page_size(void);
size_t turbo_mmap_pages(const turbo_mmap_t *mmap);
```

**访问模式**：
```c
TURBO_MMAP_READ    // 只读
TURBO_MMAP_WRITE   // 读写
TURBO_MMAP_EXEC    // 可执行
```

**优化提示**：
```c
TURBO_MMAP_NORMAL     // 无特殊处理
TURBO_MMAP_SEQUENTIAL // 顺序访问
TURBO_MMAP_RANDOM     // 随机访问
TURBO_MMAP_WILLNEED   // 即将访问
TURBO_MMAP_DONTNEED   // 不再需要
```

**组映射**：
```c
void turbo_mmap_group_init(turbo_mmap_group_t *group);
int turbo_mmap_group_open(turbo_mmap_group_t *group, 
                          const char **paths, size_t count, int access);
void turbo_mmap_group_close(turbo_mmap_group_t *group);
```

**使用场景**：
- ✅ 大文件零拷贝读取（日志分析）
- ✅ 数据库页面映射
- ✅ 共享内存（进程间通信）
- ✅ 多文件顺序访问（日志段、数据分片）
- ❌ 小文件（<4KB，开销大于收益）
- ❌ 需要频繁 sync（性能差）

**示例**：
```c
// 映射整个文件
turbo_mmap_t mmap;
turbo_mmap_init(&mmap);
if (turbo_mmap_open(&mmap, "data.bin", TURBO_MMAP_READ) == 0) {
    const uint8_t *data = turbo_mmap_data(&mmap);
    size_t size = turbo_mmap_size(&mmap);
    // 直接访问 data[0..size-1]，零拷贝
    process(data, size);
    turbo_mmap_close(&mmap);
}

// 多文件组映射（日志段）
const char *logs[] = {"log.1", "log.2", "log.3"};
turbo_mmap_group_t group;
turbo_mmap_group_init(&group);
turbo_mmap_group_open(&group, logs, 3, TURBO_MMAP_READ);
// group.data 指向连续虚拟内存，包含所有文件
const char *all_data = group.data;
size_t total = group.total_size;
turbo_mmap_group_close(&group);
```

---

## 日志系统

### 高性能异步日志 (`tlog.h`)

**核心类型**：
- `tlog_t` - 日志器句柄
- `turbo_log_sink_t` - Sink 接口（console/file/callback）
- `turbo_log_entry_t` - 日志条目

**日志级别**：
```c
TURBO_LOG_LEVEL_DEBUG
TURBO_LOG_LEVEL_INFO
TURBO_LOG_LEVEL_WARN
TURBO_LOG_LEVEL_ERROR
TURBO_LOG_LEVEL_FATAL

const char *turbo_log_level_name(turbo_log_level_t level);
turbo_log_level_t turbo_log_level_from_name(const char *name);
```

**Logger 管理**：
```c
tlog_t *tlog_create(const tlog_config_t *config);
void tlog_destroy(tlog_t *logger);
int tlog_add_sink(tlog_t *logger, turbo_log_sink_t *sink);
void tlog_remove_sink(tlog_t *logger, turbo_log_sink_t *sink);
void tlog_flush(tlog_t *logger);
void tlog_set_level(tlog_t *logger, turbo_log_level_t level);
turbo_log_level_t tlog_get_level(const tlog_t *logger);

// 全局默认 logger
void tlog_set_default(tlog_t *logger);
tlog_t *tlog_get_default(void);
```

**Sink 创建**：
```c
turbo_log_sink_t *turbo_sink_console_create(const turbo_console_sink_opts_t *opts);
turbo_log_sink_t *turbo_sink_file_create(const turbo_file_sink_opts_t *opts);
turbo_log_sink_t *turbo_sink_callback_create(turbo_log_callback_fn callback, void *user_data);
void turbo_sink_destroy(turbo_log_sink_t *sink);
```

**Console Sink 配置**：
```c
turbo_console_sink_opts_t opts = {
    .output = stdout,  // 或 stderr
    .use_colors = 1,
    .pattern = "[{time}] [{level}] {message}"
};
```

**File Sink 配置**：
```c
turbo_file_sink_opts_t opts = {
    .path = "app.log",
    .max_size = 100 * 1024 * 1024,  // 100MB
    .max_files = 10,
    .append = 1,
    .pattern = "[{time}] [{level}] [{component}] {message}"
};
```

**日志宏（推荐使用）**：
```c
// 使用默认 logger
TLOG_DEBUG("value = {}", value);
TLOG_INFO("started on port {}", port);
TLOG_WARN("retry {} failed", count);
TLOG_ERROR("failed to open {}: {}", path, err);
TLOG_FATAL("critical error: {}", msg);

// 指定 logger 和 component
TURBO_LOG_DEBUG(logger, "Parser", "token = {}", token);
TURBO_LOG_INFO(logger, "Network", "connected to {}", addr);
```


**格式化占位符**：
```
{time}      - 时间戳（YYYY-MM-DD HH:MM:SS）
{time_ms}   - 时间戳（含毫秒）
{level}     - 日志级别
{component} - 组件名
{file}      - 源文件
{line}      - 行号
{thread}    - 线程 ID
{message}   - 日志消息
```

**性能**：
- **吞吐量**：~9M ops/s（文件日志，4 线程）
- **延迟**：低于 100ns（异步模式）
- **实现**：Lock-free ring buffer + pwrite（无锁写入）

**统计**：
```c
uint64_t tlog_get_written(const tlog_t *logger);
uint64_t tlog_get_dropped(const tlog_t *logger);  // 背压丢失
int tlog_get_queue_size(const tlog_t *logger);
```

**使用场景**：
- ✅ 所有生产日志需求
- ✅ 高并发服务（多线程安全）
- ✅ 结构化日志（类型安全格式化）
- ❌ 临时调试（用 `printf` 即可）

**示例**：
```c
// 初始化
tlog_config_t cfg = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .buffer_size = 64 * 1024,
    .pool_size = 32 * 1024
};
tlog_t *logger = tlog_create(&cfg);

// 添加 console sink
turbo_console_sink_opts_t console_opts = {
    .output = stdout,
    .use_colors = 1,
    .pattern = TURBO_LOG_DEFAULT_PATTERN
};
tlog_add_sink(logger, turbo_sink_console_create(&console_opts));

// 添加 file sink（自动滚动）
turbo_file_sink_opts_t file_opts = {
    .path = "app.log",
    .max_size = 100 * 1024 * 1024,  // 100MB
    .max_files = 10,
    .append = 1,
    .pattern = "[{time_ms}] [{level}] {message}"
};
tlog_add_sink(logger, turbo_sink_file_create(&file_opts));

// 设为默认
tlog_set_default(logger);

// 使用
TLOG_INFO("Server started on port {}", port);
TLOG_ERROR("Failed to connect: {}", strerror(errno));

// 关闭前刷新
tlog_flush(logger);
tlog_destroy(logger);
```

---

## 平台基础设施

### 时间、系统信息与定时器 (`platform.h`)

**时间 API**：
```c
uint64_t turbo_monotonic_ms(void);  // 单调时钟，适合测量间隔
uint64_t turbo_realtime_ms(void);   // 墙钟时间，Unix 纪元毫秒
uint64_t turbo_hrtime(void);        // 高精度纳秒
uint64_t turbo_uptime_ms(void);     // 进程运行时间

int turbo_gettimeofday(turbo_timeval_t *tv, turbo_timezone_t *tz);
int turbo_gmtime(time_t t, struct tm *out);
int turbo_localtime(time_t t, struct tm *out);
time_t turbo_timegm(const struct tm *tm_value);
time_t turbo_mktime(struct tm *tm_value);
int turbo_strftime_utc(time_t t, const char *format, char *buffer, size_t buffer_size);
int turbo_strftime_local(time_t t, const char *format, char *buffer, size_t buffer_size);

uint64_t turbo_ns_to_ms(uint64_t ns);
uint64_t turbo_ms_to_ns(uint64_t ms);
```

**平台信息**：
```c
int turbo_platform_os_name(char *buffer, size_t buffer_size);
int turbo_platform_os_version(char *buffer, size_t buffer_size);
int turbo_platform_arch(char *buffer, size_t buffer_size);
int turbo_platform_username(char *buffer, size_t buffer_size);
int turbo_platform_hostname(char *buffer, size_t buffer_size);

int turbo_platform_cpu_info(turbo_platform_cpu_info_t *info);
int turbo_platform_memory_info(turbo_platform_memory_info_t *info);
int turbo_platform_load_average(turbo_platform_load_average_t *info);
int turbo_platform_network_interfaces(turbo_platform_network_interface_t *interfaces,
                                      size_t max_interfaces, size_t *count);
```

**定时器**：
```c
typedef struct turbo_native_timer_s turbo_timer_t;
typedef void (*turbo_timer_cb)(turbo_timer_t *timer);

turbo_timer_t *turbo_timer_create(void *loop);  // loop 参数仅保留兼容性
int turbo_timer_start(turbo_timer_t *timer, turbo_timer_cb cb,
                      uint64_t timeout, uint64_t repeat);
int turbo_timer_stop(turbo_timer_t *timer);
void turbo_timer_destroy(turbo_timer_t *timer);
void turbo_timer_set_data(turbo_timer_t *timer, void *data);
void *turbo_timer_get_data(turbo_timer_t *timer);
uint64_t turbo_timer_get_repeat(turbo_timer_t *timer);
```

**使用原则**：
- ✅ 计时、超时、日志时间戳、平台信息查询统一走 `platform.h`
- ✅ 定时器回调必须线程安全，不依赖调用线程上下文
- ❌ 不在业务代码中直接使用底层平台时间/系统信息 API

---

## 并发与线程

### 线程原语 (`turbo_thread.h`)

**核心类型**：
- `turbo_mutex_t` - 互斥锁
- `turbo_rwlock_t` - 读写锁
- `turbo_cond_t` - 条件变量
- `turbo_thread_t` - 线程句柄
- `turbo_once_t` - 一次性初始化

**互斥锁**：
```c
void turbo_mutex_init(turbo_mutex_t *mutex);
void turbo_mutex_destroy(turbo_mutex_t *mutex);
void turbo_mutex_lock(turbo_mutex_t *mutex);
void turbo_mutex_unlock(turbo_mutex_t *mutex);
```

**读写锁**：
```c
int turbo_rwlock_init(turbo_rwlock_t *lock);
void turbo_rwlock_destroy(turbo_rwlock_t *lock);
void turbo_rwlock_rdlock(turbo_rwlock_t *lock);     // 共享读锁
void turbo_rwlock_rdunlock(turbo_rwlock_t *lock);
void turbo_rwlock_wrlock(turbo_rwlock_t *lock);     // 独占写锁
void turbo_rwlock_wrunlock(turbo_rwlock_t *lock);
```

**条件变量**：
```c
void turbo_cond_init(turbo_cond_t *cond);
void turbo_cond_destroy(turbo_cond_t *cond);
void turbo_cond_signal(turbo_cond_t *cond);
void turbo_cond_broadcast(turbo_cond_t *cond);
void turbo_cond_wait(turbo_cond_t *cond, turbo_mutex_t *mutex);
int turbo_cond_timedwait(turbo_cond_t *cond, turbo_mutex_t *mutex, uint64_t timeout_ns);
```

**线程**：
```c
typedef void (*turbo_thread_cb)(void *arg);
int turbo_thread_create(turbo_thread_t *thread, turbo_thread_cb entry, void *arg);
int turbo_thread_join(turbo_thread_t *thread);
void turbo_thread_destroy(turbo_thread_t *thread);
void turbo_thread_yield(void);
void turbo_sleep_ms(uint32_t ms);
```

**一次性初始化**：
```c
turbo_once_t guard = TURBO_ONCE_INIT;
void turbo_once(turbo_once_t *guard, void (*callback)(void));
```

**线程局部存储**：
```c
static TURBO_THREAD_LOCAL int tls_var;
```

**单线程优化**：
```c
void turbo_sync_set_single_threaded(int enabled);  // 禁用内部锁
int turbo_sync_is_single_threaded(void);
```

**使用场景**：
- ✅ 跨平台线程封装（由 TurboNet util 适配平台差异）
- ✅ 配置/缓存访问保护（读写锁）
- ✅ 生产者-消费者模式（条件变量）
- ❌ 高性能场景（用无锁数据结构）

**示例**：
```c
// 互斥锁保护共享数据
turbo_mutex_t lock;
turbo_mutex_init(&lock);
turbo_mutex_lock(&lock);
// 访问共享数据
turbo_mutex_unlock(&lock);
turbo_mutex_destroy(&lock);

// 读写锁（多读少写）
turbo_rwlock_t rwlock;
turbo_rwlock_init(&rwlock);
turbo_rwlock_rdlock(&rwlock);
int val = shared_config.value;  // 读取
turbo_rwlock_rdunlock(&rwlock);

// 一次性初始化
static turbo_once_t init_guard = TURBO_ONCE_INIT;
void init_func(void) { /* 初始化代码 */ }
turbo_once(&init_guard, init_func);  // 线程安全，只执行一次
```

---

### 线程池 (`turbo_thread.h`)

**核心类型**：
- `turbo_threadpool_t` - 线程池句柄
- `turbo_task_fn` - 任务回调

**主要 API**：
```c
turbo_threadpool_t *turbo_threadpool_create(int num_threads);  // 0 = auto
turbo_threadpool_t *turbo_threadpool_create_with_config(const turbo_threadpool_config_t *config);
void turbo_threadpool_destroy(turbo_threadpool_t *pool);

int turbo_threadpool_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg);
int turbo_threadpool_try_submit(turbo_threadpool_t *pool, turbo_task_fn task, void *arg);
void turbo_threadpool_wait(turbo_threadpool_t *pool);
void turbo_threadpool_shutdown(turbo_threadpool_t *pool);

// 查询
int turbo_threadpool_pending(turbo_threadpool_t *pool);
int turbo_threadpool_size(turbo_threadpool_t *pool);
size_t turbo_threadpool_capacity(turbo_threadpool_t *pool);
int turbo_threadpool_is_accepting(turbo_threadpool_t *pool);
void turbo_threadpool_get_stats(turbo_threadpool_t *pool, turbo_threadpool_stats_t *stats);
```

**配置**：
```c
turbo_threadpool_config_t config = {
    .num_threads = 4,
    .queue_capacity = 1024
};
```

**统计**：
```c
turbo_threadpool_stats_t stats;
turbo_threadpool_get_stats(pool, &stats);
// stats.submitted_tasks, stats.completed_tasks, stats.active_tasks 等
```

**使用场景**：
- ✅ CPU 密集型并行任务
- ✅ I/O 密集型批量操作
- ✅ 异步任务队列
- ❌ 低延迟需求（用专用线程）

**示例**：
```c
// 创建线程池（自动检测核心数）
turbo_threadpool_t *pool = turbo_threadpool_create(0);

// 提交任务
void process_item(void *arg) {
    int *item = (int*)arg;
    // 处理任务...
    free(item);
}

for (int i = 0; i < 100; i++) {
    int *item = malloc(sizeof(int));
    *item = i;
    turbo_threadpool_submit(pool, process_item, item);
}

// 等待所有任务完成
turbo_threadpool_wait(pool);

// 关闭
turbo_threadpool_shutdown(pool);
turbo_threadpool_destroy(pool);
```

---

## 无锁数据结构

### Disruptor (`disruptor.h`)

**核心类型**：
- `disruptor_t` - MPMC 无锁队列（LMAX Disruptor 算法）
- `disruptor_cursor_t` - 序列号游标
- `disruptor_consumer_t` - 消费者句柄

**主要 API**：
```c
disruptor_t *disruptor_create(const disruptor_config_t *config);
void disruptor_destroy(disruptor_t *disruptor);

// 生产者
int disruptor_publisher_try_claim(disruptor_t *d, disruptor_cursor_t *cursor);
void disruptor_publisher_next_entry_blocking(disruptor_t *d, disruptor_cursor_t *cursor);
void *disruptor_publisher_next_entry_and_acquire_blocking(disruptor_t *d, disruptor_cursor_t *cursor);
int disruptor_publisher_publish(disruptor_t *d, const disruptor_cursor_t *cursor);

// 消费者
uint64_t disruptor_consumer_register(disruptor_t *d, disruptor_consumer_t *consumer);
void disruptor_consumer_unregister(disruptor_t *d, const disruptor_consumer_t *consumer);
void disruptor_consumer_wait_for_blocking(const disruptor_t *d, disruptor_cursor_t *cursor);
void disruptor_consumer_release_entry(disruptor_t *d, const disruptor_consumer_t *consumer,
                                      const disruptor_cursor_t *cursor);

// 通用消费者循环
void disruptor_consumer_run(disruptor_t *disruptor, disruptor_consumer_t *consumer,
                            disruptor_should_run_fn should_run,
                            disruptor_batch_fn process_batch, void *ctx);
```

**配置**：
```c
disruptor_config_t config = {
    .entry_size = sizeof(event_t),
    .capacity = 1024,  // 必须是 2 的幂
    .consumer_capacity = 16
};
```

**性能**：
- **吞吐量**：数百万 ops/s（多线程）
- **延迟**：微秒级
- **内存**：预分配，零动态分配

**使用场景**：
- ✅ 高频事件总线（日志传输、事件流）
- ✅ MPMC 任务队列（生产者 > 1，消费者 > 1）
- ✅ 低延迟消息传递
- ❌ 低频场景（用带锁队列更简单）
- ❌ 动态大小消息（固定 entry_size）

**示例**：
```c
typedef struct { int id; char data[64]; } event_t;

disruptor_config_t cfg = {
    .entry_size = sizeof(event_t),
    .capacity = 1024,
    .consumer_capacity = 4
};
disruptor_t *d = disruptor_create(&cfg);

// 生产者线程
void producer(void *arg) {
    for (int i = 0; i < 10000; i++) {
        disruptor_cursor_t cursor;
        event_t *e = disruptor_publisher_next_entry_and_acquire_blocking(d, &cursor);
        e->id = i;
        snprintf(e->data, sizeof(e->data), "event-%d", i);
        disruptor_publisher_publish(d, &cursor);
    }
}

// 消费者回调
int should_run(void *ctx) { return *(int*)ctx; }
void process_batch(void *ctx, uint64_t first, uint64_t last) {
    for (uint64_t seq = first; seq <= last; seq++) {
        disruptor_cursor_t cursor = {.sequence = seq};
        const event_t *e = disruptor_show_entry(d, &cursor);
        printf("Process: %d\n", e->id);
    }
}

// 消费者线程
void consumer(void *arg) {
    disruptor_consumer_t cons;
    int running = 1;
    disruptor_consumer_run(d, &cons, should_run, process_batch, &running);
}
```

---

### SPSC 环形缓冲区 (`ring_buffer.h`, `ring_buffer_spsc.h`)

**核心类型**：
- `ring_data_type` - 单线程环形缓冲区
- SPSC 版本在 `ring_buffer_spsc.h`（原子操作，单生产者单消费者）

**主要 API**：
```c
void ring_init(ring_data_type *inst, uint8_t *data_array, size_t size);

// 写入
uint8_t *ring_write_acquire(ring_data_type *inst, size_t free_required);
void ring_write_release(ring_data_type *inst, size_t written);

// 读取
uint8_t *ring_read_acquire(ring_data_type *inst, size_t *available);
void ring_read_release(ring_data_type *inst, size_t read);
```

**线程安全**：
- `ring_buffer.h` - **单线程**，无原子操作
- `ring_buffer_spsc.h` - **SPSC**，原子操作

**使用场景**：
- ✅ 单生产者单消费者（SPSC 版本）
- ✅ 网络缓冲区
- ✅ 音视频流
- ❌ 多生产者或多消费者（用 Disruptor）

**示例**：
```c
uint8_t buffer[4096];
ring_data_type ring;
ring_init(&ring, buffer, sizeof(buffer));

// 写入
uint8_t *wr = ring_write_acquire(&ring, 128);
if (wr) {
    memcpy(wr, data, 128);
    ring_write_release(&ring, 128);
}

// 读取
size_t avail;
uint8_t *rd = ring_read_acquire(&ring, &avail);
if (rd && avail > 0) {
    process(rd, avail);
    ring_read_release(&ring, avail);
}
```

---

### 桶式优先队列 (`bucket_priority_queue.h`)

**核心类型**：
- `bucket_priority_queue_t` - 4 优先级队列
- SPSC/MPMC 变体：`bucket_priority_queue_spsc.h`, `bucket_priority_queue_mpmc.h`

**优先级**：
```c
BUCKET_PRIORITY_LOW      = 0
BUCKET_PRIORITY_NORMAL   = 1
BUCKET_PRIORITY_HIGH     = 2
BUCKET_PRIORITY_CRITICAL = 3
```

**主要 API**：
```c
bool bucket_priority_queue_init(bucket_priority_queue_t *queue, size_t capacity_per_bucket);
void bucket_priority_queue_destroy(bucket_priority_queue_t *queue);
void bucket_priority_queue_clear(bucket_priority_queue_t *queue);
bool bucket_priority_queue_reserve(bucket_priority_queue_t *queue, size_t capacity_per_bucket);

bool bucket_priority_queue_push(bucket_priority_queue_t *queue,
                                 bucket_priority_t priority,
                                 bucket_priority_value_t value);
bool bucket_priority_queue_pop(bucket_priority_queue_t *queue,
                                bucket_priority_value_t *out_value);
bool bucket_priority_queue_peek(const bucket_priority_queue_t *queue,
                                 bucket_priority_value_t *out_value);
size_t bucket_priority_queue_pop_batch(bucket_priority_queue_t *queue,
                                        size_t max_items,
                                        bucket_priority_value_t *out_values);

bool bucket_priority_queue_empty(const bucket_priority_queue_t *queue);
size_t bucket_priority_queue_size(const bucket_priority_queue_t *queue);
```

**使用场景**：
- ✅ 任务调度（4 级优先级）
- ✅ 定时器队列
- ✅ 事件分发
- ❌ 需要更多优先级（只支持 4 级）

**示例**：
```c
bucket_priority_queue_t queue;
bucket_priority_queue_init(&queue, 256);

// 推入不同优先级
bucket_priority_queue_push(&queue, BUCKET_PRIORITY_LOW, 1);
bucket_priority_queue_push(&queue, BUCKET_PRIORITY_HIGH, 2);
bucket_priority_queue_push(&queue, BUCKET_PRIORITY_CRITICAL, 3);

// 弹出（按优先级）
bucket_priority_value_t val;
while (bucket_priority_queue_pop(&queue, &val)) {
    printf("Process: %zu\n", val);  // 3, 2, 1 顺序
}

bucket_priority_queue_destroy(&queue);
```

---

## 编码与工具

### Base64 编解码 (`base64_utils.h`)

**主要 API**：
```c
int tn_base64_encode(const uint8_t *data, size_t len, char **output);
int tn_base64_encode_buf(const uint8_t *data, size_t len, char *out, size_t out_cap);
int tn_base64_decode(const char *input, uint8_t **output, size_t *output_len);
```

**使用场景**：
- ✅ HTTP Basic Auth
- ✅ 嵌入二进制数据（JSON/XML）
- ✅ 数据传输编码

**示例**：
```c
// 编码
const uint8_t data[] = {0x12, 0x34, 0x56};
char *encoded;
tn_base64_encode(data, sizeof(data), &encoded);
printf("Encoded: %s\n", encoded);
free(encoded);

// 解码
uint8_t *decoded;
size_t decoded_len;
tn_base64_decode("EjRW", &decoded, &decoded_len);
free(decoded);
```

---

### 类型安全格式化 (`fmt.h`)

轻量级、零分配的类型安全格式化库，C/C++ 双模支持。使用 `{}` 占位符语法，修饰符透传 `printf`。

**主要 API**：
```c
fmt(buf, size, format, ...);  // 宏：包装 fmt_print + FMT_ARGS
int fmt_print(char *buf, size_t size, const char *format,
              const fmt_arg_t *args, size_t arg_count);

FMT_ARG(value)       // 单个参数
FMT_ARGS(...)        // 多个参数
FMT_NARGS(...)       // 参数个数
FMT_TIME(time_value) // time_t 转时间参数

tstr_t tstr_cat_typed(tstr_t s, const char *format, ...);
```

**基本使用**：
```c
char buf[128];

fmt(buf, sizeof(buf), "hello {}", "world");
fmt(buf, sizeof(buf), "{} + {} = {}", 1, 2, 3);
fmt(buf, sizeof(buf), "{:08d} {:x} {:.2f}", 42, 255, 3.14159);

tstr_t s = tstr_new();
s = tstr_cat_typed(s, "id={} name={}", 42, "alice");
tstr_free(s);

TLOG_INFO("port={}, path={}", port, path);
```

**支持类型**：
- 整数、浮点、字符串、指针、`size_t`、`bool`
- `tstr_v`：按 `data + len` 拷贝，不要求 NUL 结尾
- `turbo_timeval_t` / `FMT_TIME(time_t)`：时间格式化
- C++：`std::string`、有 `c_str()` 的类型、`data()+size()` 视图、枚举、任意指针

**限制**：
- 最多 8 个格式化参数
- 单个参数格式化后最大 256 字节
- 修饰符最大 60 字节
- `tstr_cat_typed()` 单次追加最大 1024 字节，返回值必须重新赋值
- 不支持位置参数、命名参数、自定义对齐和千位分隔符

---

## 使用场景映射表

| 需求场景 | 优先使用 | 替代方案 | 禁止使用 |
|---------|---------|---------|---------|
| 解析器临时内存 | `mem_pool_t` + `mem_reset()` | `MemoryPool` | 全局 `malloc` |
| AST 节点分配 | `object_pool_t` | `mem_pool_t` | 裸 `malloc`/`free` |
| 字符串拼接 | `tstr_cat()` | `sds`（vendor） | `char*` + `strcat` |
| 只读字符串引用 | `tstr_v` | `const char* + size_t` | 拷贝字符串 |
| 配置文件读取 | `turbo_fs_read_file()` | `turbo_mmap_open()` | 裸标准 I/O |
| 大文件零拷贝 | `turbo_mmap_open()` | `turbo_fs_pread()` | 手写逐块读取循环 |
| 流式文件读取 | `turbo_fs_open()` + `turbo_fs_read()` | `turbo_fs_pread()` | 裸标准 I/O |
| 日志记录 | `TLOG_INFO()` | `turbo_sink_file_create()` | `printf`/`fprintf` |
| 互斥锁 | `turbo_mutex_t` | C11 `mtx_t` | 平台原生锁直接使用 |
| 读写锁 | `turbo_rwlock_t` | `turbo_mutex_t` | 手写双锁 |
| 线程创建 | `turbo_thread_create()` | C11 `thrd_create` | 平台线程 API 直接使用 |
| 线程池 | `turbo_threadpool_create()` | 手写线程池 | 每任务创建线程 |
| MPMC 队列 | `disruptor_t` | 带锁队列 | 手写无锁队列 |
| SPSC 队列 | `ring_buffer_spsc` | `disruptor_t` | 带锁队列 |
| 优先队列 | `bucket_priority_queue_t` | 手写堆 | `qsort` 排序 |
| Base64 编码 | `tn_base64_encode()` | aklomp-base64（vcpkg） | 手写 Base64 |
| 网络缓冲区 | `mem_buffer_t` + `mem_slice()` | `turbo_mmap_t` | 裸 `malloc` |

---

## 性能指标参考

| 工具 | 吞吐量 | 延迟 | 内存特性 |
|-----|--------|------|---------|
| `mem_pool_t` | - | O(1) 分配 | Slab 分配，预分配块 |
| `object_pool_t` | 100M+ ops/s | O(1) | Free-list，缓存友好 |
| `MemoryPool` | - | O(1) | Arena，批量分配 |
| `tstr_t` | - | O(1) 长度 | 动态扩容，O(n) 拷贝 |
| `tstr_v` | - | O(1) | 零拷贝，栈分配 |
| `turbo_fs_read_file()` | - | 阻塞 I/O | 单次 `malloc` |
| `turbo_mmap_open()` | - | 页错误延迟 | 虚拟内存，零拷贝 |
| `tlog` | ~9M ops/s (4线程) | <100ns (异步) | Lock-free ring buffer |
| `turbo_mutex_t` | - | 微秒级 | TurboNet 跨平台封装 |
| `turbo_rwlock_t` | - | 微秒级 | 多读单写 |
| `turbo_threadpool_t` | - | 毫秒级 | 固定线程，队列调度 |
| `disruptor_t` | 数百万 ops/s | 微秒级 | MPMC，预分配环形 |
| `ring_buffer_spsc` | 数千万 ops/s | 纳秒级 | SPSC，无锁 |
| `bucket_priority_queue` | - | O(1) push/pop | 4 个环形缓冲区 |

---

## 典型工作流示例

### 高性能解析器

```c
// 1. 创建 slab 池用于临时分配
mem_pool_t pool;
mem_init(&pool, 0);

// 2. 创建对象池用于 AST 节点
object_pool_config_t cfg = {.object_size = sizeof(ast_node_t), .initial_capacity = 10000};
object_pool_t *ast_pool = object_pool_create(&cfg);

// 3. 读取源文件（零拷贝）
turbo_mmap_t mmap;
turbo_mmap_open(&mmap, "source.tbs", TURBO_MMAP_READ);
const char *source = turbo_mmap_data(&mmap);
size_t source_len = turbo_mmap_size(&mmap);

// 4. 解析（使用池）
ast_node_t *root = object_pool_alloc(ast_pool);
parse(source, source_len, root, &pool, ast_pool);

// 5. 清理
mem_reset(&pool);  // 释放临时内存
turbo_mmap_close(&mmap);
// AST 节点保留在 ast_pool 中供后续使用
```

### 高并发服务器

```c
// 1. 初始化日志
tlog_t *logger = tlog_create(&(tlog_config_t){.min_level = TURBO_LOG_LEVEL_INFO});
tlog_add_sink(logger, turbo_sink_file_create(&(turbo_file_sink_opts_t){
    .path = "server.log", .max_size = 100*1024*1024, .max_files = 10
}));
tlog_set_default(logger);

// 2. 创建线程池
turbo_threadpool_t *pool = turbo_threadpool_create(0);  // auto-detect cores

// 3. 创建 Disruptor 事件队列
disruptor_t *events = disruptor_create(&(disruptor_config_t){
    .entry_size = sizeof(request_t), .capacity = 4096, .consumer_capacity = 16
});

// 4. 启动消费者线程
turbo_thread_t consumers[4];
for (int i = 0; i < 4; i++) {
    turbo_thread_create(&consumers[i], consumer_thread, events);
}

// 5. 主线程接收请求，发布到 Disruptor
while (running) {
    request_t *req = accept_request();
    disruptor_cursor_t cursor;
    request_t *entry = disruptor_publisher_next_entry_and_acquire_blocking(events, &cursor);
    memcpy(entry, req, sizeof(request_t));
    disruptor_publisher_publish(events, &cursor);
}

// 6. 清理
for (int i = 0; i < 4; i++) {
    turbo_thread_join(&consumers[i]);
    turbo_thread_destroy(&consumers[i]);
}
turbo_threadpool_destroy(pool);
disruptor_destroy(events);
tlog_destroy(logger);
```

---

## 常见陷阱与最佳实践

### ❌ 不要

- **不要**混用不同内存管理器（`mem_pool_t` 分配 → `free()` 释放）
- **不要**跨池传递指针（`mem_pool_t A` 分配 → 传给使用 `mem_pool_t B` 的函数）
- **不要**在热路径使用 `tstr_cat()`（每次可能 `realloc`，用 `tstr_reserve()` 预分配）
- **不要**在 SPSC `ring_buffer` 上多生产者/消费者（数据竞争）
- **不要**在 `turbo_mmap_t` 上频繁 `sync()`（严重性能损失）
- **不要**在锁内执行 I/O 或长时间计算（死锁/性能下降）

### ✅ 务必

- **务必**在使用 `tstr_cat()` 后重新赋值：`s = tstr_cat(s, "text")`
- **务必**在 `mem_buffer_t` 使用完后 `mem_unref()`（引用计数）
- **务必**在 `object_pool_free()` 前清理对象内部资源（池不调用析构函数）
- **务必**在生产环境关闭 DEBUG 日志（`tlog_set_level(logger, TURBO_LOG_LEVEL_INFO)`）
- **务必**为 Disruptor 选择 2 的幂容量（性能优化）
- **务必**在多线程环境用 `turbo_fs_pread()` 而非 `turbo_fs_read()`（后者改变文件位置）

---

## 版本与兼容性

- **TurboNet 版本**：确认项目使用的 TurboNet 版本，API 可能有变化
- **C 标准**：需要 C11（`stdatomic.h`、`_Thread_local`）
- **平台支持**：以 TurboNet 构建配置与 util/coro 封装为准
- **依赖**：事件循环、文件系统、线程等能力通过 TurboNet 统一入口接入

---

## 参考资料

- **源码位置**：通过 `TURBONET_ROOT` 或 CMake 包配置定位
- **头文件目录**：`%TURBONET_ROOT%/turbonet/utils/include/`
- **测试示例**：查看 TurboNet 仓库的 `tests/` 和 `examples/` 目录
- **性能基准**：参考 TurboNet 文档中的 benchmark 数据

---

**最后更新**：2026-07-05  
**适用项目**：TurboScript 及所有使用 TurboNet Utils 的 C 项目
