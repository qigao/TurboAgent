# C 语言设计模式实现指南

## 概述

本文档提供常用设计模式在纯 C 语言中的实现方法，适用于 TurboScript 等 C 项目。

**核心原则**：
- 使用 opaque 指针隐藏实现细节
- 使用函数指针表实现多态
- 使用结构体组合实现继承关系
- 避免过度抽象，优先简单直接的实现

---

## 创建型模式

### 工厂模式 (Factory Pattern)

**适用场景**：
- ✅ 对象创建逻辑复杂
- ✅ 需要延迟初始化
- ✅ 需要根据配置/参数选择实现
- ✅ 示例：Parser 创建、插件实例化、协议适配器

**实现方式**：工厂函数返回 opaque 指针

#### 简单工厂

```c
// parser.h - 公开接口
typedef struct parser_t parser_t;  // opaque 类型

parser_t *parser_create(const char *type);
void parser_destroy(parser_t *p);
int parser_parse(parser_t *p, const char *input);

// parser.c - 实现
struct parser_t {
    int (*parse_fn)(void *ctx, const char *input);
    void *context;
};

parser_t *parser_create(const char *type) {
    parser_t *p = malloc(sizeof(parser_t));
    if (strcmp(type, "json") == 0) {
        p->parse_fn = json_parse;
        p->context = json_parser_init();
    } else if (strcmp(type, "xml") == 0) {
        p->parse_fn = xml_parse;
        p->context = xml_parser_init();
    } else {
        free(p);
        return NULL;
    }
    return p;
}

void parser_destroy(parser_t *p) {
    if (p && p->context) {
        // 清理特定实现的上下文
        free(p->context);
    }
    free(p);
}

int parser_parse(parser_t *p, const char *input) {
    return p->parse_fn(p->context, input);
}
```


#### 注册表工厂

```c
// 允许运行时注册新类型
typedef parser_t *(*parser_factory_fn)(void);

typedef struct {
    const char *name;
    parser_factory_fn factory;
} parser_registry_entry_t;

static parser_registry_entry_t registry[16];
static int registry_count = 0;

void parser_register(const char *name, parser_factory_fn factory) {
    if (registry_count < 16) {
        registry[registry_count].name = name;
        registry[registry_count].factory = factory;
        registry_count++;
    }
}

parser_t *parser_create_by_name(const char *name) {
    for (int i = 0; i < registry_count; i++) {
        if (strcmp(registry[i].name, name) == 0) {
            return registry[i].factory();
        }
    }
    return NULL;
}

// 使用示例
parser_t *create_json_parser(void) {
    parser_t *p = malloc(sizeof(parser_t));
    // 初始化 JSON parser...
    return p;
}

// 启动时注册
void init_parsers(void) {
    parser_register("json", create_json_parser);
    parser_register("xml", create_xml_parser);
}
```

**优点**：
- 隐藏实现细节
- 集中创建逻辑
- 运行时可扩展（注册表模式）

**缺点**：
- 增加间接层
- 需要手动管理内存

---

### 建造者模式 (Builder Pattern)

**适用场景**：
- ✅ 对象构造参数 >5 个
- ✅ 需要分步构造
- ✅ 需要验证参数组合有效性
- ✅ 示例：配置对象、复杂查询、AST 构建

**实现方式**：builder 结构体 + setter 函数 + build 函数

```c
// config_builder.h
typedef struct config_builder_t config_builder_t;
typedef struct config_t config_t;

config_builder_t *config_builder_new(void);
void config_builder_set_port(config_builder_t *b, int port);
void config_builder_set_host(config_builder_t *b, const char *host);
void config_builder_set_timeout(config_builder_t *b, int timeout_ms);
void config_builder_set_max_connections(config_builder_t *b, int max_conn);

// 返回 Result 结构，包含错误信息
typedef struct {
    bool ok;
    union {
        config_t *config;
        const char *error;
    };
} config_result_t;

config_result_t config_builder_build(config_builder_t *b);
void config_builder_destroy(config_builder_t *b);
```


```c
// config_builder.c
struct config_builder_t {
    int port;
    char host[256];
    int timeout_ms;
    int max_connections;
    bool port_set;
    bool host_set;
};

struct config_t {
    int port;
    char host[256];
    int timeout_ms;
    int max_connections;
};

config_builder_t *config_builder_new(void) {
    config_builder_t *b = calloc(1, sizeof(config_builder_t));
    // 默认值
    b->timeout_ms = 30000;
    b->max_connections = 100;
    return b;
}

void config_builder_set_port(config_builder_t *b, int port) {
    b->port = port;
    b->port_set = true;
}

void config_builder_set_host(config_builder_t *b, const char *host) {
    strncpy(b->host, host, sizeof(b->host) - 1);
    b->host_set = true;
}

config_result_t config_builder_build(config_builder_t *b) {
    config_result_t result;
    
    // 验证必填字段
    if (!b->port_set) {
        result.ok = false;
        result.error = "Port is required";
        return result;
    }
    if (!b->host_set) {
        result.ok = false;
        result.error = "Host is required";
        return result;
    }
    
    // 验证范围
    if (b->port < 1 || b->port > 65535) {
        result.ok = false;
        result.error = "Port must be in range [1, 65535]";
        return result;
    }
    
    // 创建最终对象
    config_t *cfg = malloc(sizeof(config_t));
    cfg->port = b->port;
    strncpy(cfg->host, b->host, sizeof(cfg->host));
    cfg->timeout_ms = b->timeout_ms;
    cfg->max_connections = b->max_connections;
    
    result.ok = true;
    result.config = cfg;
    return result;
}

// 使用示例
config_builder_t *builder = config_builder_new();
config_builder_set_host(builder, "localhost");
config_builder_set_port(builder, 8080);
config_builder_set_timeout(builder, 5000);

config_result_t result = config_builder_build(builder);
if (result.ok) {
    config_t *cfg = result.config;
    // 使用 cfg...
    free(cfg);
} else {
    fprintf(stderr, "Config error: %s\n", result.error);
}
config_builder_destroy(builder);
```

