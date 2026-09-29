# 日志系统最佳实践

## 概述

本文档定义日志记录的量化约束、质量规范和性能控制，确保日志有效、高性能且可维护。

**核心原则**：
1. **有效性优先**：每条日志必须提供可操作信息
2. **量化约束**：严格控制日志数量，避免日志泛滥
3. **性能敏感**：热路径禁止日志，非热路径使用异步日志
4. **结构化优先**：使用类型安全格式化，便于搜索和分析

---

## 日志数量与性能约束

### 禁止日志的场景（严格限制）

#### 1. 热路径（每秒 >100 次）

```c
// ❌ 错误：热路径记录日志
for (int i = 0; i < 1000000; i++) {
    TLOG_INFO("Processing item {}", i);  // 禁止！
    process(items[i]);
}

// ✅ 正确：采样或完全移除
for (int i = 0; i < 1000000; i++) {
    if (i % 10000 == 0) {  // 采样：每 10000 次记录 1 次
        TLOG_DEBUG("Processing batch, current={}", i);
    }
    process(items[i]);
}

// 或者完全移除日志
for (int i = 0; i < 1000000; i++) {
    process(items[i]);  // 无日志
}
```

#### 2. 循环内（INFO 以上）

```c
// ❌ 错误：循环内 INFO 日志
for (int i = 0; i < count; i++) {
    TLOG_INFO("Item {}: {}", i, items[i].name);
}

// ✅ 正确：循环外记录摘要
TLOG_INFO("Processing {} items", count);
for (int i = 0; i < count; i++) {
    process(items[i]);
}
TLOG_INFO("Processed {} items successfully", count);

// 或使用 DEBUG 级别 + 采样
for (int i = 0; i < count; i++) {
    if (i % 1000 == 0) {
        TLOG_DEBUG("Progress: {}/{}", i, count);
    }
    process(items[i]);
}
```

#### 3. 递归（深度 >10）

```c
// ❌ 错误：每层都打印日志
int fibonacci(int n, int depth) {
    TLOG_DEBUG("fib({}), depth={}", n, depth);  // 禁止！
    if (n <= 1) return n;
    return fibonacci(n - 1, depth + 1) + fibonacci(n - 2, depth + 1);
}

// ✅ 正确：只在入口记录
int fibonacci(int n) {
    TLOG_DEBUG("Computing fibonacci({})", n);
    return fibonacci_impl(n);  // 实现中无日志
}

static int fibonacci_impl(int n) {
    if (n <= 1) return n;
    return fibonacci_impl(n - 1) + fibonacci_impl(n - 2);
}
```

#### 4. 中间层（调用栈中间）

```c
// ❌ 错误：每层都记录
void layer3() {
    TLOG_INFO("Enter layer3");  // 冗余
    do_work();
    TLOG_INFO("Exit layer3");   // 冗余
}

void layer2() {
    TLOG_INFO("Enter layer2");  // 冗余
    layer3();
    TLOG_INFO("Exit layer2");   // 冗余
}

void layer1() {
    TLOG_INFO("Enter layer1");  // 冗余
    layer2();
    TLOG_INFO("Exit layer1");   // 冗余
}

// ✅ 正确：只在边界层记录
void layer1() {
    TLOG_INFO("Starting operation");
    layer2();  // 无日志
    TLOG_INFO("Operation completed");
}
```

#### 5. 高频构造/析构

```c
// ❌ 错误：每次创建都记录
typedef struct {
    int id;
} item_t;

item_t *item_create(int id) {
    TLOG_DEBUG("Creating item {}", id);  // 禁止！
    item_t *item = malloc(sizeof(item_t));
    item->id = id;
    return item;
}

// 每秒创建 10000 次 → 10000 条日志

// ✅ 正确：移除构造/析构日志
item_t *item_create(int id) {
    item_t *item = malloc(sizeof(item_t));
    item->id = id;
    return item;  // 无日志
}
```


### 允许日志的场景

#### 1. 错误边界

```c
// ✅ 捕获异常/错误转换
int process_request(request_t *req) {
    int result = validate_request(req);
    if (result != 0) {
        TLOG_ERROR("Request validation failed: code={}, path={}", 
                   result, req->path);
        return result;
    }
    // ...
}
```

#### 2. 关键里程碑

