# 插件系统开发规范

## 概述

本文档定义 C 语言插件系统的完整架构、接口设计、隔离机制和最佳实践，适用于 TurboScript 等需要动态扩展的 C 项目。

**核心原则**：
1. **稳定 ABI**：纯 C 接口，二进制兼容
2. **隔离优先**：内存、资源、权限、崩溃隔离
3. **版本管理**：语义化版本、兼容性检查
4. **安全第一**：权限白名单、签名验证

---

## 插件架构

### 核心组件

```
┌─────────────────────────────────────────────┐
│              主程序 (Host)                   │
│  ┌───────────────────────────────────────┐  │
│  │     插件管理器 (Plugin Manager)        │  │
│  │  - 发现  - 注册  - 生命周期  - 通信   │  │
│  └───────────────────────────────────────┘  │
│           ↓          ↓          ↓            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ Plugin A │  │ Plugin B │  │ Plugin C │  │
│  │ (module) │  │ (module) │  │ (module) │  │
│  └──────────┘  └──────────┘  └──────────┘  │
└─────────────────────────────────────────────┘
```

### 插件生命周期

```
[发现] → [验证] → [加载] → [初始化] → [就绪] → [停止] → [卸载]
   ↓        ↓       ↓         ↓         ↓        ↓        ↓
 扫描目录  签名  加载模块   init()    注册服务  cleanup  释放句柄
 读取清单  版本检查          依赖解析
```

---

## 插件发现与注册

### 插件清单文件 (plugin.json)

```json
{
  "name": "math_plugin",
  "version": "1.2.0",
  "api_version": "1.0",
  "author": "TurboScript Team",
  "description": "Mathematical functions plugin",
  "library": "math_plugin.so",
  "entry_point": "plugin_init",
  "dependencies": [
    {"name": "core", "version": ">=1.0.0"}
  ],
  "optional_dependencies": [
    {"name": "vector", "version": ">=2.0.0"}
  ],
  "capabilities": ["math.basic", "math.advanced"],
  "permissions": ["file.read", "network.http"],
  "signature": "SHA256:abcdef..."
}
```

### 插件发现

```c
// plugin_discovery.h
typedef struct {
    char name[64];
    char version[32];
    char path[256];
    char manifest_path[256];
} plugin_info_t;

typedef struct {
    plugin_info_t *plugins;
    size_t count;
    size_t capacity;
} plugin_registry_t;

/**
 * @brief 扫描插件目录
 * @param dir 插件目录路径
 * @param registry 插件注册表
 * @return 0 成功，<0 失败
 */
int plugin_discover(const char *dir, plugin_registry_t *registry);

/**
 * @brief 解析插件清单
 * @param manifest_path 清单文件路径
 * @param info 输出插件信息
 * @return 0 成功，<0 失败
 */
int plugin_parse_manifest(const char *manifest_path, plugin_info_t *info);
```


```c
// plugin_discovery.c
int plugin_discover(const char *dir, plugin_registry_t *registry) {
    DIR *d = opendir(dir);
    if (!d) {
        TLOG_ERROR("Failed to open plugin directory: {}", dir);
        return -1;
    }
    
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        char manifest_path[512];
        snprintf(manifest_path, sizeof(manifest_path), 
                 "%s/%s/plugin.json", dir, entry->d_name);
        
        plugin_info_t info;
        if (plugin_parse_manifest(manifest_path, &info) == 0) {
            // 添加到注册表
            if (registry->count < registry->capacity) {
                registry->plugins[registry->count++] = info;
                TLOG_INFO("Discovered plugin: name={}, version={}", 
                          info.name, info.version);
            }
        }
    }
    
    closedir(d);
    return 0;
}
```

### 版本管理

**语义化版本（Semantic Versioning）**：
```
MAJOR.MINOR.PATCH
  1  .  2  .  0

MAJOR: 不兼容的 API 变更
MINOR: 向后兼容的功能新增
PATCH: 向后兼容的 bug 修复
```

**版本检查**：
```c
typedef struct {
    int major;
    int minor;
    int patch;
} version_t;

version_t version_parse(const char *version_str);
bool version_satisfies(version_t actual, const char *requirement);

// 使用
version_t plugin_ver = version_parse("1.2.0");
if (!version_satisfies(plugin_ver, ">=1.0.0,<2.0.0")) {
    TLOG_ERROR("Plugin version incompatible");
    return -1;
}
```

---

## 插件接口设计

### 标准插件接口

```c
// plugin_api.h - 主程序提供的稳定 ABI
#define PLUGIN_API_VERSION_MAJOR 1
#define PLUGIN_API_VERSION_MINOR 0

// 插件句柄（opaque）
typedef struct plugin_t plugin_t;

// 插件入口函数类型
typedef int (*plugin_init_fn)(plugin_t *plugin, void *host_api);
typedef int (*plugin_shutdown_fn)(plugin_t *plugin);
typedef void *(*plugin_get_interface_fn)(plugin_t *plugin, const char *interface_name);

// 插件元信息
typedef struct {
    const char *name;
    const char *version;
    const char *author;
    const char *description;
} plugin_metadata_t;

// 插件接口
typedef struct {
    int api_version_major;
    int api_version_minor;
    
    plugin_init_fn init;
    plugin_shutdown_fn shutdown;
    plugin_get_interface_fn get_interface;
    
    plugin_metadata_t metadata;
} plugin_interface_t;

// 插件必须导出此符号
#define PLUGIN_EXPORT __attribute__((visibility("default")))

// 标准入口函数
PLUGIN_EXPORT const plugin_interface_t *plugin_get_api(void);
```

### 插件实现示例