**优点**：
- 参数验证集中
- 避免构造函数参数过多
- 可读性好

**缺点**：
- 代码量增加
- 需要额外内存（builder）

---

### 单例模式 (Singleton Pattern)

**适用场景**（慎用）：
- ✅ 全局唯一硬件资源（I/O 总线、设备控制器、插件注册表）
- ✅ 生命周期与程序一致
- ❌ **禁止**用于传递依赖
- ❌ **禁止**持有可变共享状态

**实现方式**：静态局部变量 + 初始化标志 + 互斥锁

```c
// io_bus.h
typedef struct io_bus io_bus_t;
io_bus_t *io_bus_get_instance(void);
int io_bus_read32(io_bus_t *bus, uint32_t address, uint32_t *value);

// io_bus.c
#include "turbo_thread.h"

struct io_bus {
    hw_io_device_t *device;  // 由硬件适配层持有具体平台资源
    turbo_mutex_t lock;
};

static io_bus_t *io_bus_instance = NULL;
static turbo_once_t io_bus_once = TURBO_ONCE_INIT;

static void io_bus_init_once(void) {
    io_bus_instance = malloc(sizeof(io_bus_t));
    io_bus_instance->device = hw_io_open_default_bus();
    turbo_mutex_init(&io_bus_instance->lock);
}

io_bus_t *io_bus_get_instance(void) {
    turbo_once(&io_bus_once, io_bus_init_once);
    return io_bus_instance;
}

// 使用
uint32_t status = 0;
io_bus_t *bus = io_bus_get_instance();
io_bus_read32(bus, SENSOR_STATUS_REGISTER, &status);
```

**懒汉式（延迟初始化）**：
```c
io_bus_t *io_bus_get_instance(void) {
    static io_bus_t *instance = NULL;
    static turbo_mutex_t lock;
    static bool lock_initialized = false;
    
    // 双重检查锁定
    if (instance == NULL) {
        if (!lock_initialized) {
            turbo_mutex_init(&lock);
            lock_initialized = true;
        }
        turbo_mutex_lock(&lock);
        if (instance == NULL) {
            instance = malloc(sizeof(io_bus_t));
            instance->device = hw_io_open_default_bus();
            turbo_mutex_init(&instance->lock);
        }
        turbo_mutex_unlock(&lock);
    }
    return instance;
}
```

**饿汉式（程序启动时初始化）**：
```c
static io_bus_t global_io_bus;
static bool initialized = false;

io_bus_t *io_bus_get_instance(void) {
    if (!initialized) {
        // 在 main() 开始时调用
        global_io_bus.device = hw_io_open_default_bus();
        turbo_mutex_init(&global_io_bus.lock);
        initialized = true;
    }
    return &global_io_bus;
}
```

**反模式警告**：
- ❌ 不要用单例传递依赖（应用依赖注入）
- ❌ 不要用单例作为全局可变状态（难以测试）
- ❌ 不要用服务定位器模式替代依赖注入

---

## 结构型模式

### 适配器模式 (Adapter Pattern)

**适用场景**：
- ✅ 集成第三方库
- ✅ 兼容旧接口
- ✅ 跨平台抽象
- ✅ 协议转换

**实现方式**：薄适配层，只做类型转换与错误映射

```c
// 假设要适配第三方 JSON 库 cJSON
// adapter.h - 统一接口
typedef struct json_t json_t;

json_t *json_parse(const char *text);
const char *json_get_string(json_t *json, const char *key);
int json_get_int(json_t *json, const char *key);
void json_free(json_t *json);
```


```c
// cjson_adapter.c - cJSON 适配器
#include "cJSON.h"

struct json_t {
    cJSON *impl;  // 包装第三方实现
};

json_t *json_parse(const char *text) {
    cJSON *cjson = cJSON_Parse(text);
    if (!cjson) return NULL;
    
    json_t *json = malloc(sizeof(json_t));
    json->impl = cjson;
    return json;
}

const char *json_get_string(json_t *json, const char *key) {
    cJSON *item = cJSON_GetObjectItem(json->impl, key);
    return item ? item->valuestring : NULL;
}

int json_get_int(json_t *json, const char *key) {
    cJSON *item = cJSON_GetObjectItem(json->impl, key);
    return item ? item->valueint : 0;
}

void json_free(json_t *json) {
    if (json) {
        cJSON_Delete(json->impl);
        free(json);
    }
}
```