```c
// ✅ 启动/关闭
TLOG_INFO("Server starting on port {}", port);
// ...
TLOG_INFO("Server stopped gracefully");

// ✅ 连接建立/断开
TLOG_INFO("Client connected: addr={}, id={}", addr, client_id);
// ...
TLOG_INFO("Client disconnected: id={}, duration={}s", client_id, duration);

// ✅ 状态迁移
TLOG_INFO("State transition: {} -> {}", old_state, new_state);
```

#### 3. 配置变更

```c
// ✅ 运行时配置更新
TLOG_INFO("Config updated: max_connections {} -> {}", 
          old_max, new_max);

// ✅ Feature flag 切换
TLOG_INFO("Feature '{}' {}", feature_name, enabled ? "enabled" : "disabled");
```

#### 4. 审计事件

```c
// ✅ 权限变更
TLOG_WARN("User {} granted admin privileges by {}", 
          user_id, admin_id);

// ✅ 数据修改
TLOG_INFO("Record {} updated by user {}", record_id, user_id);

// ✅ 安全事件
TLOG_ERROR("Failed login attempt: user={}, ip={}, attempts={}", 
           username, ip_addr, attempt_count);
```

---

### 日志数量量化标准

| 场景 | 最大数量 | 说明 |
|------|---------|------|
| 单次请求 | ≤10 条 | INFO+WARN+ERROR 总和 |
| 单个函数 | ≤3 条 | 不含子函数调用 |
| 启动/关闭 | ≤50 条 | 包含所有模块初始化 |
| 热路径（>100/s） | 0 条 | 绝对禁止 INFO 以上 |
| DEBUG 日志 | 采样 | 高频事件必须采样（如每 1000 次记录 1 次）|

**超标处理**：
- 超过标准必须在代码审查中说明理由
- 考虑合并多条日志为一条
- 使用结构化日志减少冗余信息

---

### 日志性能开销控制

#### 1. 延迟求值

```c
// ❌ 错误：总是计算参数（即使 DEBUG 关闭）
TLOG_DEBUG("Expensive data: {}", compute_expensive_string());

// ✅ 正确：宏自动延迟求值
#define TLOG_DEBUG(fmt, ...)                                    \
    do {                                                        \
        if (tlog_get_level(tlog_peek_default()) <= TURBO_LOG_LEVEL_DEBUG) { \
            TURBO_LOG_TYPED(tlog_get_default(), TURBO_LOG_LEVEL_DEBUG,     \
                           NULL, fmt, ##__VA_ARGS__);           \
        }                                                       \
    } while (0)

// 使用时不会在 DEBUG 关闭时计算参数
TLOG_DEBUG("Value: {}", expensive_call());  // DEBUG 关闭时不调用 expensive_call()
```

#### 2. 异步日志

```c
// TurboNet tlog 默认使用异步写入
tlog_config_t cfg = {
    .min_level = TURBO_LOG_LEVEL_INFO,
    .buffer_size = 64 * 1024,  // 64KB ring buffer
    .pool_size = 32 * 1024     // 内存池
};
tlog_t *logger = tlog_create(&cfg);

// 日志调用立即返回（写入 ring buffer）
TLOG_INFO("Message");  // ~100ns，非阻塞

// 后台线程异步写入文件
```

#### 3. 批量刷盘

```c
// 日志配置
turbo_file_sink_opts_t opts = {
    .path = "app.log",
    .max_size = 100 * 1024 * 1024,
    .max_files = 10,
    .append = 1
};

// 内部实现：
// - 非 ERROR 日志：缓冲区满或定时（1 秒）刷盘
// - ERROR 日志：立即刷盘（确保不丢失）
```

#### 4. 采样策略

```c
// 高频事件采样
static uint64_t counter = 0;

void high_frequency_event() {
    counter++;
    if (counter % 1000 == 0) {  // 每 1000 次记录 1 次
        TLOG_DEBUG("High freq event count: {}", counter);
    }
    // ...
}

// 或随机采样
#include <stdlib.h>

if (rand() % 100 == 0) {  // 1% 采样率
    TLOG_DEBUG("Sampled event data: {}", data);
}
```

#### 5. 日志开关

```c
// 运行时动态调整日志级别
void set_log_level(const char *level_str) {
    turbo_log_level_t level = turbo_log_level_from_name(level_str);
    tlog_set_level(tlog_get_default(), level);
    TLOG_INFO("Log level changed to {}", level_str);
}

// 生产环境默认 INFO，排查问题时临时开启 DEBUG
// $ kill -SIGUSR1 <pid>  // 信号处理器调用 set_log_level("DEBUG")
```

---

## 日志内容质量规范

### 必须包含的信息