```c
// math_plugin.c
#include "plugin_api.h"

// 插件私有上下文
typedef struct {
    int init_count;
    void *host_api;
} math_plugin_ctx_t;

static math_plugin_ctx_t g_ctx;

// 初始化
static int math_plugin_init(plugin_t *plugin, void *host_api) {
    g_ctx.host_api = host_api;
    g_ctx.init_count++;
    
    // 注册服务、初始化资源等
    TLOG_INFO("Math plugin initialized");
    return 0;
}

// 关闭
static int math_plugin_shutdown(plugin_t *plugin) {
    // 清理资源
    TLOG_INFO("Math plugin shutdown");
    return 0;
}

// 数学服务接口
typedef struct {
    double (*add)(double a, double b);
    double (*multiply)(double a, double b);
} math_service_t;

static double add_impl(double a, double b) {
    return a + b;
}

static double multiply_impl(double a, double b) {
    return a * b;
}

static math_service_t g_math_service = {
    .add = add_impl,
    .multiply = multiply_impl
};

// 获取接口
static void *math_plugin_get_interface(plugin_t *plugin, const char *interface_name) {
    if (strcmp(interface_name, "math.service") == 0) {
        return &g_math_service;
    }
    return NULL;
}

// 导出插件 API
PLUGIN_EXPORT const plugin_interface_t *plugin_get_api(void) {
    static plugin_interface_t api = {
        .api_version_major = PLUGIN_API_VERSION_MAJOR,
        .api_version_minor = PLUGIN_API_VERSION_MINOR,
        .init = math_plugin_init,
        .shutdown = math_plugin_shutdown,
        .get_interface = math_plugin_get_interface,
        .metadata = {
            .name = "math_plugin",
            .version = "1.0.0",
            .author = "TurboScript Team",
            .description = "Mathematical functions"
        }
    };
    return &api;
}
```

---

## 插件加载与卸载

### 动态加载

```c
// plugin_loader.h
typedef struct host_library_t host_library_t;

typedef struct plugin_handle_t {
    host_library_t *library;   // 主程序通过 TurboNet util 适配层持有模块句柄
    plugin_interface_t *api;   // 插件 API
    plugin_info_t info;        // 插件元信息
    bool loaded;
    bool initialized;
} plugin_handle_t;

int plugin_load(const char *path, plugin_handle_t *handle);
int plugin_unload(plugin_handle_t *handle);
```


```c
// plugin_loader.c
// host_library_* 是主程序封装的动态库入口，底层平台差异由 TurboNet util 处理。

int plugin_load(const char *path, plugin_handle_t *handle) {
    // 1. 加载插件模块
    host_library_t *library = host_library_open(path);
    if (!library) {
        TLOG_ERROR("Failed to load plugin: path={}, error={}", 
                   path, host_library_last_error());
        return -1;
    }
    
    // 2. 查找入口函数
    typedef const plugin_interface_t *(*get_api_fn)(void);
    get_api_fn get_api = (get_api_fn)host_library_symbol(library, "plugin_get_api");
    if (!get_api) {
        TLOG_ERROR("Plugin missing plugin_get_api: path={}", path);
        host_library_close(library);
        return -1;
    }
    
    // 3. 获取插件 API
    const plugin_interface_t *api = get_api();
    if (!api) {
        TLOG_ERROR("plugin_get_api returned NULL: path={}", path);
        host_library_close(library);
        return -1;
    }
    
    // 4. 检查 API 版本
    if (api->api_version_major != PLUGIN_API_VERSION_MAJOR) {
        TLOG_ERROR("Plugin API version mismatch: expected {}.x, got {}.{}", 
                   PLUGIN_API_VERSION_MAJOR, 
                   api->api_version_major, api->api_version_minor);
        host_library_close(library);
        return -1;
    }
    
    // 5. 初始化句柄
    handle->library = library;
    handle->api = (plugin_interface_t*)api;
    handle->loaded = true;
    handle->initialized = false;
    
    TLOG_INFO("Plugin loaded: name={}, version={}", 
              api->metadata.name, api->metadata.version);
    return 0;
}

int plugin_unload(plugin_handle_t *handle) {
    if (!handle || !handle->loaded) {
        return -1;
    }

    const char *plugin_name = handle->api ? handle->api->metadata.name : "<unknown>";
    
    // 1. 确保已关闭
    if (handle->initialized && handle->api && handle->api->shutdown) {
        handle->api->shutdown(NULL);
        handle->initialized = false;
    }
    
    TLOG_INFO("Plugin unloaded: name={}", plugin_name);

    // 2. 释放模块句柄
    if (handle->library) {
        host_library_close(handle->library);
        handle->library = NULL;
    }
    
    handle->api = NULL;
    handle->loaded = false;
    return 0;
}
```

---

## 插件隔离机制

### 内存隔离

**规则**：
- 插件内分配的内存必须在插件内释放
- 禁止主程序 `free()` 插件分配的内存
- 禁止插件 `free()` 主程序分配的内存

**实现**：
```c
// host_api.h - 主程序提供给插件的 API
typedef struct {
    void *(*malloc)(size_t size);
    void (*free)(void *ptr);
    char *(*strdup)(const char *str);
} host_memory_api_t;

typedef struct {
    host_memory_api_t memory;
    // 其他主程序 API...
} host_api_t;

// 主程序实现
static void *host_malloc(size_t size) {
    return malloc(size);
}

static void host_free(void *ptr) {
    free(ptr);
}

static host_api_t g_host_api = {
    .memory = {
        .malloc = host_malloc,
        .free = host_free,
        .strdup = host_strdup
    }
};

// 插件使用主程序的内存 API
void plugin_function(host_api_t *host) {
    char *buf = host->memory.malloc(1024);
    // ...
    host->memory.free(buf);  // 使用主程序的 free
}
```

### 资源隔离