**优点**：
- 隔离第三方库依赖
- 便于替换实现
- 统一错误处理

**最佳实践**：
- 适配层保持薄（thin adapter）
- 只做类型转换，不引入业务逻辑
- 错误码统一映射到项目标准

---

### 桥接模式 (Bridge Pattern)

**适用场景**：
- ✅ 抽象与实现独立变化
- ✅ 平台相关代码隔离
- ✅ 示例：渲染引擎、网络传输、存储后端

**实现方式**：抽象接口 + 实现接口，通过指针组合

```c
// storage.h - 抽象接口
typedef struct storage_t storage_t;
typedef struct storage_impl_t storage_impl_t;

struct storage_impl_t {
    int (*read)(void *ctx, const char *key, char *buf, size_t size);
    int (*write)(void *ctx, const char *key, const char *data, size_t size);
    int (*delete)(void *ctx, const char *key);
    void *context;
};

struct storage_t {
    storage_impl_t *impl;
};

storage_t *storage_create(storage_impl_t *impl);
int storage_read(storage_t *s, const char *key, char *buf, size_t size);
int storage_write(storage_t *s, const char *key, const char *data, size_t size);
int storage_delete(storage_t *s, const char *key);
void storage_destroy(storage_t *s);
```


```c
// storage.c
storage_t *storage_create(storage_impl_t *impl) {
    storage_t *s = malloc(sizeof(storage_t));
    s->impl = impl;
    return s;
}

int storage_read(storage_t *s, const char *key, char *buf, size_t size) {
    return s->impl->read(s->impl->context, key, buf, size);
}

int storage_write(storage_t *s, const char *key, const char *data, size_t size) {
    return s->impl->write(s->impl->context, key, data, size);
}

// file_storage_impl.c - 文件系统实现
typedef struct {
    char base_dir[256];
} file_storage_ctx_t;

static int file_read(void *ctx, const char *key, char *buf, size_t size) {
    file_storage_ctx_t *fs = (file_storage_ctx_t*)ctx;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", fs->base_dir, key);
    
    turbo_fs_buf_t fbuf;
    if (turbo_fs_read_file(path, &fbuf) != 0) return -1;
    
    size_t to_copy = fbuf.len < size ? fbuf.len : size;
    memcpy(buf, fbuf.base, to_copy);
    turbo_fs_buf_free(&fbuf);
    return to_copy;
}

static int file_write(void *ctx, const char *key, const char *data, size_t size) {
    file_storage_ctx_t *fs = (file_storage_ctx_t*)ctx;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", fs->base_dir, key);
    
    turbo_fs_buf_t fbuf = {.base = (char*)data, .len = size};
    return turbo_fs_write_file(path, &fbuf);
}

storage_impl_t *file_storage_impl_create(const char *base_dir) {
    file_storage_ctx_t *ctx = malloc(sizeof(file_storage_ctx_t));
    strncpy(ctx->base_dir, base_dir, sizeof(ctx->base_dir) - 1);
    
    storage_impl_t *impl = malloc(sizeof(storage_impl_t));
    impl->read = file_read;
    impl->write = file_write;
    impl->delete = file_delete;
    impl->context = ctx;
    return impl;
}

// 使用示例
storage_impl_t *file_impl = file_storage_impl_create("/data");
storage_t *storage = storage_create(file_impl);

char buf[1024];
storage_read(storage, "config.json", buf, sizeof(buf));
storage_write(storage, "state.dat", data, len);
```

**优点**：
- 抽象与实现解耦
- 易于扩展新实现（内存存储、数据库存储等）
- 运行时切换实现

---

### 组合模式 (Composite Pattern)

**适用场景**：
- ✅ 树形结构
- ✅ 递归处理
- ✅ 统一叶子与容器
- ✅ 示例：AST、表达式树、文件系统、UI 组件树

**实现方式**：统一接口、递归遍历

```c
// ast_node.h
typedef enum {
    AST_NUMBER,
    AST_BINARY_OP,
    AST_UNARY_OP
} ast_node_type_t;

typedef struct ast_node_t ast_node_t;

struct ast_node_t {
    ast_node_type_t type;
    union {
        double number;                    // 叶子节点
        struct {                          // 二元操作（容器节点）
            char op;
            ast_node_t *left;
            ast_node_t *right;
        } binary;
        struct {                          // 一元操作
            char op;
            ast_node_t *operand;
        } unary;
    } data;
};

// 统一接口
double ast_eval(ast_node_t *node);
void ast_print(ast_node_t *node);
void ast_free(ast_node_t *node);
```