每条日志必须包含以下信息的子集（根据级别）：

#### 1. 操作类型

```c
// ✅ 明确操作
TLOG_INFO("Parsing configuration file: path={}", path);
TLOG_INFO("Connecting to database: host={}, port={}", host, port);
TLOG_INFO("Starting HTTP server on port {}", port);
```

#### 2. 关键标识

```c
// ✅ 包含标识符
TLOG_ERROR("Failed to open file: path={}, error={}", path, strerror(errno));
TLOG_INFO("User {} logged in from {}", user_id, ip_addr);
TLOG_DEBUG("Processing request: id={}, method={}, path={}", 
           req_id, method, path);
```

#### 3. 失败原因

```c
// ❌ 错误：无具体原因
TLOG_ERROR("Operation failed");

// ✅ 正确：具体错误信息
TLOG_ERROR("Failed to connect to database: host={}, port={}, error={}", 
           host, port, strerror(errno));

// ✅ 更好：包含期望值 vs 实际值
TLOG_ERROR("Invalid parameter: expected range [0, 100], got {}", value);
```

#### 4. 可操作建议（ERROR/WARN）

```c
// ✅ ERROR：说明如何恢复
TLOG_ERROR("Config file not found: {}. Using default configuration. "
           "To specify custom config, use --config=<path>", path);

// ✅ WARN：说明风险和建议
TLOG_WARN("Memory usage {}MB exceeds {}MB threshold. "
          "Consider increasing max_memory or reducing cache size.",
          current_mb, threshold_mb);
```

---

### 禁止的日志内容

#### 1. 无信息量

```c
// ❌ 禁止
TLOG_ERROR("Error occurred");
TLOG_INFO("Something wrong");
TLOG_DEBUG("Debug 1");

// ✅ 正确
TLOG_ERROR("Failed to parse JSON: {}, at line {}", error_msg, line_num);
TLOG_INFO("Configuration validation failed: missing required field '{}'", field);
TLOG_DEBUG("Token type: {}, value: '{}'", token_type, token_value);
```

#### 2. 缺少上下文

```c
// ❌ 禁止：哪个文件？
TLOG_ERROR("File not found");

// ✅ 正确
TLOG_ERROR("File not found: path={}, cwd={}", path, getcwd(NULL, 0));

// ❌ 禁止：哪个参数？
TLOG_ERROR("Invalid parameter");

// ✅ 正确
TLOG_ERROR("Invalid parameter '{}': expected integer in range [0, 100], got '{}'",
           param_name, param_value);
```

#### 3. 技术黑话（对用户）

```c
// ❌ 禁止：用户看不懂
TLOG_ERROR("Null pointer dereference at 0x{:x}", addr);
TLOG_ERROR("Segmentation fault");

// ✅ 正确：转换为业务语言
TLOG_ERROR("Failed to access data: record {} does not exist", record_id);
TLOG_ERROR("System error: please contact support with error code E{:04d}", code);
```

#### 4. 重复堆栈

```c
// ❌ 禁止：每层都打印完整堆栈
void layer3() {
    if (error) {
        TLOG_ERROR("Layer3 error\n{}", get_stacktrace());
        return ERR_LAYER3;
    }
}

void layer2() {
    int result = layer3();
    if (result != 0) {
        TLOG_ERROR("Layer2 error, caused by layer3\n{}", get_stacktrace());
        return ERR_LAYER2;
    }
}

// ✅ 正确：只在边界打印一次
void layer3() {
    if (error) {
        return ERR_LAYER3;  // 只返回错误码
    }
}

void layer1() {
    int result = layer2();
    if (result != 0) {
        TLOG_ERROR("Operation failed: code={}\n{}", result, get_stacktrace());
        return result;
    }
}
```

#### 5. 调试残留

```c
// ❌ 禁止：未移除的调试日志
printf("Debug: value = %d\n", value);
fprintf(stderr, "Here!\n");

// ✅ 正确：使用 TLOG_DEBUG 且有意义
TLOG_DEBUG("Validation result: value={}, range=[{}, {}], valid={}", 
           value, min, max, is_valid);

// 发布前确认：所有 printf/fprintf 已移除或转为 TLOG_*
```

---

### 日志格式标准

#### 结构化日志

**格式模板**：
```
[时间戳] [级别] [模块] [线程] 消息内容 key1=value1 key2=value2
```

**TurboNet tlog 配置**：
```c
turbo_file_sink_opts_t opts = {
    .path = "app.log",
    .pattern = "[{time_ms}] [{level}] [{component}] [{thread}] {message}"
};
```