```c
// 插件文件句柄管理
typedef struct {
    turbo_file_t files[64];
    int count;
} plugin_file_manager_t;

turbo_file_t plugin_open_file(plugin_t *plugin, const char *path, int flags) {
    // 权限检查
    if (!plugin_has_permission(plugin, "file.read")) {
        TLOG_ERROR("Plugin lacks file.read permission");
        return TURBO_INVALID_FILE;
    }
    
    // 通过 TurboNet 文件系统封装打开文件
    turbo_file_t fd = turbo_fs_open(path, flags, 0);
    if (fd != TURBO_INVALID_FILE) {
        // 注册到插件文件管理器
        plugin_register_file(plugin, fd);
    }
    return fd;
}

void plugin_cleanup_resources(plugin_t *plugin) {
    // 自动关闭插件打开的所有文件
    plugin_file_manager_t *mgr = plugin_get_file_manager(plugin);
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->files[i] != TURBO_INVALID_FILE) {
            turbo_fs_close(mgr->files[i]);
            mgr->files[i] = TURBO_INVALID_FILE;
        }
    }
    mgr->count = 0;
}
```

### 权限隔离

```c
// 权限定义
typedef enum {
    PERM_FILE_READ    = 1 << 0,
    PERM_FILE_WRITE   = 1 << 1,
    PERM_NETWORK_HTTP = 1 << 2,
    PERM_NETWORK_RAW  = 1 << 3,
    PERM_EXEC         = 1 << 4
} plugin_permission_t;

typedef struct {
    uint32_t granted_permissions;  // 位掩码
} plugin_security_ctx_t;

bool plugin_has_permission(plugin_t *plugin, const char *perm_name) {
    plugin_security_ctx_t *ctx = plugin_get_security_ctx(plugin);
    
    uint32_t required = 0;
    if (strcmp(perm_name, "file.read") == 0) {
        required = PERM_FILE_READ;
    } else if (strcmp(perm_name, "file.write") == 0) {
        required = PERM_FILE_WRITE;
    } else if (strcmp(perm_name, "network.http") == 0) {
        required = PERM_NETWORK_HTTP;
    } else {
        return false;
    }
    
    return (ctx->granted_permissions & required) != 0;
}

// 从清单文件加载权限
void plugin_load_permissions(plugin_t *plugin, const char *manifest_path) {
    // 解析 manifest["permissions"]
    // 例如: ["file.read", "network.http"]
    
    plugin_security_ctx_t *ctx = plugin_get_security_ctx(plugin);
    ctx->granted_permissions = PERM_FILE_READ | PERM_NETWORK_HTTP;
    
    TLOG_INFO("Plugin permissions: {:b}", ctx->granted_permissions);
}
```

### 崩溃隔离

**方法 1：信号捕获（同进程）**
```c
#include <signal.h>
#include <setjmp.h>

static jmp_buf g_plugin_jmp_buf;
static bool g_in_plugin_call = false;

void plugin_signal_handler(int signum) {
    if (g_in_plugin_call) {
        TLOG_ERROR("Plugin crashed: signal={}", signum);
        longjmp(g_plugin_jmp_buf, 1);
    }
}

int safe_plugin_call(plugin_t *plugin, void (*fn)(void)) {
    signal(SIGSEGV, plugin_signal_handler);
    signal(SIGABRT, plugin_signal_handler);
    
    g_in_plugin_call = true;
    if (setjmp(g_plugin_jmp_buf) == 0) {
        // 正常执行
        fn();
        g_in_plugin_call = false;
        return 0;
    } else {
        // 捕获到崩溃
        g_in_plugin_call = false;
        plugin_mark_crashed(plugin);
        return -1;
    }
}
```

**方法 2：进程隔离（推荐高危插件）**
```c
// 插件运行在独立进程
pid_t plugin_spawn_process(plugin_t *plugin) {
    pid_t pid = fork();
    if (pid == 0) {
        // 子进程：加载并运行插件
        plugin_load_and_run(plugin);
        exit(0);
    } else if (pid > 0) {
        // 父进程：监控子进程
        plugin->process_pid = pid;
        return pid;
    } else {
        TLOG_ERROR("Failed to fork plugin process");
        return -1;
    }
}

void plugin_monitor_process(plugin_t *plugin) {
    int status;
    pid_t result = waitpid(plugin->process_pid, &status, WNOHANG);
    
    if (result > 0) {
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            TLOG_INFO("Plugin process exited: code={}", exit_code);
        } else if (WIFSIGNALED(status)) {
            int signal = WTERMSIG(status);
            TLOG_ERROR("Plugin process crashed: signal={}", signal);
            plugin_mark_crashed(plugin);
        }
    }
}
```

---

## 插件依赖管理

### 依赖声明与解析

```c
// plugin_dependency.h
typedef struct {
    char name[64];
    char version_requirement[32];  // 例如: ">=1.0.0,<2.0.0"
    bool optional;
} plugin_dependency_t;

typedef struct {
    plugin_dependency_t *deps;
    size_t count;
} plugin_dep_list_t;

/**
 * @brief 解析插件依赖
 * @param manifest_path 清单文件路径
 * @param deps 输出依赖列表
 * @return 0 成功，<0 失败
 */
int plugin_parse_dependencies(const char *manifest_path, plugin_dep_list_t *deps);

/**
 * @brief 检查依赖是否满足
 * @param registry 插件注册表
 * @param plugin 待检查插件
 * @return 0 满足，<0 不满足
 */
int plugin_check_dependencies(plugin_registry_t *registry, plugin_info_t *plugin);
```

### 拓扑排序与加载顺序