```c
// ast_node.c
double ast_eval(ast_node_t *node) {
    switch (node->type) {
        case AST_NUMBER:
            return node->data.number;
        
        case AST_BINARY_OP: {
            double left = ast_eval(node->data.binary.left);
            double right = ast_eval(node->data.binary.right);
            switch (node->data.binary.op) {
                case '+': return left + right;
                case '-': return left - right;
                case '*': return left * right;
                case '/': return right != 0 ? left / right : 0;
                default: return 0;
            }
        }
        
        case AST_UNARY_OP: {
            double operand = ast_eval(node->data.unary.operand);
            switch (node->data.unary.op) {
                case '-': return -operand;
                case '+': return operand;
                default: return 0;
            }
        }
    }
    return 0;
}

void ast_print(ast_node_t *node) {
    switch (node->type) {
        case AST_NUMBER:
            printf("%.2f", node->data.number);
            break;
        case AST_BINARY_OP:
            printf("(");
            ast_print(node->data.binary.left);
            printf(" %c ", node->data.binary.op);
            ast_print(node->data.binary.right);
            printf(")");
            break;
        case AST_UNARY_OP:
            printf("(%c", node->data.unary.op);
            ast_print(node->data.unary.operand);
            printf(")");
            break;
    }
}

void ast_free(ast_node_t *node) {
    if (!node) return;
    
    switch (node->type) {
        case AST_NUMBER:
            break;  // 叶子节点无子节点
        case AST_BINARY_OP:
            ast_free(node->data.binary.left);
            ast_free(node->data.binary.right);
            break;
        case AST_UNARY_OP:
            ast_free(node->data.unary.operand);
            break;
    }
    free(node);
}
```

**优点**：
- 统一处理叶子和容器
- 递归遍历简洁
- 易于扩展新节点类型

---

### 装饰器模式 (Decorator Pattern)

**适用场景**：
- ✅ 动态增加职责
- ✅ 可选功能组合
- ✅ 避免继承爆炸
- ✅ 示例：日志装饰、缓存装饰、权限检查、性能监控

**实现方式**：包装同接口对象，转发核心调用，前后增加逻辑

```c
// stream.h - 基础接口
typedef struct stream_t stream_t;

struct stream_t {
    int (*read)(stream_t *s, char *buf, size_t size);
    int (*write)(stream_t *s, const char *data, size_t size);
    void (*close)(stream_t *s);
    void *context;
};

int stream_read(stream_t *s, char *buf, size_t size);
int stream_write(stream_t *s, const char *data, size_t size);
void stream_close(stream_t *s);
```


```c
// 基础设备流
typedef struct {
    hw_io_device_t *device;
} io_stream_ctx_t;

static int io_stream_read(stream_t *s, char *buf, size_t size) {
    io_stream_ctx_t *ctx = s->context;
    return hw_io_read(ctx->device, buf, size);
}

stream_t *io_stream_create(const char *device_name) {
    hw_io_device_t *device = hw_io_open(device_name);
    if (!device) return NULL;
    
    io_stream_ctx_t *ctx = malloc(sizeof(io_stream_ctx_t));
    ctx->device = device;
    
    stream_t *s = malloc(sizeof(stream_t));
    s->read = io_stream_read;
    s->write = io_stream_write;
    s->close = io_stream_close;
    s->context = ctx;
    return s;
}

// 日志装饰器
typedef struct {
    stream_t *wrapped;  // 被装饰的流
    const char *name;
} logging_stream_ctx_t;

static int logging_read(stream_t *s, char *buf, size_t size) {
    logging_stream_ctx_t *ctx = s->context;
    TLOG_DEBUG("Reading from {}, size={}", ctx->name, size);
    int result = stream_read(ctx->wrapped, buf, size);
    TLOG_DEBUG("Read {} bytes from {}", result, ctx->name);
    return result;
}

stream_t *logging_stream_wrap(stream_t *inner, const char *name) {
    logging_stream_ctx_t *ctx = malloc(sizeof(logging_stream_ctx_t));
    ctx->wrapped = inner;
    ctx->name = name;
    
    stream_t *s = malloc(sizeof(stream_t));
    s->read = logging_read;
    s->write = logging_write;
    s->close = logging_close;
    s->context = ctx;
    return s;
}

// 使用示例（链式装饰）
stream_t *base = file_stream_create("data.bin");
stream_t *logged = logging_stream_wrap(base, "data.bin");
stream_t *cached = cache_stream_wrap(logged, 4096);

char buf[1024];
stream_read(cached, buf, sizeof(buf));  // 经过：缓存 → 日志 → 文件
```

**优点**：
- 动态组合功能
- 符合开闭原则
- 职责单一

---

## 行为型模式

### 策略模式 (Strategy Pattern)

**适用场景**：
- ✅ 算法可互换
- ✅ 避免大量 if/switch
- ✅ 运行时选择策略
- ✅ 示例：排序算法、编码格式、压缩算法、路由策略

**实现方式**：策略接口 + 具体策略，上下文持有策略指针

```c
// sort_strategy.h
typedef int (*compare_fn)(const void *a, const void *b);

typedef struct sorter_t {
    void (*sort)(void *array, size_t n, size_t size, compare_fn cmp);
    const char *name;
} sorter_t;

// 快速排序策略
static void quick_sort_impl(void *array, size_t n, size_t size, compare_fn cmp) {
    qsort(array, n, size, cmp);
}

static sorter_t quick_sort_strategy = {
    .sort = quick_sort_impl,
    .name = "QuickSort"
};

// 归并排序策略
static void merge_sort_impl(void *array, size_t n, size_t size, compare_fn cmp) {
    // 实现归并排序...
}

static sorter_t merge_sort_strategy = {
    .sort = merge_sort_impl,
    .name = "MergeSort"
};

// 上下文
typedef struct {
    sorter_t *strategy;
} sort_context_t;

void sort_context_set_strategy(sort_context_t *ctx, sorter_t *strategy) {
    ctx->strategy = strategy;
}

void sort_context_execute(sort_context_t *ctx, void *array, size_t n, 
                          size_t size, compare_fn cmp) {
    TLOG_DEBUG("Using strategy: {}", ctx->strategy->name);
    ctx->strategy->sort(array, n, size, cmp);
}

// 使用示例
sort_context_t ctx;
if (n < 100) {
    sort_context_set_strategy(&ctx, &quick_sort_strategy);
} else {
    sort_context_set_strategy(&ctx, &merge_sort_strategy);
}
sort_context_execute(&ctx, array, n, sizeof(int), compare_int);
```