**示例输出**：
```
[2026-07-05T10:30:45.123456] [ERROR] [Parser] [T-1234] Failed to parse JSON file path=/data/input.json line=42 reason="unexpected token"
[2026-07-05T10:30:45.234567] [INFO] [Server] [T-1] Server started port=8080 max_connections=1000
[2026-07-05T10:30:45.345678] [WARN] [Cache] [T-5678] Cache hit rate low rate=45.2% threshold=60.0%
```

#### 必须字段

- **时间戳**：微秒级精度（`{time_ms}`）
- **级别**：DEBUG/INFO/WARN/ERROR/FATAL
- **模块**：组件名（Parser、Server、Cache 等）
- **线程 ID**：使用日志系统内置 `{thread}` 字段；需要手工采集时走 TurboNet 线程抽象，不直接调用平台线程 API
- **文件:行号**（可选，DEBUG 模式）：`{file}:{line}`

#### 可选字段（根据场景）

- **请求 ID**：用于追踪分布式请求
- **用户 ID**：审计日志
- **会话 ID**：会话追踪
- **Trace ID**：分布式追踪

```c
// 使用自定义字段
TLOG_INFO("Request completed: req_id={}, user_id={}, duration_ms={}, status={}", 
          req_id, user_id, duration_ms, status_code);
```

---

### 日志级别使用规范

#### ERROR - 用户需要立即关注

**定义**：导致功能失败、数据不一致或安全风险的错误

**必须包含**：
- 错误码
- 堆栈（可选，严重错误）
- 完整上下文
- 恢复建议

**示例**：
```c
// ✅ 数据库连接失败
TLOG_ERROR("Database connection failed: host={}, port={}, error={}, "
           "retry_count={}, max_retries={}. Check network and credentials.",
           host, port, strerror(errno), retry_count, max_retries);

// ✅ 数据不一致
TLOG_ERROR("Data inconsistency detected: expected checksum={:x}, "
           "actual={:x}, file={}. Data may be corrupted.",
           expected_checksum, actual_checksum, filename);

// ✅ 安全事件
TLOG_ERROR("Authorization failed: user={}, resource={}, required_permission={}",
           user_id, resource_path, required_perm);
```

#### WARN - 需要监控但系统仍可运行

**定义**：降级服务、重试成功、资源接近上限

**示例**：
```c
// ✅ 降级逻辑
TLOG_WARN("Cache miss, falling back to database: key={}, cache_hit_rate={:.1f}%",
          key, hit_rate * 100);

// ✅ 重试成功
TLOG_WARN("Request retried successfully: url={}, attempt={}, total_time_ms={}",
          url, attempt_count, total_time);

// ✅ 资源告警
TLOG_WARN("Memory usage high: current={}MB, threshold={}MB, available={}MB",
          current_mb, threshold_mb, available_mb);
```

#### INFO - 关键路径里程碑

**定义**：启动/关闭、主要状态迁移、外部交互

**示例**：
```c
// ✅ 启动
TLOG_INFO("Server started: version={}, port={}, workers={}, config={}",
          VERSION, port, worker_count, config_path);

// ✅ 状态迁移
TLOG_INFO("State transition: old={}, new={}, trigger={}, duration_ms={}",
          old_state_name, new_state_name, trigger_event, duration);

// ✅ 外部交互
TLOG_INFO("API request: method={}, path={}, client_ip={}, user_agent={}",
          method, path, client_ip, user_agent);
```

#### DEBUG - 详细执行流程

**定义**：中间状态、变量值、算法步骤

**仅在开发和排查时开启**

**示例**：
```c
// ✅ 解析细节
TLOG_DEBUG("Token parsed: type={}, value='{}', position={}:{}", 
           token_type, token_value, line, col);

// ✅ 算法步骤
TLOG_DEBUG("Binary search: low={}, high={}, mid={}, target={}, found={}",
           low, high, mid, target, found);

// ✅ 变量状态
TLOG_DEBUG("Context state: flags={:b}, counter={}, buffer_size={}, pos={}",
           flags, counter, buffer_size, position);
```

---

### 禁止级别误用

```c
// ❌ 错误：INFO 记录堆栈
TLOG_INFO("Function called\n{}", get_stacktrace());

// ❌ 错误：DEBUG 记录关键错误
TLOG_DEBUG("Database connection lost");  // 应该是 ERROR

// ❌ 错误：WARN 记录正常操作
TLOG_WARN("User logged in");  // 应该是 INFO

// ❌ 错误：ERROR 记录非错误
TLOG_ERROR("Cache miss");  // 应该是 DEBUG 或 WARN
```

