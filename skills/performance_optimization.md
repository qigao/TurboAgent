# 性能优化专项指南

## 概述

本文档定义 C 语言项目的性能优化标准、热路径识别、内存管理优化、SIMD 使用和性能测试框架。

**核心原则**：
1. **测量优先**：profiling 证明瓶颈后再优化，不做过早优化
2. **量化标准**：所有优化必须有明确的性能指标和测试
3. **可维护性**：优化不得牺牲代码可读性和可维护性
4. **渐进式**：从简单优化开始，复杂优化需要充分理由

---

## 热路径识别与优化

### 热路径定义

**热路径**是指执行频率高或耗时占比大的代码路径，需要优先优化。

**识别标准**：
- 每秒执行次数 >1000 次
- 占总执行时间 >20%
- 以 profiling 工具实测为准（gprof、perf、VTune、Instruments）

**非热路径**不应过度优化，保持代码清晰优先。

### 热路径禁止事项

**严格禁止**以下操作（除非 profiling 证明无影响）：

1. **动态内存分配**
   - 禁止：`malloc`、`calloc`、`realloc`、`free`
   - 替代：预分配缓冲区、对象池、栈分配（小对象）、arena 分配器

2. **函数指针间接调用**
   - 禁止：通过函数指针表或虚表调用（除非内联）
   - 替代：直接调用、宏展开、模板展开

3. **字符串拷贝**
   - 禁止：`strcpy`、`strdup`、`strcat`
   - 替代：string view（只读引用）、预分配缓冲区、零拷贝

4. **复杂数学计算**
   - 禁止：`sqrt`、`sin`、`cos`、`log`（未优化版本）
   - 替代：查表法、多项式近似、miniblas 或 SIMDe 封装路径

5. **I/O 操作**
   - 禁止：裸标准 I/O；统一走 TurboNet 文件系统封装
   - 替代：批量 I/O、异步 I/O、内存映射文件

6. **锁操作**
   - 禁止：互斥锁、读写锁（高竞争场景）
   - 替代：无锁数据结构、原子操作、分段锁

### 热路径优化手段

**允许的优化手段**（按优先级排序）：

#### 1. 内联 (Inlining)
```c
// 小函数标记为 inline 或 static inline
static inline int add(int a, int b) {
    return a + b;
}

// 编译器提示
#define FORCE_INLINE __attribute__((always_inline)) inline
#define NEVER_INLINE __attribute__((noinline))
```

#### 2. 库级向量化
```c
// 数值线代优先复用 vendor/miniblas；底层向量化由 miniblas + SIMDe 处理
#include "linalg.h"

void multiply_matrices(const float *a, const float *b, float *c, int n) {
    // miniblas 使用列主序矩阵布局
    matmul("N", "N", n, n, n, 1.0f, a, b, 0.0f, c);
}
```

#### 3. 查表法 (Lookup Table)
```c
// 预计算常用值
static const float sin_table[360] = { /* 预计算 sin(0°) 到 sin(359°) */ };

float fast_sin_deg(int degrees) {
    return sin_table[degrees % 360];
}

// 位操作查表
static const uint8_t popcount_table[256] = { /* 预计算每个字节的 1 位数 */ };

int popcount_byte(uint8_t x) {
    return popcount_table[x];
}
```

#### 4. 分支预测提示
```c
// likely/unlikely 宏（GCC/Clang）
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

// 使用示例
if (unlikely(ptr == NULL)) {
    handle_error();  // 错误路径，不常执行
}

if (likely(cache_hit)) {
    return cached_value;  // 热路径
}
```

#### 5. 缓存预取 (Prefetch)
```c
// 预取下一次循环需要的数据
void process_list(node_t *head) {
    node_t *curr = head;
    
    while (curr) {
        // 预取下一个节点
        if (curr->next) {
            __builtin_prefetch(curr->next, 0, 3);  // 读取，高时间局部性
        }
        
        process_node(curr);
        curr = curr->next;
    }
}
```

#### 6. 循环展开 (Loop Unrolling)
```c
// 手动展开小循环
void zero_array(int *arr, size_t n) {
    size_t i = 0;
    
    // 每次处理 4 个元素
    for (; i + 4 <= n; i += 4) {
        arr[i]   = 0;
        arr[i+1] = 0;
        arr[i+2] = 0;
        arr[i+3] = 0;
    }
    
    // 处理剩余元素
    for (; i < n; i++) {
        arr[i] = 0;
    }
}
```

---