```c
// 拓扑排序确定加载顺序
int plugin_topological_sort(plugin_registry_t *registry, plugin_info_t **sorted) {
    // 1. 构建依赖图
    // 2. 计算入度
    // 3. Kahn 算法拓扑排序
    // 4. 检测循环依赖
    
    int *in_degree = calloc(registry->count, sizeof(int));
    bool **adj_matrix = calloc(registry->count, sizeof(bool*));
    
    for (size_t i = 0; i < registry->count; i++) {
        adj_matrix[i] = calloc(registry->count, sizeof(bool));
    }
    
    // 构建邻接矩阵和入度
    for (size_t i = 0; i < registry->count; i++) {
        plugin_dep_list_t deps;
        plugin_parse_dependencies(registry->plugins[i].manifest_path, &deps);
        
        for (size_t j = 0; j < deps.count; j++) {
            int dep_idx = find_plugin_index(registry, deps.deps[j].name);
            if (dep_idx >= 0) {
                adj_matrix[dep_idx][i] = true;
                in_degree[i]++;
            }
        }
    }
    
    // Kahn 算法
    int queue[MAX_PLUGINS];
    int queue_front = 0, queue_rear = 0;
    
    for (size_t i = 0; i < registry->count; i++) {
        if (in_degree[i] == 0) {
            queue[queue_rear++] = i;
        }
    }
    
    int sorted_count = 0;
    while (queue_front < queue_rear) {
        int u = queue[queue_front++];
        sorted[sorted_count++] = &registry->plugins[u];
        
        for (size_t v = 0; v < registry->count; v++) {
            if (adj_matrix[u][v]) {
                in_degree[v]--;
                if (in_degree[v] == 0) {
                    queue[queue_rear++] = v;
                }
            }
        }
    }
    
    // 检测循环依赖
    if (sorted_count != registry->count) {
        TLOG_ERROR("Circular dependency detected in plugins");
        free(in_degree);
        for (size_t i = 0; i < registry->count; i++) {
            free(adj_matrix[i]);
        }
        free(adj_matrix);
        return -1;
    }
    
    free(in_degree);
    for (size_t i = 0; i < registry->count; i++) {
        free(adj_matrix[i]);
    }
    free(adj_matrix);
    
    TLOG_INFO("Plugin load order resolved: {} plugins", sorted_count);
    return 0;
}
```

### 可选依赖处理

```c
int plugin_load_with_optional_deps(plugin_handle_t *handle, plugin_registry_t *registry) {
    plugin_dep_list_t deps;
    plugin_parse_dependencies(handle->info.manifest_path, &deps);
    
    for (size_t i = 0; i < deps.count; i++) {
        plugin_dependency_t *dep = &deps.deps[i];
        plugin_handle_t *dep_handle = find_loaded_plugin(registry, dep->name);
        
        if (!dep_handle) {
            if (dep->optional) {
                TLOG_WARN("Optional dependency missing: name={}, plugin={}",
                          dep->name, handle->info.name);
                continue;  // 可选依赖缺失，降级功能
            } else {
                TLOG_ERROR("Required dependency missing: name={}, plugin={}",
                           dep->name, handle->info.name);
                return -1;  // 必需依赖缺失，加载失败
            }
        }
        
        // 检查版本兼容性
        if (!version_satisfies(dep_handle->api->metadata.version, dep->version_requirement)) {
            TLOG_ERROR("Dependency version mismatch: plugin={}, dep={}, required={}, actual={}",
                       handle->info.name, dep->name, 
                       dep->version_requirement, dep_handle->api->metadata.version);
            return -1;
        }
    }
    
    return 0;
}
```

---

## 插件通信机制

### 事件总线（发布/订阅）

```c
// event_bus.h
typedef enum {
    EVENT_PRIORITY_HIGH = 0,
    EVENT_PRIORITY_NORMAL = 1,
    EVENT_PRIORITY_LOW = 2
} event_priority_t;

typedef struct {
    char type[64];
    void *data;
    size_t data_size;
    event_priority_t priority;
} event_t;

typedef void (*event_handler_fn)(const event_t *event, void *user_data);

typedef struct {
    event_handler_fn handler;
    void *user_data;
    char event_type[64];
    plugin_t *subscriber;
} event_subscription_t;

typedef struct {
    event_subscription_t *subscriptions;
    size_t count;
    size_t capacity;
    turbo_mutex_t lock;
} event_bus_t;

/**
 * @brief 创建事件总线
 */
event_bus_t *event_bus_create(void);

/**
 * @brief 订阅事件
 * @param bus 事件总线
 * @param event_type 事件类型（例如："config.changed"）
 * @param handler 事件处理函数
 * @param user_data 用户数据
 * @param subscriber 订阅者插件
 * @return 订阅 ID，失败返回 <0
 */
int event_bus_subscribe(event_bus_t *bus, const char *event_type, 
                        event_handler_fn handler, void *user_data, 
                        plugin_t *subscriber);

/**
 * @brief 发布事件（异步）
 * @param bus 事件总线
 * @param event 事件
 * @return 0 成功，<0 失败
 */
int event_bus_publish(event_bus_t *bus, const event_t *event);

/**
 * @brief 取消订阅
 */
int event_bus_unsubscribe(event_bus_t *bus, int subscription_id);
```

### 服务注册

```c
// service_registry.h
typedef void *service_interface_t;  // opaque

typedef struct {
    char service_name[64];
    service_interface_t *interface;
    plugin_t *provider;
} service_entry_t;

typedef struct {
    service_entry_t *services;
    size_t count;
    turbo_rwlock_t lock;
} service_registry_t;

/**
 * @brief 注册服务
 * @param registry 服务注册表
 * @param service_name 服务名（例如："db.query"）
 * @param interface 服务接口
 * @param provider 提供者插件
 * @return 0 成功，<0 失败（重复注册）
 */
int service_register(service_registry_t *registry, 
                     const char *service_name,
                     service_interface_t *interface,
                     plugin_t *provider);

/**
 * @brief 查找服务
 * @param registry 服务注册表
 * @param service_name 服务名
 * @return 服务接口，未找到返回 NULL
 */
service_interface_t *service_find(service_registry_t *registry, 
                                  const char *service_name);

/**
 * @brief 注销服务
 */
int service_unregister(service_registry_t *registry, const char *service_name);
```