---

## 日志文件管理

### 文件命名

**格式**：`{service}.{level}.{date}.log`

**示例**：
```
turboscript.error.2026-07-05.log
turboscript.info.2026-07-05.log
turboscript.debug.2026-07-05.log
```

**分级存储**：
- ERROR/WARN 单独文件
- INFO/DEBUG 单独文件

### 日志滚动策略

#### 1. 按大小滚动

```c
turbo_file_sink_opts_t opts = {
    .path = "app.log",
    .max_size = 100 * 1024 * 1024,  // 100MB
    .max_files = 10,                 // 保留 10 个文件
    .append = 1
};

// 文件命名：
// app.log          (当前)
// app.log.1        (上一个)
// app.log.2
// ...
// app.log.10       (最旧，超过后删除)
```

#### 2. 按时间滚动

**每天 00:00 创建新文件**：
```c
// 文件名包含日期
char filename[256];
time_t now = time(NULL);
struct tm *tm = localtime(&now);
snprintf(filename, sizeof(filename), "app.%04d-%02d-%02d.log",
         tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
```

#### 3. 压缩归档

```c
// 日志轮转和归档策略应通过 tlog sink 或部署层统一配置
turbo_file_sink_opts_t archive_opts = {
    .path = "logs/app.log",
    .rotate_max_files = 30,
    .rotate_compress = true,
    .reopen_on_rotate = true,
};
```

### 保留策略

| 级别 | 保留时间 | 压缩 | 原因 |
|------|---------|------|------|
| ERROR | 90 天 | 是 | 长期审计、问题追溯 |
| WARN | 30 天 | 是 | 中期监控、趋势分析 |
| INFO | 30 天 | 是 | 运维审计、性能分析 |
| DEBUG | 7 天 | 否 | 短期排查、开发调试 |

```c
// 清理脚本（cron job）
# 每天凌晨 2 点清理旧日志
0 2 * * * /usr/local/bin/cleanup_logs.sh

# cleanup_logs.sh
find /var/log/turboscript -name "*.error.*.log.gz" -mtime +90 -delete
find /var/log/turboscript -name "*.info.*.log.gz" -mtime +30 -delete
find /var/log/turboscript -name "*.debug.*.log" -mtime +7 -delete
```

---

### 日志路径管理

**配置外部化**：
```c
// config.toml
[logging]
path = "/var/log/turboscript"
level = "INFO"
max_size_mb = 100
max_files = 10
```

**权限控制**：
```bash
# 日志目录权限
chmod 700 /var/log/turboscript
chown turboscript:turboscript /var/log/turboscript

# 日志文件权限
chmod 600 /var/log/turboscript/*.log
```

**磁盘空间检查**：
```c
#include <sys/statvfs.h>

bool check_disk_space(const char *path, uint64_t min_free_bytes) {
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        return false;
    }
    
    uint64_t free_bytes = stat.f_bavail * stat.f_frsize;
    if (free_bytes < min_free_bytes) {
        TLOG_ERROR("Low disk space: path={}, free={}MB, required={}MB",
                   path, free_bytes / 1024 / 1024, min_free_bytes / 1024 / 1024);
        return false;
    }
    return true;
}

// 日志写入前检查
if (!check_disk_space("/var/log", 1024 * 1024 * 1024)) {  // 1GB
    // 停止 DEBUG 日志，只保留 ERROR
    tlog_set_level(logger, TURBO_LOG_LEVEL_ERROR);
}
```

---

## 生产环境日志

### 默认配置

```c
// 生产环境推荐配置
tlog_config_t prod_config = {
    .min_level = TURBO_LOG_LEVEL_INFO,  // 默认 INFO
    .buffer_size = 64 * 1024,
    .pool_size = 32 * 1024
};

turbo_file_sink_opts_t prod_file_opts = {
    .path = "/var/log/turboscript/app.log",
    .max_size = 100 * 1024 * 1024,  // 100MB
    .max_files = 10,
    .append = 1,
    .pattern = "[{time_ms}] [{level}] [{component}] {message}"
};

turbo_console_sink_opts_t prod_console_opts = {
    .output = stderr,
    .use_colors = 0,  // 生产环境不用颜色
    .pattern = "[{time}] [{level}] {message}"
};
```