**优点**：
- 消除大量条件分支
- 算法独立变化
- 易于测试单个策略

---

### 观察者模式 (Observer Pattern)

**适用场景**：
- ✅ 一对多通知
- ✅ 事件驱动
- ✅ 松耦合通知
- ✅ 示例：事件系统、数据绑定、MVC、插件通知

**实现方式**：Subject + Observer 接口，弱引用避免循环

```c
// observer.h
typedef struct observer_t observer_t;
typedef struct subject_t subject_t;

// 观察者接口
typedef void (*observer_notify_fn)(observer_t *obs, void *event_data);

struct observer_t {
    observer_notify_fn notify;
    void *context;
};

// 主题
struct subject_t {
    observer_t *observers[32];
    int observer_count;
};

void subject_init(subject_t *s);
void subject_attach(subject_t *s, observer_t *obs);
void subject_detach(subject_t *s, observer_t *obs);
void subject_notify(subject_t *s, void *event_data);

// observer.c
void subject_init(subject_t *s) {
    s->observer_count = 0;
}

void subject_attach(subject_t *s, observer_t *obs) {
    if (s->observer_count < 32) {
        s->observers[s->observer_count++] = obs;
    }
}

void subject_detach(subject_t *s, observer_t *obs) {
    for (int i = 0; i < s->observer_count; i++) {
        if (s->observers[i] == obs) {
            // 移动后面的元素
            for (int j = i; j < s->observer_count - 1; j++) {
                s->observers[j] = s->observers[j + 1];
            }
            s->observer_count--;
            break;
        }
    }
}

void subject_notify(subject_t *s, void *event_data) {
    for (int i = 0; i < s->observer_count; i++) {
        s->observers[i]->notify(s->observers[i], event_data);
    }
}

// 使用示例
typedef struct {
    int temperature;
    subject_t subject;
} sensor_t;

void sensor_set_temperature(sensor_t *sensor, int temp) {
    sensor->temperature = temp;
    subject_notify(&sensor->subject, &temp);
}

// 观察者实现
typedef struct {
    const char *name;
} display_ctx_t;

void display_notify(observer_t *obs, void *event_data) {
    display_ctx_t *ctx = obs->context;
    int temp = *(int*)event_data;
    printf("%s: Temperature = %d\n", ctx->name, temp);
}

// 注册观察者
sensor_t sensor;
subject_init(&sensor.subject);

display_ctx_t display1_ctx = {.name = "Display1"};
observer_t display1 = {.notify = display_notify, .context = &display1_ctx};
subject_attach(&sensor.subject, &display1);

sensor_set_temperature(&sensor, 25);  // 通知所有观察者
```

**优点**：
- 松耦合
- 支持广播通信
- 动态订阅/取消订阅

**注意事项**：
- 使用弱引用避免循环引用
- 异步通知避免阻塞
- 观察者失效时及时 detach

---

### 命令模式 (Command Pattern)

**适用场景**：
- ✅ 操作对象化
- ✅ 支持撤销
- ✅ 事务日志
- ✅ 宏录制
- ✅ 示例：编辑器操作、数据库事务、RPC 调用、任务队列

**实现方式**：Command 接口（execute/undo）、命令队列、Memento 保存状态

```c
// command.h
typedef struct command_t command_t;

struct command_t {
    void (*execute)(command_t *cmd);
    void (*undo)(command_t *cmd);
    void *context;
};

void command_execute(command_t *cmd);
void command_undo(command_t *cmd);

// 命令历史
typedef struct {
    command_t *history[100];
    int count;
    int current;
} command_history_t;

void history_init(command_history_t *h);
void history_execute(command_history_t *h, command_t *cmd);
void history_undo(command_history_t *h);
void history_redo(command_history_t *h);
```