### 共享数据（只读配置）

```c
// shared_config.h
typedef struct {
    char key[128];
    char value[512];
} config_entry_t;

typedef struct {
    config_entry_t *entries;
    size_t count;
    turbo_rwlock_t lock;  // 读多写少，使用读写锁
} shared_config_t;

/**
 * @brief 读取配置（线程安全）
 */
const char *shared_config_get(shared_config_t *config, const char *key);

/**
 * @brief 更新配置（只能由主程序调用）
 */
int shared_config_set(shared_config_t *config, const char *key, const char *value);
```

### 禁止直接调用规则

```c
// ❌ 错误：插件直接持有其他插件函数指针
// plugin_a.c
extern void plugin_b_function(void);  // 危险！

void plugin_a_call_b(void) {
    plugin_b_function();  // 直接调用，违反隔离原则
}

// ✅ 正确：通过主程序服务注册表间接调用
// plugin_a.c
void plugin_a_call_service(host_api_t *host) {
    service_interface_t *service = host->service_find(host, "plugin_b.service");
    if (service) {
        // 通过接口调用
        plugin_b_service_t *b_service = (plugin_b_service_t*)service;
        b_service->do_something();
    }
}
```

---

## 插件热重载

### 状态迁移

```c
// hot_reload.h
typedef struct {
    void *(*serialize_state)(plugin_t *plugin, size_t *size);
    int (*deserialize_state)(plugin_t *plugin, const void *data, size_t size);
} plugin_state_ops_t;

/**
 * @brief 热重载插件
 * @param old_handle 旧版本句柄
 * @param new_path 新版本库路径
 * @param new_handle 输出新版本句柄
 * @return 0 成功，<0 失败（回滚到旧版本）
 */
int plugin_hot_reload(plugin_handle_t *old_handle, 
                      const char *new_path,
                      plugin_handle_t *new_handle) {
    // 1. 序列化旧版本状态
    size_t state_size = 0;
    void *state = NULL;
    
    if (old_handle->api->get_interface) {
        plugin_state_ops_t *state_ops = old_handle->api->get_interface(
            NULL, "plugin.state_ops");
        
        if (state_ops && state_ops->serialize_state) {
            state = state_ops->serialize_state(NULL, &state_size);
            TLOG_INFO("Serialized plugin state: size={}", state_size);
        }
    }
    
    // 2. 加载新版本
    if (plugin_load(new_path, new_handle) != 0) {
        TLOG_ERROR("Failed to load new plugin version");
        if (state) free(state);
        return -1;
    }
    
    // 3. 初始化新版本
    if (new_handle->api->init && new_handle->api->init(NULL, NULL) != 0) {
        TLOG_ERROR("Failed to initialize new plugin version");
        plugin_unload(new_handle);
        if (state) free(state);
        return -1;
    }
    
    // 4. 反序列化状态到新版本
    if (state) {
        plugin_state_ops_t *new_state_ops = new_handle->api->get_interface(
            NULL, "plugin.state_ops");
        
        if (new_state_ops && new_state_ops->deserialize_state) {
            if (new_state_ops->deserialize_state(NULL, state, state_size) != 0) {
                TLOG_ERROR("Failed to migrate state to new plugin version");
                plugin_unload(new_handle);
                free(state);
                return -1;
            }
        }
        
        free(state);
    }
    
    // 5. 原子切换（等待旧版本引用清零）
    // 使用引用计数确保无活跃调用
    
    // 6. 卸载旧版本
    plugin_unload(old_handle);
    
    TLOG_INFO("Plugin hot reload successful: name={}", 
              new_handle->api->metadata.name);
    return 0;
}
```

### 引用计数管理

```c
// plugin_refcount.h
typedef struct {
    plugin_handle_t *handle;
    atomic_int refcount;  // C11 atomic
    bool marked_for_reload;
} plugin_ref_t;

/**
 * @brief 获取插件引用
 */
plugin_ref_t *plugin_acquire(plugin_registry_t *registry, const char *name) {
    plugin_ref_t *ref = find_plugin_ref(registry, name);
    if (ref) {
        atomic_fetch_add(&ref->refcount, 1);
        TLOG_DEBUG("Plugin acquired: name={}, refcount={}", 
                   name, atomic_load(&ref->refcount));
    }
    return ref;
}

/**
 * @brief 释放插件引用
 */
void plugin_release(plugin_ref_t *ref) {
    if (!ref) return;
    
    int old_count = atomic_fetch_sub(&ref->refcount, 1);
    TLOG_DEBUG("Plugin released: name={}, refcount={}", 
               ref->handle->api->metadata.name, old_count - 1);
    
    // 引用清零且标记重载，触发卸载
    if (old_count == 1 && ref->marked_for_reload) {
        TLOG_INFO("Plugin refcount zero, safe to reload: name={}",
                  ref->handle->api->metadata.name);
        // 触发重载流程...
    }
}

/**
 * @brief RAII 风格插件引用（配合宏）
 */
#define WITH_PLUGIN(registry, name) \
    for (plugin_ref_t *__ref = plugin_acquire(registry, name); \
         __ref; \
         plugin_release(__ref), __ref = NULL)

// 使用示例
WITH_PLUGIN(registry, "math_plugin") {
    service_interface_t *math_svc = __ref->handle->api->get_interface(
        NULL, "math.service");
    // 使用服务...
}  // 自动释放引用
```

### 原子切换策略