### DEBUG 开关（临时排查）

**自动关闭机制**：
```c
#include <time.h>

typedef struct {
    tlog_t *logger;
    time_t debug_start_time;
    bool debug_enabled;
} logger_manager_t;

void enable_debug_logging(logger_manager_t *mgr, int duration_seconds) {
    tlog_set_level(mgr->logger, TURBO_LOG_LEVEL_DEBUG);
    mgr->debug_enabled = true;
    mgr->debug_start_time = time(NULL);
    
    TLOG_WARN("DEBUG logging enabled for {} seconds", duration_seconds);
}

void check_debug_timeout(logger_manager_t *mgr, int max_duration) {
    if (mgr->debug_enabled) {
        time_t now = time(NULL);
        if (now - mgr->debug_start_time > max_duration) {
            tlog_set_level(mgr->logger, TURBO_LOG_LEVEL_INFO);
            mgr->debug_enabled = false;
            TLOG_WARN("DEBUG logging auto-disabled after {} seconds", 
                      now - mgr->debug_start_time);
        }
    }
}

// 定时检查（如每 1 分钟）
// DEBUG 超过 5 分钟自动关闭
check_debug_timeout(&logger_mgr, 5 * 60);
```

**信号处理器**：
```c
#include <signal.h>

static logger_manager_t *g_logger_mgr = NULL;

void signal_handler(int signum) {
    if (signum == SIGUSR1) {
        // kill -SIGUSR1 <pid> 开启 DEBUG
        enable_debug_logging(g_logger_mgr, 300);  // 5 分钟
    } else if (signum == SIGUSR2) {
        // kill -SIGUSR2 <pid> 关闭 DEBUG
        tlog_set_level(g_logger_mgr->logger, TURBO_LOG_LEVEL_INFO);
        TLOG_INFO("DEBUG logging manually disabled");
    }
}

// 注册信号处理器
signal(SIGUSR1, signal_handler);
signal(SIGUSR2, signal_handler);
```

### 采样率（高频日志）

```c
typedef struct {
    uint64_t total_events;
    uint64_t sampled_events;
    double sample_rate;  // 0.01 = 1%
} sampler_t;

bool should_sample(sampler_t *sampler) {
    sampler->total_events++;
    
    // 简单随机采样
    if ((double)rand() / RAND_MAX < sampler->sample_rate) {
        sampler->sampled_events++;
        return true;
    }
    return false;
}

// 使用
static sampler_t event_sampler = {.sample_rate = 0.01};  // 1% 采样

void high_freq_event(const char *data) {
    if (should_sample(&event_sampler)) {
        TLOG_DEBUG("High freq event: data={}, total={}, sampled={}", 
                   data, event_sampler.total_events, event_sampler.sampled_events);
    }
    // 处理事件...
}
```

**动态调整采样率**：
```c
void adjust_sample_rate(sampler_t *sampler) {
    // 根据系统负载调整
    double load = get_system_load();
    if (load > 0.8) {
        sampler->sample_rate = 0.001;  // 高负载：0.1%
    } else if (load > 0.5) {
        sampler->sample_rate = 0.01;   // 中负载：1%
    } else {
        sampler->sample_rate = 0.1;    // 低负载：10%
    }
}
```

### 敏感信息脱敏

**强制脱敏（生产环境）**：
```c
// 脱敏函数
const char *mask_string(const char *str, int visible_chars) {
    static char masked[256];
    int len = strlen(str);
    
    if (len <= visible_chars) {
        return str;  // 太短，不脱敏
    }
    
    int visible = visible_chars;
    int masked_len = len - visible;
    
    strncpy(masked, str, visible);
    memset(masked + visible, '*', masked_len);
    masked[len] = '\0';
    
    return masked;
}

// 使用
TLOG_INFO("User login: username={}, password={}", 
          username, mask_string(password, 0));  // "********"

TLOG_INFO("Credit card: {}", 
          mask_string(card_number, 4));  // "1234************"

// 邮箱脱敏
const char *mask_email(const char *email) {
    static char masked[256];
    const char *at = strchr(email, '@');
    if (!at) return "***";
    
    int prefix_len = at - email;
    int visible = prefix_len > 2 ? 2 : 1;
    
    strncpy(masked, email, visible);
    memset(masked + visible, '*', prefix_len - visible);
    strcpy(masked + prefix_len, at);
    
    return masked;
}

// "user@example.com" → "us***@example.com"
TLOG_INFO("Email sent to: {}", mask_email(email));
```