## 算法复杂度约束

### 复杂度标注

**所有非平凡算法**必须在函数注释中标注时间/空间复杂度：

```c
/**
 * @brief 二分查找
 * @param arr 已排序数组
 * @param n 数组长度
 * @param target 目标值
 * @return 目标索引，未找到返回 -1
 * @complexity 时间 O(log n)，空间 O(1)
 */
int binary_search(int *arr, size_t n, int target);

/**
 * @brief 快速排序
 * @complexity 时间 O(n log n) 平均，O(n²) 最坏；空间 O(log n) 递归栈
 */
void quick_sort(int *arr, size_t n);
```

### 高复杂度算法约束

**O(n²) 以上算法**必须说明：
- 数据规模上界（例如：n ≤ 1000）
- 分摊分析（例如：均摊 O(1)）
- 为何不能用更优算法

```c
/**
 * @brief 冒泡排序（仅用于教学）
 * @complexity 时间 O(n²)
 * @warning 生产环境禁用，仅限 n < 100 的测试代码
 */
void bubble_sort(int *arr, size_t n);
```

### 递归深度约束

**递归函数**必须有明确上界或尾递归优化：

```c
// ❌ 错误：无上界递归
int factorial(int n) {
    return n == 0 ? 1 : n * factorial(n - 1);  // 可能栈溢出
}

// ✅ 正确：限制递归深度
#define MAX_RECURSION_DEPTH 1000

int factorial_safe(int n, int depth) {
    if (depth > MAX_RECURSION_DEPTH) {
        return -1;  // 错误：超过最大深度
    }
    return n == 0 ? 1 : n * factorial_safe(n - 1, depth + 1);
}

// ✅ 更好：尾递归（编译器可优化为循环）
int factorial_tail(int n, int acc) {
    return n == 0 ? acc : factorial_tail(n - 1, n * acc);
}
```

---

## 内存管理优化

### 默认策略

**默认使用** `malloc`/`free` + 明确所有权 + 清晰生命周期文档。

### 批量分配：Arena Allocator

**适用场景**：
- 统一生命周期：请求级、事务级、会话级
- 批量分配后统一释放
- 避免碎片化

```c
// Arena 分配器（简化版）
typedef struct {
    char *buffer;
    size_t size;
    size_t offset;
} arena_t;

arena_t *arena_create(size_t size) {
    arena_t *arena = malloc(sizeof(arena_t));
    arena->buffer = malloc(size);
    arena->size = size;
    arena->offset = 0;
    return arena;
}

void *arena_alloc(arena_t *arena, size_t size) {
    if (arena->offset + size > arena->size) {
        return NULL;  // 超出容量
    }
    void *ptr = arena->buffer + arena->offset;
    arena->offset += size;
    return ptr;
}

void arena_reset(arena_t *arena) {
    arena->offset = 0;  // 重置，不释放内存
}

void arena_destroy(arena_t *arena) {
    free(arena->buffer);
    free(arena);
}

// 使用示例：解析器临时内存
void parse_request(request_t *req) {
    arena_t *arena = arena_create(4096);
    
    token_t *tokens = arena_alloc(arena, sizeof(token_t) * 100);
    ast_node_t *ast = arena_alloc(arena, sizeof(ast_node_t) * 50);
    
    // 解析逻辑...
    
    arena_destroy(arena);  // 统一释放
}
```

### 对象池 (Object Pool)

**适用场景**：
- 频繁创建销毁的小对象（<128 字节）
- 固定大小对象
- 需考虑缓存命中率

```c
// 对象池（简化版）
typedef struct pool_block_t {
    struct pool_block_t *next;
} pool_block_t;

typedef struct {
    void *memory;
    pool_block_t *free_list;
    size_t block_size;
    size_t capacity;
} object_pool_t;

object_pool_t *pool_create(size_t block_size, size_t capacity) {
    object_pool_t *pool = malloc(sizeof(object_pool_t));
    pool->memory = malloc(block_size * capacity);
    pool->block_size = block_size;
    pool->capacity = capacity;
    
    // 初始化空闲链表
    pool->free_list = NULL;
    for (size_t i = 0; i < capacity; i++) {
        pool_block_t *block = (pool_block_t*)((char*)pool->memory + i * block_size);
        block->next = pool->free_list;
        pool->free_list = block;
    }
    
    return pool;
}

void *pool_alloc(object_pool_t *pool) {
    if (!pool->free_list) {
        return NULL;  // 池满
    }
    
    pool_block_t *block = pool->free_list;
    pool->free_list = block->next;
    return block;
}

void pool_free(object_pool_t *pool, void *ptr) {
    pool_block_t *block = (pool_block_t*)ptr;
    block->next = pool->free_list;
    pool->free_list = block;
}

// 使用示例：AST 节点池
object_pool_t *ast_pool = pool_create(sizeof(ast_node_t), 1000);

ast_node_t *create_node() {
    return (ast_node_t*)pool_alloc(ast_pool);
}

void free_node(ast_node_t *node) {
    pool_free(ast_pool, node);
}
```