```c
// 策略 1：双版本路由（新旧并存）
typedef struct {
    plugin_handle_t *old_version;
    plugin_handle_t *new_version;
    atomic_int route_percentage;  // 0-100，新版本流量比例
} dual_version_router_t;

void gradual_switch(dual_version_router_t *router) {
    for (int pct = 0; pct <= 100; pct += 10) {
        atomic_store(&router->route_percentage, pct);
        TLOG_INFO("Routing {}% traffic to new version", pct);
        sleep(5);  // 观察 5 秒
        
        // 监控错误率、性能指标
        if (check_health_metrics() != 0) {
            TLOG_ERROR("New version unhealthy, rolling back");
            atomic_store(&router->route_percentage, 0);
            return;
        }
    }
    
    // 100% 切换完成，卸载旧版本
    plugin_unload(router->old_version);
    router->old_version = NULL;
}
```

### 回滚机制

```c
int plugin_reload_with_rollback(plugin_handle_t *old_handle, 
                                const char *new_path,
                                plugin_handle_t *new_handle) {
    // 1. 备份旧版本句柄
    plugin_handle_t backup = *old_handle;
    
    // 2. 尝试热重载
    if (plugin_hot_reload(old_handle, new_path, new_handle) != 0) {
        TLOG_WARN("Hot reload failed, keeping old version");
        *old_handle = backup;  // 恢复
        return -1;
    }
    
    // 3. 健康检查（超时 30 秒）
    for (int i = 0; i < 30; i++) {
        if (plugin_health_check(new_handle) == 0) {
            TLOG_INFO("New plugin version healthy");
            return 0;
        }
        sleep(1);
    }
    
    // 4. 健康检查失败，回滚
    TLOG_ERROR("New plugin version failed health check, rolling back");
    plugin_unload(new_handle);
    
    // 5. 重新加载旧版本
    if (plugin_load(backup.info.path, old_handle) == 0) {
        TLOG_INFO("Rolled back to old plugin version");
        return -1;
    } else {
        TLOG_CRITICAL("Rollback failed! Plugin unavailable");
        return -2;
    }
}
```

---

## 安全机制

### 签名验证

```c
// plugin_security.h
#include <openssl/sha.h>
#include <openssl/rsa.h>

/**
 * @brief 验证插件签名
 * @param plugin_path 插件库路径
 * @param signature 签名（Base64 编码）
 * @param public_key 公钥（PEM 格式）
 * @return 0 验证通过，<0 失败
 */
int plugin_verify_signature(const char *plugin_path, 
                            const char *signature,
                            const char *public_key) {
    // 1. 计算插件文件 SHA256
    turbo_fs_buf_t plugin_bytes = {0};
    unsigned char hash[SHA256_DIGEST_LENGTH];
    if (turbo_fs_read_file(plugin_path, &plugin_bytes) != 0) {
        TLOG_ERROR("Failed to read plugin file: path={}", plugin_path);
        return -1;
    }
    
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, plugin_bytes.base, plugin_bytes.len);
    SHA256_Final(hash, &sha256);
    turbo_fs_buf_free(&plugin_bytes);
    
    // 2. 使用公钥验证签名
    // （省略 RSA 验证代码，使用 OpenSSL API）
    
    TLOG_INFO("Plugin signature verified: path={}", plugin_path);
    return 0;
}

/**
 * @brief 从清单文件加载并验证签名
 */
int plugin_load_with_verification(const char *manifest_path, 
                                   const char *public_key,
                                   plugin_handle_t *handle) {
    // 1. 解析清单
    plugin_info_t info;
    if (plugin_parse_manifest(manifest_path, &info) != 0) {
        return -1;
    }
    
    // 2. 验证签名
    char signature[512];
    if (parse_signature_from_manifest(manifest_path, signature, sizeof(signature)) != 0) {
        TLOG_ERROR("Missing signature in manifest: path={}", manifest_path);
        return -1;
    }
    
    if (plugin_verify_signature(info.path, signature, public_key) != 0) {
        TLOG_ERROR("Signature verification failed: path={}", info.path);
        return -1;
    }
    
    // 3. 加载插件
    return plugin_load(info.path, handle);
}
```

### 沙箱机制（主程序适配层）

```c
typedef struct {
    bool allow_file_read;
    bool allow_file_write;
    bool allow_network;
    size_t memory_limit_bytes;
} plugin_sandbox_policy_t;

/**
 * @brief 启用插件沙箱；隔离细节由主程序通过 TurboNet util/coro 统一适配。
 */
int plugin_enable_sandbox(plugin_t *plugin, const plugin_sandbox_policy_t *policy) {
    if (!plugin || !policy) {
        return -1;
    }

    if (host_sandbox_apply(plugin_get_host(plugin), plugin, policy) != 0) {
        TLOG_ERROR("Failed to enable plugin sandbox: name={}",
                   plugin->handle->api->metadata.name);
        return -1;
    }

    TLOG_INFO("Plugin sandbox enabled: name={}",
              plugin->handle->api->metadata.name);
    return 0;
}
```

---

## 插件测试与调试

### 插件单元测试

```c
// test_plugin.c
#include "plugin_api.h"
#include <assert.h>

void test_plugin_load(void) {
    plugin_handle_t handle;
    int ret = plugin_load("./plugins/math_plugin.so", &handle);
    assert(ret == 0);
    assert(handle.loaded == true);
    assert(handle.api != NULL);
    
    plugin_unload(&handle);
}

void test_plugin_interface(void) {
    plugin_handle_t handle;
    plugin_load("./plugins/math_plugin.so", &handle);
    
    // 获取服务接口
    math_service_t *math = handle.api->get_interface(NULL, "math.service");
    assert(math != NULL);
    
    // 测试服务函数
    double result = math->add(1.0, 2.0);
    assert(result == 3.0);
    
    plugin_unload(&handle);
}

void test_plugin_dependencies(void) {
    plugin_registry_t registry;
    plugin_registry_init(&registry);
    
    // 加载依赖插件
    plugin_handle_t core_handle;
    plugin_load("./plugins/core.so", &core_handle);
    plugin_registry_add(&registry, &core_handle);
    
    // 加载依赖者插件
    plugin_handle_t math_handle;
    int ret = plugin_load_with_optional_deps("./plugins/math_plugin.so", 
                                             &math_handle, &registry);
    assert(ret == 0);
    
    plugin_registry_cleanup(&registry);
}

int main(void) {
    test_plugin_load();
    test_plugin_interface();
    test_plugin_dependencies();
    
    printf("All plugin tests passed!\n");
    return 0;
}
```