---

## TurboNet tlog 使用指南

### 初始化完整示例

```c
#include "tlog.h"

int init_logging(const char *log_path, const char *level_str) {
    // 1. 创建 logger
    tlog_config_t cfg = {
        .min_level = turbo_log_level_from_name(level_str),
        .buffer_size = 64 * 1024,
        .pool_size = 32 * 1024
    };
    tlog_t *logger = tlog_create(&cfg);
    if (!logger) {
        fprintf(stderr, "Failed to create logger\n");
        return -1;
    }
    
    // 2. 添加 console sink（stderr）
    turbo_console_sink_opts_t console_opts = {
        .output = stderr,
        .use_colors = 1,
        .pattern = "[{time}] [{level}] {message}"
    };
    turbo_log_sink_t *console_sink = turbo_sink_console_create(&console_opts);
    if (console_sink) {
        tlog_add_sink(logger, console_sink);
    }
    
    // 3. 添加 file sink
    turbo_file_sink_opts_t file_opts = {
        .path = log_path,
        .max_size = 100 * 1024 * 1024,
        .max_files = 10,
        .append = 1,
        .pattern = "[{time_ms}] [{level}] [{component}] [{thread}] {message}"
    };
    turbo_log_sink_t *file_sink = turbo_sink_file_create(&file_opts);
    if (file_sink) {
        tlog_add_sink(logger, file_sink);
    }
    
    // 4. 设为默认 logger
    tlog_set_default(logger);
    
    TLOG_INFO("Logging initialized: path={}, level={}", log_path, level_str);
    return 0;
}

void shutdown_logging(void) {
    tlog_t *logger = tlog_get_default();
    if (logger) {
        TLOG_INFO("Shutting down logging system");
        tlog_flush(logger);  // 确保所有日志写入
        tlog_destroy(logger);
    }
}
```

### 多 Sink 场景

```c
// 不同级别输出到不同位置
void init_multi_sink_logging(void) {
    tlog_t *logger = tlog_create(&(tlog_config_t){
        .min_level = TURBO_LOG_LEVEL_DEBUG
    });
    
    // Console：只显示 WARN 以上
    turbo_console_sink_opts_t console_opts = {
        .output = stderr,
        .use_colors = 1,
        .pattern = "[{level}] {message}"
    };
    turbo_log_sink_t *console = turbo_sink_console_create(&console_opts);
    console->min_level = TURBO_LOG_LEVEL_WARN;  // Per-sink 过滤
    tlog_add_sink(logger, console);
    
    // 文件 1：所有级别
    turbo_log_sink_t *all_file = turbo_sink_file_create(&(turbo_file_sink_opts_t){
        .path = "all.log",
        .max_size = 50 * 1024 * 1024,
        .max_files = 5
    });
    tlog_add_sink(logger, all_file);
    
    // 文件 2：只记录 ERROR
    turbo_log_sink_t *error_file = turbo_sink_file_create(&(turbo_file_sink_opts_t){
        .path = "error.log",
        .max_size = 10 * 1024 * 1024,
        .max_files = 20
    });
    error_file->min_level = TURBO_LOG_LEVEL_ERROR;
    tlog_add_sink(logger, error_file);
    
    tlog_set_default(logger);
}
```

### 自定义 Sink（回调）

```c
// 发送关键日志到监控系统
void monitoring_callback(const turbo_log_entry_t *entry, void *user_data) {
    if (entry->level >= TURBO_LOG_LEVEL_ERROR) {
        // 发送告警到监控系统
        send_alert_to_monitoring(entry->message, entry->level);
    }
}

// 添加自定义 sink
turbo_log_sink_t *monitoring_sink = turbo_sink_callback_create(
    monitoring_callback, NULL
);
monitoring_sink->min_level = TURBO_LOG_LEVEL_ERROR;
tlog_add_sink(logger, monitoring_sink);
```

---

## 日志审查检查清单

### 代码审查时检查

- [ ] 热路径（>100/s）无 INFO 以上日志
- [ ] 循环内无 INFO 日志（或已采样）
- [ ] 递归函数无每层日志
- [ ] 单次请求日志 ≤10 条
- [ ] 单个函数日志 ≤3 条
- [ ] 所有 ERROR 日志包含错误原因
- [ ] 所有 WARN 日志说明风险和建议
- [ ] 无"Error occurred"等无信息量日志
- [ ] 无缺少上下文的日志（如"File not found"）
- [ ] 无技术黑话（对用户可见时）
- [ ] 敏感信息已脱敏（密码、密钥、PII）
- [ ] 日志级别使用正确
- [ ] 已移除调试残留（printf、fprintf）