### 栈分配

**适用场景**：
- 小型固定大小对象（<1KB）
- 生命周期明确（函数作用域）
- 谨慎使用 VLA 或 `alloca`

```c
// ✅ 固定大小栈分配
void process_small_data(void) {
    char buffer[256];  // 栈分配，函数结束自动释放
    // ...
}

// ⚠️ VLA（可变长度数组）- C99
void process_dynamic(size_t n) {
    if (n > 1024) return;  // 限制大小
    
    int arr[n];  // VLA，谨慎使用
    // ...
}

// ❌ alloca - 不推荐（难以检测栈溢出）
void process_alloca(size_t n) {
    int *arr = alloca(n * sizeof(int));  // 危险！
}
```

### 缓存行对齐

**适用场景**：
- 多线程共享数据（避免伪共享）
- SIMD 数据对齐要求（16/32/64 字节）

```c
// 缓存行大小（通常 64 字节）
#define CACHE_LINE_SIZE 64

// 对齐声明使用 C11 stdalign.h，避免编译器私有属性散落在业务代码中
#include <stdalign.h>

// 避免伪共享
typedef struct {
    alignas(CACHE_LINE_SIZE) atomic_int counter;
    char padding[CACHE_LINE_SIZE - sizeof(atomic_int)];
} aligned_counter_t;

// 动态对齐分配必须通过 TurboNet 或项目内存适配层提供；
// 若当前没有统一入口，优先使用 SIMDe 的 loadu/storeu 处理未对齐输入。
```

### 数据局部性优化

**优化原则**：
- 结构体成员按访问频率排序（热字段放前面）
- 数组优于链表（连续内存）
- 避免指针追逐

```c
// ❌ 错误：冷字段在前
typedef struct {
    char *debug_name;      // 冷字段
    void *user_data;       // 冷字段
    int x, y;              // 热字段
    int width, height;     // 热字段
} bad_layout_t;

// ✅ 正确：热字段在前
typedef struct {
    int x, y;              // 热字段（缓存行前部）
    int width, height;     // 热字段
    void *user_data;       // 冷字段
    char *debug_name;      // 冷字段
} good_layout_t;
```

---

## SIMD 与向量化

### 适用场景

- 数值计算：向量运算、矩阵乘法
- 图像处理：像素批量操作
- 批量数据转换：编码、解码、校验和
- 数学库函数：`sin`、`cos`、`sqrt`（批量）

### SIMD 使用规范

**优先级**：
1. 线性代数、矩阵、向量和批量浮点运算优先使用 `vendor/miniblas` 的 `miniblas_linalg`。
2. 需要新增底层向量化时，使用 vcpkg 提供的 SIMDe 头文件，不直接写特定平台 intrinsic。
3. 只有 miniblas/SIMDe 无法表达且 profiling 证明收益明确时，才允许新增更底层实现；必须先说明接口、fallback 和验证范围。

仓库事实：
- `vendor/miniblas/CMakeLists.txt` 定义 `miniblas_linalg`。
- `vendor/miniblas` 发现 `simde/simde-common.h` 时定义 `MINIBLAS_USE_SIMDE`。
- `vcpkg.json` 已声明 `simde` 依赖。

```c
// ✅ 正确：矩阵乘法走 miniblas_linalg
#include "linalg.h"

void matmul_hot_path(const float *a, const float *b, float *c, int n) {
    matmul("N", "N", n, n, n, 1.0f, a, b, 0.0f, c);
}

// ✅ 允许：miniblas 无法覆盖的小型批量内核用 SIMDe
#include <simde/x86/sse.h>

void add_arrays_simde(const float *a, const float *b, float *out, size_t n) {
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        simde__m128 av = simde_mm_loadu_ps(a + i);
        simde__m128 bv = simde_mm_loadu_ps(b + i);
        simde_mm_storeu_ps(out + i, simde_mm_add_ps(av, bv));
    }
    for (; i < n; ++i) out[i] = a[i] + b[i];
}
```