```c
// command.c
void history_init(command_history_t *h) {
    h->count = 0;
    h->current = -1;
}

void history_execute(command_history_t *h, command_t *cmd) {
    // 清除 redo 栈
    h->count = h->current + 1;
    
    if (h->count < 100) {
        h->history[h->count] = cmd;
        h->current = h->count;
        h->count++;
        command_execute(cmd);
    }
}

void history_undo(command_history_t *h) {
    if (h->current >= 0) {
        command_undo(h->history[h->current]);
        h->current--;
    }
}

void history_redo(command_history_t *h) {
    if (h->current < h->count - 1) {
        h->current++;
        command_execute(h->history[h->current]);
    }
}

// 具体命令：插入文本
typedef struct {
    char *buffer;      // 文档缓冲区
    int position;      // 插入位置
    char *text;        // 插入文本
    int text_len;
} insert_text_ctx_t;

void insert_text_execute(command_t *cmd) {
    insert_text_ctx_t *ctx = cmd->context;
    // 在 position 位置插入 text
    memmove(ctx->buffer + ctx->position + ctx->text_len,
            ctx->buffer + ctx->position,
            strlen(ctx->buffer + ctx->position) + 1);
    memcpy(ctx->buffer + ctx->position, ctx->text, ctx->text_len);
    TLOG_DEBUG("Insert '{}' at {}", ctx->text, ctx->position);
}

void insert_text_undo(command_t *cmd) {
    insert_text_ctx_t *ctx = cmd->context;
    // 删除插入的文本
    memmove(ctx->buffer + ctx->position,
            ctx->buffer + ctx->position + ctx->text_len,
            strlen(ctx->buffer + ctx->position + ctx->text_len) + 1);
    TLOG_DEBUG("Undo insert at {}", ctx->position);
}

command_t *insert_text_command_create(char *buffer, int pos, const char *text) {
    insert_text_ctx_t *ctx = malloc(sizeof(insert_text_ctx_t));
    ctx->buffer = buffer;
    ctx->position = pos;
    ctx->text_len = strlen(text);
    ctx->text = strdup(text);
    
    command_t *cmd = malloc(sizeof(command_t));
    cmd->execute = insert_text_execute;
    cmd->undo = insert_text_undo;
    cmd->context = ctx;
    return cmd;
}

// 使用示例
char buffer[1024] = "Hello";
command_history_t history;
history_init(&history);

command_t *cmd1 = insert_text_command_create(buffer, 5, " World");
history_execute(&history, cmd1);  // "Hello World"

command_t *cmd2 = insert_text_command_create(buffer, 11, "!");
history_execute(&history, cmd2);  // "Hello World!"

history_undo(&history);  // "Hello World"
history_undo(&history);  // "Hello"
history_redo(&history);  // "Hello World"
```

**优点**：
- 操作对象化，易于管理
- 支持撤销/重做
- 可记录操作历史

---

### 访问者模式 (Visitor Pattern)

**适用场景**：
- ✅ 对象结构稳定
- ✅ 操作频繁变化
- ✅ 双重分派
- ✅ 示例：AST 遍历、序列化、代码生成、优化 pass

**实现方式**：Visitor 接口 + Element.accept()

```c
// ast_visitor.h
typedef struct ast_visitor_t ast_visitor_t;
typedef struct ast_node_t ast_node_t;

// 访问者接口
struct ast_visitor_t {
    void (*visit_number)(ast_visitor_t *v, ast_node_t *node);
    void (*visit_binary_op)(ast_visitor_t *v, ast_node_t *node);
    void (*visit_unary_op)(ast_visitor_t *v, ast_node_t *node);
    void *context;
};

// AST 节点（添加 accept 方法）
void ast_accept(ast_node_t *node, ast_visitor_t *visitor);
```


```c
// ast_visitor.c
void ast_accept(ast_node_t *node, ast_visitor_t *visitor) {
    switch (node->type) {
        case AST_NUMBER:
            visitor->visit_number(visitor, node);
            break;
        case AST_BINARY_OP:
            // 先访问子节点
            ast_accept(node->data.binary.left, visitor);
            ast_accept(node->data.binary.right, visitor);
            visitor->visit_binary_op(visitor, node);
            break;
        case AST_UNARY_OP:
            ast_accept(node->data.unary.operand, visitor);
            visitor->visit_unary_op(visitor, node);
            break;
    }
}

// 具体访问者：打印
typedef struct {
    int indent;
} print_visitor_ctx_t;

void print_visit_number(ast_visitor_t *v, ast_node_t *node) {
    print_visitor_ctx_t *ctx = v->context;
    printf("%*sNumber: %.2f\n", ctx->indent, "", node->data.number);
}

void print_visit_binary_op(ast_visitor_t *v, ast_node_t *node) {
    print_visitor_ctx_t *ctx = v->context;
    printf("%*sBinary: %c\n", ctx->indent, "", node->data.binary.op);
}

ast_visitor_t *print_visitor_create(void) {
    print_visitor_ctx_t *ctx = malloc(sizeof(print_visitor_ctx_t));
    ctx->indent = 0;
    
    ast_visitor_t *v = malloc(sizeof(ast_visitor_t));
    v->visit_number = print_visit_number;
    v->visit_binary_op = print_visit_binary_op;
    v->visit_unary_op = print_visit_unary_op;
    v->context = ctx;
    return v;
}

// 具体访问者：代码生成
typedef struct {
    tstr_t code;
} codegen_visitor_ctx_t;

void codegen_visit_number(ast_visitor_t *v, ast_node_t *node) {
    codegen_visitor_ctx_t *ctx = v->context;
    ctx->code = tstr_cat_fmt(ctx->code, "%.2f", node->data.number);
}

void codegen_visit_binary_op(ast_visitor_t *v, ast_node_t *node) {
    codegen_visitor_ctx_t *ctx = v->context;
    ctx->code = tstr_cat_fmt(ctx->code, " %c ", node->data.binary.op);
}

// 使用示例
ast_node_t *root = build_ast();

// 打印 AST
ast_visitor_t *printer = print_visitor_create();
ast_accept(root, printer);

// 代码生成
ast_visitor_t *codegen = codegen_visitor_create();
ast_accept(root, codegen);
codegen_visitor_ctx_t *ctx = codegen->context;
printf("Generated code: %s\n", ctx->code);
```