### 运行时监控

- [ ] 日志文件大小受控（定期滚动）
- [ ] 磁盘空间充足（>1GB）
- [ ] 日志写入性能正常（无阻塞）
- [ ] ERROR 日志数量可接受（<1% 请求）
- [ ] DEBUG 日志已关闭（生产环境）
- [ ] 日志采样率合理（高频事件）

---

## 常见陷阱与最佳实践

### ❌ 避免

1. **日志泛滥**
   ```c
   // ❌ 每次循环都记录
   for (int i = 0; i < 1000000; i++) {
       TLOG_INFO("Processing {}", i);
   }
   ```

2. **日志成为瓶颈**
   ```c
   // ❌ 在热路径强制同步刷新
   TLOG_INFO("Message");
   tlog_flush(logger);  // 每次 flush，性能差
   ```

3. **无意义日志**
   ```c
   // ❌ 无用信息
   TLOG_DEBUG("Here");
   TLOG_DEBUG("Value: {}", x);  // 什么 value？
   ```

4. **日志泄露敏感信息**
   ```c
   // ❌ 明文记录密码
   TLOG_DEBUG("User login: user={}, password={}", user, password);
   ```

5. **忽略日志级别**
   ```c
   // ❌ 所有日志都用 INFO
   TLOG_INFO("Error occurred");  // 应该是 ERROR
   ```

### ✅ 最佳实践

1. **合并日志**
   ```c
   // ✅ 一次记录多个信息
   TLOG_INFO("Batch processed: total={}, success={}, failed={}, duration_ms={}",
             total, success, failed, duration);
   ```

2. **异步日志**
   ```c
   // ✅ 使用 TurboNet tlog（默认异步）
   TLOG_INFO("Message");  // 非阻塞，写入 ring buffer
   ```

3. **结构化日志**
   ```c
   // ✅ key=value 格式
   TLOG_ERROR("Request failed: method={}, path={}, status={}, error={}", 
              method, path, status, error_msg);
   ```

4. **脱敏敏感信息**
   ```c
   // ✅ 脱敏或省略
   TLOG_INFO("User authenticated: user={}", username);
   // 不记录密码
   ```

5. **使用正确级别**
   ```c
   // ✅ 根据严重性选择级别
   TLOG_ERROR("Database connection lost");    // 错误
   TLOG_WARN("Cache hit rate low: {:.1f}%", hit_rate);  // 警告
   TLOG_INFO("Server started on port {}", port);  // 信息
   TLOG_DEBUG("Token: type={}, value='{}'", type, value);  // 调试
   ```

---

## 日志分析工具推荐

### 命令行工具

```bash
# 查找错误日志
grep ERROR app.log

# 统计错误数量
grep -c ERROR app.log

# 查看最近 100 条日志
tail -n 100 app.log

# 实时查看日志
tail -f app.log

# 按时间范围过滤（假设有时间戳）
awk '/2026-07-05 10:30/,/2026-07-05 10:35/' app.log

# 统计各级别日志数量
awk '{print $3}' app.log | sort | uniq -c
```

### 结构化日志查询

如果使用 JSON 格式日志：
```bash
# jq 查询 ERROR 级别
cat app.json.log | jq 'select(.level == "ERROR")'

# 统计错误类型
cat app.json.log | jq -r 'select(.level == "ERROR") | .error_code' | sort | uniq -c

# 查询特定用户的日志
cat app.json.log | jq 'select(.user_id == "12345")'
```

### 日志聚合工具

- **ELK Stack**（Elasticsearch + Logstash + Kibana）
- **Loki**（Grafana Loki）
- **Splunk**
- **Graylog**

---

## 总结

### 核心要点

1. **量化约束**：单次请求 ≤10 条，单个函数 ≤3 条，热路径 0 条
2. **质量第一**：每条日志必须有信息量、上下文和可操作建议
3. **性能优先**：异步日志、采样、延迟求值
4. **安全意识**：脱敏敏感信息、控制文件权限
5. **可维护性**：结构化格式、清晰级别、定期清理

### 一句话规则

**日志不是越多越好，而是每条都有价值。**

---

**最后更新**：2026-07-05  
**适用项目**：TurboScript 及所有使用 TurboNet tlog 的 C 项目  
**参考资料**：TurboNet tlog API 文档、《The Art of Logging》