### 数据对齐要求

**必须文档化对齐要求**，并优先选择能处理未对齐输入的库函数或 `loadu/storeu` 形式。

```c
/**
 * @brief 批量向量加法
 * @param a 输入数组 A，可未对齐
 * @param b 输入数组 B，可未对齐
 * @param out 输出数组，可未对齐
 * @param n 数组长度，尾部元素由标量路径处理
 * @requires SIMDe available through vcpkg
 */
void add_arrays_simde(const float *a, const float *b, float *out, size_t n);
```

### 依赖与构建

- CMake 中链接 `miniblas_linalg`，不要复制 miniblas 源码或手写同类矩阵内核。
- SIMDe 由 vcpkg 提供；新代码若直接包含 `simde/...`，必须通过现有构建系统传递 include path。
- 不在业务代码中散落 ISA 检测宏；能力选择集中在库或适配层。

---

## 缓存优化

### 缓存策略定义

**所有缓存**必须定义：
- 容量上限（例如：1000 条目，100MB）
- 失效策略（LRU、TTL、手动失效）
- 线程安全保证（锁、无锁、单线程）

```c
// LRU 缓存示例
typedef struct {
    hash_table_t *map;         // 键值映射
    list_t *lru_list;          // LRU 链表
    size_t capacity;           // 容量上限
    size_t current_size;       // 当前大小
    turbo_mutex_t lock;        // 线程安全
} lru_cache_t;

lru_cache_t *lru_cache_create(size_t capacity);
void *lru_cache_get(lru_cache_t *cache, const char *key);
void lru_cache_put(lru_cache_t *cache, const char *key, void *value);
void lru_cache_invalidate(lru_cache_t *cache, const char *key);
```

### 缓存命中率要求

**缓存命中率 <60%** 时重新评估必要性：
- 检查访问模式是否适合缓存
- 调整容量或失效策略
- 考虑移除缓存

```c
// 缓存统计
typedef struct {
    atomic_size_t hits;
    atomic_size_t misses;
} cache_stats_t;

double cache_hit_rate(cache_stats_t *stats) {
    size_t hits = atomic_load(&stats->hits);
    size_t misses = atomic_load(&stats->misses);
    return (double)hits / (hits + misses);
}
```

### 缓存预热策略

**预热必须异步且可配置**：

```c
// 异步预热
void cache_warmup_async(lru_cache_t *cache, const char **keys, size_t count) {
    turbo_thread_t thread;
    warmup_args_t *args = malloc(sizeof(warmup_args_t));
    args->cache = cache;
    args->keys = keys;
    args->count = count;
    
    turbo_thread_create(&thread, warmup_worker, args);
    turbo_thread_detach(thread);  // 后台运行
}
```

---

## 并发优化

### 无锁数据结构

**仅在 profiling 证明锁竞争是瓶颈后**考虑无锁：
- 必须提供带锁版本作为验证基准
- 必须处理 ABA 问题（hazard pointer、epoch-based reclamation）
- 限于简单结构：队列、栈、计数器

### 分段锁

**降低锁粒度**：

```c
#define NUM_SHARDS 16

typedef struct {
    hash_table_t *tables[NUM_SHARDS];
    turbo_mutex_t locks[NUM_SHARDS];
} sharded_hash_table_t;

size_t get_shard(const char *key) {
    return hash(key) % NUM_SHARDS;
}

void *sharded_get(sharded_hash_table_t *ht, const char *key) {
    size_t shard = get_shard(key);
    turbo_mutex_lock(&ht->locks[shard]);
    void *value = hash_table_get(ht->tables[shard], key);
    turbo_mutex_unlock(&ht->locks[shard]);
    return value;
}
```

---

## 性能测试框架

### Benchmark 要求

**性能关键模块**必须有 benchmark（使用  [tinytest.md](tinytest.md) 中的benchmark框架）：

```c
// benchmark 示例（伪代码）
BENCHMARK(BM_ArrayAdd) {
    float a[1000], b[1000], result[1000];
    init_arrays(a, b, 1000);
    
    BENCHMARK_LOOP {
        add_arrays(a, b, result, 1000);
    }
}

BENCHMARK(BM_Matmul_Miniblas) {
    float a[64 * 64], b[64 * 64], c[64 * 64];
    init_matrices_column_major(a, b, 64);
    
    BENCHMARK_LOOP {
        matmul("N", "N", 64, 64, 64, 1.0f, a, b, 0.0f, c);
    }
}
```