### 插件调试技巧

**1. 符号调试**
```bash
# 编译插件时保留符号
gcc -g -shared -fPIC math_plugin.c -o math_plugin.so

# GDB 调试
gdb ./host_program
(gdb) break plugin_load
(gdb) run
(gdb) info sharedlibrary  # 查看已加载插件
(gdb) break math_plugin.c:42  # 在插件代码设置断点
```

**2. 日志追踪**
```c
// 启用插件调试日志
#define PLUGIN_DEBUG_LOG 1

#if PLUGIN_DEBUG_LOG
#define PLUGIN_LOG(fmt, ...) \
    TLOG_DEBUG("[PLUGIN:{}] " fmt, __func__, ##__VA_ARGS__)
#else
#define PLUGIN_LOG(fmt, ...)
#endif

// 使用
PLUGIN_LOG("Loading plugin: path={}", path);
```

**3. 内存泄漏检测**
```bash
# Valgrind 检测插件内存泄漏
valgrind --leak-check=full --show-leak-kinds=all \
         --track-origins=yes \
         ./host_program
```

**4. 崩溃转储**
```c
// 启用 core dump
#include <sys/resource.h>

void enable_core_dump(void) {
    struct rlimit limit;
    limit.rlim_cur = RLIM_INFINITY;
    limit.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_CORE, &limit);
}

// 分析 core dump
// gdb ./host_program core
```

---

## 常见陷阱与最佳实践

### 常见陷阱

#### 1. 跨边界内存管理错误

```c
// ❌ 错误：主程序 free 插件分配的内存
char *plugin_get_string(plugin_t *p) {
    return strdup("hello");  // 插件内分配
}

// 主程序
char *str = plugin_get_string(p);
free(str);  // 危险！可能使用不同的堆

// ✅ 正确：使用主程序提供的分配器
char *plugin_get_string_safe(plugin_t *p, host_api_t *host) {
    return host->memory.strdup("hello");  // 使用主程序分配器
}

// 或者：插件提供释放函数
void plugin_free_string(char *str) {
    free(str);
}
```

#### 2. ABI 不兼容

```c
// ❌ 错误：跨边界传递复杂结构
typedef struct {
    std::vector<int> data;  // C++ 容器，ABI 不稳定
} complex_struct_t;

// ✅ 正确：使用纯 C 类型 + opaque 指针
typedef struct data_array_t data_array_t;  // opaque

data_array_t *data_array_create(void);
int data_array_get(data_array_t *arr, size_t index);
void data_array_destroy(data_array_t *arr);
```

#### 3. 循环依赖

```json
// ❌ 错误：插件 A 依赖 B，B 依赖 A
// plugin_a.json
{
  "dependencies": [{"name": "plugin_b"}]
}

// plugin_b.json
{
  "dependencies": [{"name": "plugin_a"}]
}

// ✅ 正确：提取公共依赖到 plugin_c
// plugin_a.json & plugin_b.json
{
  "dependencies": [{"name": "plugin_core"}]
}
```

#### 4. 全局状态竞争

```c
// ❌ 错误：插件使用全局变量
static int g_counter = 0;  // 多线程不安全

void plugin_function(void) {
    g_counter++;  // 数据竞争
}

// ✅ 正确：使用插件实例上下文 + 锁
typedef struct {
    atomic_int counter;  // C11 atomic
} plugin_ctx_t;

void plugin_function(plugin_t *plugin) {
    plugin_ctx_t *ctx = plugin_get_context(plugin);
    atomic_fetch_add(&ctx->counter, 1);
}
```

#### 5. 资源泄漏

```c
// ❌ 错误：插件卸载时未清理资源
int plugin_init(plugin_t *p, void *host_api) {
    plugin_ctx_t *ctx = plugin_get_context(p);
    ctx->trace_file = plugin_open_file(
        p, "trace.log", TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT);
    return 0;
}

int plugin_shutdown(plugin_t *p) {
    // 忘记关闭 trace_file
    return 0;
}

// ✅ 正确：确保资源清理
int plugin_shutdown_correct(plugin_t *p) {
    plugin_ctx_t *ctx = plugin_get_context(p);
    if (ctx->trace_file != TURBO_INVALID_FILE) {
        turbo_fs_close(ctx->trace_file);
        ctx->trace_file = TURBO_INVALID_FILE;
    }
    return 0;
}
```

### 最佳实践

#### 1. 版本化接口

```c
// 向后兼容的接口演进
// v1.0
typedef struct {
    void (*func_a)(void);
} plugin_interface_v1_t;

// v1.1（新增函数）
typedef struct {
    void (*func_a)(void);
    void (*func_b)(void);  // 新增
} plugin_interface_v1_1_t;

// 插件检查版本
void plugin_init(plugin_t *p, void *host_api) {
    host_api_t *api = (host_api_t*)host_api;
    
    if (api->version >= 0x0101) {  // 1.1+
        // 使用新 API
    } else {
        // 降级到旧 API
    }
}
```

#### 2. 错误恢复策略

```c
// 插件调用失败时的降级策略
service_interface_t *svc = service_find(registry, "optional_service");

if (svc) {
    // 尝试使用插件服务
    int ret = svc->process_data(data);
    if (ret != 0) {
        TLOG_WARN("Plugin service failed, using fallback: error={}", ret);
        // 降级到内置实现
        builtin_process_data(data);
    }
} else {
    // 服务不可用，使用内置实现
    builtin_process_data(data);
}
```