**优点**：
- 操作与数据结构分离
- 易于增加新操作（不修改 AST 节点）
- 集中相关操作

**缺点**：
- 增加新节点类型需修改所有访问者
- 破坏封装性

---

### 模板方法模式 (Template Method Pattern)

**适用场景**：
- ✅ 算法骨架固定
- ✅ 子步骤可定制
- ✅ 示例：框架钩子、测试夹具、算法模板

**实现方式**（C 语言用函数指针模拟）：

```c
// template.h
typedef struct template_t {
    void (*step1)(void *ctx);
    void (*step2)(void *ctx);
    void (*step3)(void *ctx);
    void *context;
} template_t;

// 模板方法（固定流程）
void template_execute(template_t *t) {
    printf("--- Begin Template ---\n");
    if (t->step1) t->step1(t->context);
    if (t->step2) t->step2(t->context);
    if (t->step3) t->step3(t->context);
    printf("--- End Template ---\n");
}

// 具体实现 A
typedef struct { int value; } impl_a_ctx_t;

void impl_a_step1(void *ctx) {
    impl_a_ctx_t *c = ctx;
    printf("A: Step1, value=%d\n", c->value);
}

void impl_a_step2(void *ctx) {
    printf("A: Step2\n");
}

template_t *create_impl_a(int value) {
    impl_a_ctx_t *ctx = malloc(sizeof(impl_a_ctx_t));
    ctx->value = value;
    
    template_t *t = malloc(sizeof(template_t));
    t->step1 = impl_a_step1;
    t->step2 = impl_a_step2;
    t->step3 = NULL;  // 不实现 step3
    t->context = ctx;
    return t;
}

// 使用
template_t *ta = create_impl_a(42);
template_execute(ta);
```

---

## 反模式警告

### ❌ 禁止滥用的模式

#### 单例反模式
- **问题**：隐藏依赖、难以测试、全局可变状态
- **替代**：依赖注入、显式参数传递

```c
// ❌ 错误：用单例传递依赖
void process_data(void) {
    database_t *db = database_get_instance();  // 隐藏依赖
    db->query(db, "SELECT ...");
}

// ✅ 正确：显式传递依赖
void process_data(database_t *db) {
    db->query(db, "SELECT ...");
}
```

#### 服务定位器反模式
- **问题**：隐藏依赖、运行时失败
- **限制**：仅用于框架边界、插件系统

```c
// ❌ 错误：到处使用服务定位器
void business_logic(void) {
    io_bus_t *bus = service_locator_get("io_bus");
    storage_t *storage = service_locator_get("storage");
    // ...
}

// ✅ 正确：构造函数注入
typedef struct {
    io_bus_t *io_bus;
    storage_t *storage;
} business_logic_t;

business_logic_t *business_logic_create(io_bus_t *bus, storage_t *storage) {
    business_logic_t *bl = malloc(sizeof(business_logic_t));
    bl->io_bus = bus;
    bl->storage = storage;
    return bl;
}
```

#### 上帝对象反模式
- **问题**：承担 >5 种职责、难以维护
- **替代**：单一职责原则、模块化

```c
// ❌ 错误：上帝对象
typedef struct {
    // 混合了配置、设备 I/O、存储、网络、UI...
    config_t config;
    io_bus_t io_bus;
    storage_t storage;
    network_t net;
    ui_t ui;
} god_object_t;

// ✅ 正确：分离职责
typedef struct {
    config_manager_t *config;
    io_bus_t *io_bus;
    data_service_t *data;
} application_t;
```


#### 抽象工厂过度嵌套
- **问题**：≥3 层工厂嵌套、过度抽象
- **限制**：≤2 层工厂

```c
// ❌ 错误：过度抽象
factory_factory_t *ff = meta_factory_create();
factory_t *f = factory_factory_get(ff, "widget");
widget_t *w = factory_create(f, "button");

// ✅ 正确：简单工厂
widget_t *button = widget_create("button");
```

---

## C 语言设计模式最佳实践

### Opaque 指针（隐藏实现）

```c
// public.h - 只暴露类型声明
typedef struct my_obj_t my_obj_t;

my_obj_t *my_obj_create(void);
void my_obj_destroy(my_obj_t *obj);
int my_obj_do_something(my_obj_t *obj, int param);

// private.c - 实现细节
struct my_obj_t {
    int internal_state;
    void *private_data;
    // 用户不可见
};
```

**优点**：
- ABI 稳定（结构体变化不影响二进制兼容）
- 隐藏实现细节
- 强制使用公开 API

---

### 函数指针表（多态）