### Benchmark 覆盖范围

- 典型负载：常见场景、平均数据量
- 峰值负载：极端场景、最大数据量
- 边界条件：空输入、单元素、边界值

### 性能回归阈值

**性能退化超过以下阈值需要说明或回退**：
- 延迟：+10%
- 吞吐：-10%
- 内存：+20%

---

## 优化检查清单

### 编码前
- [ ] 使用 profiler 确认瓶颈（gprof、perf、VTune）
- [ ] 评估优化收益（预期提升 >30%）
- [ ] 确认优化不影响正确性

### 优化中
- [ ] 保留原始版本作为对照
- [ ] 编写性能测试（benchmark）
- [ ] 使用编译优化标志（`-O2`、`-O3`、`-march=native`）
- [ ] 检查编译器生成的汇编代码

### 优化后
- [ ] 运行功能测试（确保正确性）
- [ ] 运行性能测试（对比优化前后）
- [ ] 检查内存泄漏（Valgrind）
- [ ] 文档化优化理由与测量结果

---

## 常见陷阱

### 1. 过早优化
```c
// ❌ 错误：未测量就优化
void process_data(data_t *data) {
    // 复杂的手写向量化代码，但这个函数每秒只调用 1 次
}

// ✅ 正确：先测量，再优化
void process_data(data_t *data) {
    // 简单清晰的代码，函数不在热路径上
}
```

### 2. 忽略编译器优化
```c
// ❌ 错误：手写优化，编译器更优
int sum_array(int *arr, size_t n) {
    int sum = 0;
    for (size_t i = 0; i < n; i += 4) {  // 手动展开
        sum += arr[i] + arr[i+1] + arr[i+2] + arr[i+3];
    }
    return sum;
}

// ✅ 正确：让编译器自动向量化
int sum_array(int *arr, size_t n) {
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += arr[i];  // 编译器会自动 SIMD 优化
    }
    return sum;
}
```

### 3. 破坏数据局部性
```c
// ❌ 错误：AoS（Array of Structures）- 缓存不友好
typedef struct {
    float x, y, z;
    int id;
    char name[32];
} particle_t;

particle_t particles[1000];

void update_positions(void) {
    for (int i = 0; i < 1000; i++) {
        particles[i].x += 1.0f;  // 访问跨度大
    }
}

// ✅ 正确：SoA（Structure of Arrays）- 缓存友好
typedef struct {
    float *x;
    float *y;
    float *z;
    int *id;
    char (*name)[32];
} particle_system_t;

void update_positions(particle_system_t *ps, int count) {
    for (int i = 0; i < count; i++) {
        ps->x[i] += 1.0f;  // 连续访问
    }
}
```

---

## 实际案例

### 案例 1：解析器性能优化

**问题**：JSON 解析器占用 40% CPU 时间。

**分析**：
- Profiling 显示：70% 时间在字符串分配
- 每次解析分配 >100 次小字符串

**优化**：
- 使用 arena 分配器统一生命周期
- String view 替代字符串拷贝
- 预分配 token 缓冲区

**结果**：
- 解析速度提升 3.5x
- 内存分配次数降低 95%

### 案例 2：矩阵乘法库级向量化

**问题**：矩阵乘法性能不足。

**分析**：
- 标量版本：120 GFLOPS
- profiling 显示主要耗时集中在矩阵乘法内核
- 仓库已有 `vendor/miniblas`，并通过 `simde` 依赖提供可移植向量化路径

**优化**：
- 使用 `miniblas_linalg` 的 `matmul`
- 输入矩阵统一列主序布局，避免每次调用前后转置
- 若需新增更小粒度内核，优先在 miniblas/适配层中用 SIMDe 实现，不在业务代码手写 ISA 分支

**结果**：
- 性能提升以本地 benchmark 为准
- CI 保留标量正确性对照与库级路径回归测试

---

## 总结

性能优化的黄金法则：

1. **测量优先**：profiling 确认瓶颈，不做猜测优化
2. **从简单开始**：算法改进 > 数据结构 > 微观优化
3. **保持可维护性**：优化代码必须清晰注释，提供测试
4. **量化评估**：所有优化必须有 benchmark 证明有效
5. **持续监控**：CI 中运行性能测试，防止回归

**记住**：过早优化是万恶之源，但忽视性能也会导致严重问题。在正确的时机做正确的优化。