#### 3. 插件隔离边界清单

| 资源类型 | 隔离方式 | 检查方法 |
|---------|---------|---------|
| 内存 | 独立分配器 | Valgrind、AddressSanitizer |
| 文件句柄 | 注册表跟踪 | `lsof -p <pid>` |
| 线程 | 线程池隔离 | `ps -eLf` |
| 信号处理器 | 独立注册 | `cat /proc/<pid>/status` |
| 全局变量 | 禁止或原子操作 | ThreadSanitizer |
| 符号冲突 | RTLD_LOCAL | `nm -D plugin.so` |

#### 4. 插件审查检查清单

- [ ] **ABI 稳定性**：是否使用纯 C 接口？是否避免跨边界传递复杂结构？
- [ ] **内存安全**：是否使用主程序分配器？是否避免跨边界 free？
- [ ] **版本管理**：是否标注语义化版本？是否检查 API 版本兼容性？
- [ ] **依赖声明**：是否列出所有依赖？是否标注可选依赖？
- [ ] **权限控制**：是否声明所需权限？是否最小化权限？
- [ ] **资源清理**：shutdown 是否释放所有资源？是否测试重复加载/卸载？
- [ ] **线程安全**：是否保护共享状态？是否避免数据竞争？
- [ ] **错误处理**：是否返回明确错误码？是否避免崩溃？
- [ ] **日志记录**：是否记录关键操作？是否避免过量日志？
- [ ] **测试覆盖**：是否有单元测试？是否测试依赖缺失场景？

#### 5. 性能优化建议

```c
// 1. 延迟加载插件
typedef struct {
    char name[64];
    char path[256];
    plugin_handle_t *handle;  // 初始为 NULL
    bool loaded;
} lazy_plugin_t;

plugin_handle_t *lazy_load_plugin(lazy_plugin_t *lazy) {
    if (!lazy->loaded) {
        lazy->handle = malloc(sizeof(plugin_handle_t));
        plugin_load(lazy->path, lazy->handle);
        lazy->loaded = true;
    }
    return lazy->handle;
}

// 2. 插件热点函数缓存
typedef struct {
    void (*hot_function)(void);  // 缓存函数指针
    bool cached;
} plugin_cache_t;

void call_plugin_function(plugin_t *p, plugin_cache_t *cache) {
    if (!cache->cached) {
        cache->hot_function = p->api->get_interface(NULL, "hot_function");
        cache->cached = true;
    }
    
    if (cache->hot_function) {
        cache->hot_function();
    }
}

// 3. 批量调用减少边界开销
void plugin_process_batch(plugin_t *p, data_item_t *items, size_t count) {
    // 一次调用处理多个数据项，减少跨边界调用次数
}
```

---

## 完整示例：简单插件系统

### 主程序实现

```c
// main.c
#include "plugin_api.h"
#include "plugin_loader.h"
#include "service_registry.h"

int main(int argc, char **argv) {
    // 1. 初始化插件注册表
    plugin_registry_t registry;
    plugin_registry_init(&registry, 16);
    
    // 2. 发现插件
    if (plugin_discover("./plugins", &registry) != 0) {
        fprintf(stderr, "Failed to discover plugins\n");
        return 1;
    }
    
    // 3. 拓扑排序（解决依赖）
    plugin_info_t *sorted[MAX_PLUGINS];
    if (plugin_topological_sort(&registry, sorted) != 0) {
        fprintf(stderr, "Failed to resolve plugin dependencies\n");
        return 1;
    }
    
    // 4. 按顺序加载插件
    for (size_t i = 0; i < registry.count; i++) {
        plugin_handle_t handle;
        if (plugin_load(sorted[i]->path, &handle) == 0) {
            if (handle.api->init) {
                handle.api->init(NULL, &g_host_api);
            }
            plugin_registry_add_handle(&registry, &handle);
            printf("Loaded plugin: %s v%s\n", 
                   handle.api->metadata.name,
                   handle.api->metadata.version);
        }
    }
    
    // 5. 使用插件服务
    service_interface_t *math = service_find(&g_service_registry, "math.service");
    if (math) {
        math_service_t *ms = (math_service_t*)math;
        double result = ms->add(10.0, 20.0);
        printf("10 + 20 = %f\n", result);
    }
    
    // 6. 清理
    plugin_registry_shutdown(&registry);
    
    return 0;
}
```

---

## 参考资料

- **动态加载**：通过主程序的 TurboNet util 适配层统一封装模块加载、符号查找与句柄释放
- **版本管理**：[Semantic Versioning 2.0.0](https://semver.org/)
- **进程隔离**：通过主程序沙箱策略接口封装隔离能力，不在插件逻辑中直接调用平台 API
- **热重载**：[Martin Fowler - Plugin Pattern](https://martinfowler.com/eaaCatalog/plugin.html)
- **C 接口设计**：[How to Write Shared Libraries (Ulrich Drepper)](https://www.akkadia.org/drepper/dsohowto.pdf)

---

## 总结

本文档定义了 C 语言插件系统的完整规范，涵盖：

1. **架构设计**：生命周期、接口设计、隔离机制
2. **依赖管理**：拓扑排序、版本检查、可选依赖
3. **通信机制**：事件总线、服务注册、共享数据
4. **热重载**：状态迁移、引用计数、原子切换、回滚
5. **安全机制**：签名验证、权限隔离、沙箱
6. **测试调试**：单元测试、符号调试、内存检测
7. **最佳实践**：常见陷阱、审查清单、性能优化

遵循本规范可构建**稳定、安全、可扩展**的 C 语言插件系统。