```c
// 接口定义
typedef struct renderer_ops_t {
    void (*draw_line)(void *ctx, int x1, int y1, int x2, int y2);
    void (*draw_circle)(void *ctx, int x, int y, int r);
    void (*clear)(void *ctx);
} renderer_ops_t;

typedef struct renderer_t {
    renderer_ops_t *ops;
    void *context;
} renderer_t;

// OpenGL 实现
void opengl_draw_line(void *ctx, int x1, int y1, int x2, int y2) {
    // OpenGL 调用...
}

renderer_ops_t opengl_ops = {
    .draw_line = opengl_draw_line,
    .draw_circle = opengl_draw_circle,
    .clear = opengl_clear
};

renderer_t *opengl_renderer_create(void) {
    renderer_t *r = malloc(sizeof(renderer_t));
    r->ops = &opengl_ops;
    r->context = opengl_ctx_create();
    return r;
}

// 软件实现
renderer_ops_t software_ops = {
    .draw_line = software_draw_line,
    .draw_circle = software_draw_circle,
    .clear = software_clear
};

// 统一调用
void render_scene(renderer_t *r) {
    r->ops->clear(r->context);
    r->ops->draw_line(r->context, 0, 0, 100, 100);
    r->ops->draw_circle(r->context, 50, 50, 25);
}
```

---

### 结构体组合（"继承"）

```c
// 基类
typedef struct {
    int id;
    const char *name;
} base_t;

// 派生类（第一个成员是基类）
typedef struct {
    base_t base;  // 必须是第一个成员
    int specific_field;
} derived_t;

// 向上转型（安全）
void process_base(base_t *b) {
    printf("ID=%d, Name=%s\n", b->id, b->name);
}

derived_t d = {{.id = 1, .name = "test"}, .specific_field = 42};
process_base((base_t*)&d);  // 向上转型
```

**原理**：C 保证结构体第一个成员地址与结构体地址相同。

---

### 错误处理（Result 模式）

```c
typedef struct {
    bool ok;
    union {
        int value;
        const char *error;
    };
} result_int_t;

result_int_t divide(int a, int b) {
    result_int_t result;
    if (b == 0) {
        result.ok = false;
        result.error = "Division by zero";
    } else {
        result.ok = true;
        result.value = a / b;
    }
    return result;
}

// 使用
result_int_t r = divide(10, 2);
if (r.ok) {
    printf("Result: %d\n", r.value);
} else {
    fprintf(stderr, "Error: %s\n", r.error);
}
```

---

### 资源管理（RAII 替代）

C 语言无 RAII，使用 `goto cleanup` 惯用法：

```c
int read_sensor_sample(const char *device_name) {
    hw_io_device_t *device = NULL;
    char *buffer = NULL;
    int result = 0;
    
    device = hw_io_open(device_name);
    if (!device) {
        result = -1;
        goto cleanup;
    }
    
    buffer = malloc(4096);
    if (!buffer) {
        result = -2;
        goto cleanup;
    }
    
    // 读取并处理设备数据...
    
cleanup:
    if (buffer) free(buffer);
    if (device) hw_io_close(device);
    return result;
}
```

---

## 设计原则总结

### SOLID 原则（C 语言应用）

1. **单一职责原则 (SRP)**
   - 一个模块/结构体只负责一项职责
   - 示例：`parser_t` 只解析，`validator_t` 只验证

2. **开闭原则 (OCP)**
   - 对扩展开放，对修改封闭
   - 示例：插件系统、策略模式、工厂模式

3. **里氏替换原则 (LSP)**
   - 子类型可替换基类型
   - 示例：统一接口（函数指针表）

4. **接口隔离原则 (ISP)**
   - 不依赖不使用的接口
   - 示例：拆分胖接口为多个细粒度接口

5. **依赖反转原则 (DIP)**
   - 依赖抽象，不依赖具体实现
   - 示例：`storage_t` 依赖 `storage_impl_t` 接口

---

## 模式选择指南

| 问题场景 | 推荐模式 | 替代方案 |
|---------|---------|---------|
| 创建逻辑复杂 | 工厂模式 | Builder 模式 |
| 参数 >5 个 | Builder 模式 | 配置结构体 |
| 全局唯一硬件资源 | 单例模式 | 显式 owner + init/shutdown |
| 适配第三方库 | 适配器模式 | 直接调用 |
| 平台相关代码 | 桥接模式 | 条件编译 |
| 树形结构 | 组合模式 | 递归遍历 |
| 动态增加职责 | 装饰器模式 | 函数组合 |
| 算法可互换 | 策略模式 | if/switch |
| 事件通知 | 观察者模式 | 回调函数 |
| 支持撤销 | 命令模式 | 状态快照 |
| AST 遍历 | 访问者模式 | 递归函数 |
| 固定流程 | 模板方法模式 | 函数指针数组 |

---

## 常见陷阱

### ❌ 避免

1. **过度设计**：小型项目不需要完整的工厂+策略+装饰器
2. **模式堆砌**：不要为了用模式而用模式
3. **忽略性能**：虚函数调用（函数指针）有开销
4. **内存泄漏**：opaque 指针必须提供 destroy 函数
5. **ABI 破坏**：公开头文件暴露结构体定义会破坏二进制兼容

### ✅ 最佳实践

1. **从简单开始**：先写直接实现，需要时再重构为模式
2. **明确意图**：模式名字体现设计意图（`parser_factory`、`logging_decorator`）
3. **文档化约束**：说明生命周期、所有权、线程安全性
4. **提供示例**：头文件注释中给出使用示例
5. **测试先行**：为每个模式实现编写单元测试

---

**最后更新**：2026-07-05  
**适用项目**：TurboScript 及所有 C 语言项目  
**参考资料**：《Design Patterns》、《C Interfaces and Implementations》
