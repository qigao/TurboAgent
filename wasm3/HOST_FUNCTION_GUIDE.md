# Turbo wasm3 Host Function Guide

此文只讲一事：如何在 `turbo_wasm3` 中开发、挂接、并给 guest 使用 host function。

## 先判其事

不是所有宿主能力都该做成 host function。

当用 host function：
- guest 需要碰宿主对象，如 `socket`、`db`、定时器、系统服务
- `WASI` 现成能力不够
- 你要给 guest 一套稳定 ABI，而不暴露底层库细节

不当用 host function：
- 只是普通文件、时钟、argv、preopen
  这些先走 `WASI`
- 只是仓内临时调试口
  不要把临时洞做成公开 ABI

一句话：
- `WASI` 管通用沙箱能力
- host function 管 TurboNet 自有能力

## 数据结构先行

host function 最易烂在“对象怎么穿过 wasm 边界”。

规则只有四条：

1. guest 只见整数、指针、长度、句柄
2. 不把宿主裸指针暴露给 guest
3. 宿主对象一律收进 registry 或 context
4. guest 与 backend 之间永远隔一层稳定 ABI

现有好例子：
- socket 走 `wasi fd` / socket registry
- db 走 `db_handle` / `stmt_handle`
- http 走 `client_handle` / `response_handle`

坏例子：
- 把 `sqlite3 *`、`coro_socket_t *` 直接丢给 guest

## ABI 设计准则

host function 签名要薄，要傻，要稳。

推荐形状：
- 标量输入：`i32` / `i64` / `f64`
- 缓冲输入：`ptr + len`
- 缓冲输出：`ptr + len + out_written`
- 资源对象：`handle`
- 结果状态：返回 `int32_t` 错误码

不要这样设计：
- 返回复杂结构体
- 一个函数做三件事
- 让 guest 猜 buffer ownership
- 把 backend 特有枚举直接透出

现有 `db` 模块之味道尚可：
- `db_open(ptr, len, out_handle) -> i32`
- `db_prepare(db, ptr, len, out_stmt) -> i32`
- `db_bind_text(stmt, index, ptr, len) -> i32`
- `db_column_text(stmt, index, ptr, len, out_written) -> i32`

此便是该学之范式。

## 开发流程

### 1. 先定 guest ABI

先写你要给 guest 的接口，不要先写 backend 细节。

例：
```c
import "TurboNet" "kv_get" : i32 (handle, key_ptr, key_len, val_ptr, val_len, out_written)
import "TurboNet" "kv_set" : i32 (handle, key_ptr, key_len, val_ptr, val_len)
```

问自己三件事：
- 对象是谁持有？要不要 `handle`？
- 输出是否可能变长？要不要 `out_written`？
- 错误信息是否要单独接口，如 `kv_error(handle, ...)`？

### 2. 在公开头中声明 ABI

改 [include/turbo_wasm3.h](include/turbo_wasm3.h) 或对应的 `include/turbo_wasm3_<module>.h`：
- 更新 host ABI 注释
- 若有新 registry / adapter，对外声明其 API
- 若 ABI 有破坏性变化，递增 `TURBO_WASM3_HOST_ABI_VERSION`

经验法则：
- 加新函数通常可不破 ABI
- 改旧函数签名、改旧语义，才算 ABI 破坏

### 3. 在对应模块源文件写 raw host wrapper

按职责放进 `src/` 下的对应实现文件，例如 `src/turbo_wasm3_db.c`、`src/turbo_wasm3_http.c`、`src/turbo_wasm3_redis.c` 或 `src/turbo_wasm3_parser.c`。

典型结构：
```c
m3ApiRawFunction(turbo_wasm3_host_kv_get) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *key = NULL;
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, key_data)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (key_len != 0) {
    m3ApiCheckMem(key_data, key_len);
  }
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }

  key = turbo_wasm3_copy_guest_bytes(key_data, key_len);
  if (!key) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = host_registry_kv_get(...);
  free(key);

  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}
```

要点：
- 先验 `vm`
- 先验 guest memory
- 输入缓冲若要长期用，先拷贝
- 输出长度先清零
- 最后只回错误码，不夹杂别的语义

### 4. 把 raw wrapper 接到 host linker

仍在对应模块源文件：
- 找 `turbo_wasm3_vm_link_host()`
- 用 `m3_LinkRawFunctionEx()` 接入

例：
```c
result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
    module, "TurboNet", "kv_get", "i(i*i*i*)", &turbo_wasm3_host_kv_get, vm));
```

规则：
- 模块名保持稳定，现用 `TurboNet`
- 签名字串与 raw function 参数必须一致
- `userdata` 传 `vm`，不要偷全局

### 5. 若有宿主对象，给它一层 registry

若你的功能会持有真实宿主对象，不要把 backend 写死在 raw wrapper 里。

应仿 `db registry`：
- `open/create` 返回 handle
- `close/release` 释放 handle
- 每步操作先 `find_entry(handle)`
- 错误收进 entry 的 `last_error`

此可避免：
- guest 持有宿主指针
- 生命周期散在各处
- backend 替换时 ABI 全碎

### 6. 补 guest 示例

最少要有一只 standalone guest wasm 示例，放在 [examples](examples)。

现有可学者：
- [guest_db_demo.c](examples/guest_db_demo.c)
- [guest_db_crud_demo.c](examples/guest_db_crud_demo.c)

guest 示例应：
- 只依赖 imports
- 不依赖宿主内部头
- 返回固定小整数作结果码

### 7. 补 host 示例

再补一只本机可执行示例，负责：
- 建 VM
- enable host linker
- load guest wasm
- call exported function
- 检查结果

现有可学者：
- [turbo_wasm3_db_example.c](examples/turbo_wasm3_db_example.c)
- [turbo_wasm3_db_crud_example.c](examples/turbo_wasm3_db_crud_example.c)

### 8. 最后补测试

测试比示例更重要。

至少应覆盖：
- registry 正常路径
- 错误路径
- guest 端端到端调用
- 旧 ABI 未受影响

现有主测文件：
- [test_turbo_wasm3.c](tests/test_turbo_wasm3.c)

## 错误处理准则

不要发明花哨异常模型。

统一规则：
- host function 返回 `0` 表成功
- 非 `0` 即 `turbo_error.h` 风格错误码
- 详细错误文案单独走 `*_error(handle, ptr, len, out_written)`

若错误归属于 statement，而非 db：
- 给它 `stmt_error`
- 不要把一切都塞回 `db_error`

这正是 [db_stmt_error](include/turbo_wasm3_db.h) 存在之理由。

## 内存与生命周期

最容易出事之处，只有这些：

- guest 传来的 `ptr` 不能长期持有
- 要长期用，就立刻复制
- guest 输出缓冲一定先做边界检查
- handle 关闭后，不能再被用
- 父对象关闭时，先收子对象

`db registry` 现成做法：
- 关 db 前先 finalize 所有 child statement
- `stmt_error` 与 `db_error` 各自收口

## 版本策略

别把 ABI 版本当装饰。

当前已有：
- `abi_version() -> i32`
- `TURBO_WASM3_HOST_ABI_VERSION`

规则建议如下：

不升版本：
- 新增可选 host function
- 新增新模块但不改旧语义

必须升版本：
- 改旧函数签名
- 改旧错误码语义
- 改旧 handle 生命周期规则
- 改旧枚举值含义

更好的做法：
- 先加新函数
- 旧函数保留一段时间
- 别为了“理论整洁”去破 guest

## 一只最小自定义 host module 例

若你不想改内建 `TurboNet` 模块，可自己加 linker：

```c
static
m3ApiRawFunction(my_host_add_one) {
  m3ApiReturnType(int32_t)
  m3ApiGetArg(int32_t, value)
  m3ApiReturn(value + 1);
}

static M3Result
my_linker(turbo_wasm3_vm_t *vm, IM3Module module, void *user_data) {
  (void)vm;
  (void)user_data;

  return m3_LinkRawFunctionEx(
      module, "academy", "add_one", "i(i)", &my_host_add_one, NULL);
}

turbo_wasm3_vm_add_host_linker(vm, my_linker, NULL);
```

guest 侧：
```c
__attribute__((import_module("academy"), import_name("add_one")))
extern int32_t academy_add_one(int32_t value);
```

此适合：
- 试验新能力
- 学术用途
- 暂不想把 ABI 并入 `TurboNet` 主模块

## 何时并入内建 `TurboNet` 模块

满足三条再并：
- 能力已稳定
- 测试已齐
- 你愿长期维护其 ABI

否则，先做自定义 linker。

这才是好味道：
- 试验功能在边上长
- 成熟后再并主 ABI

## 开发检查单

每次加新 host function，逐项自问：

- ABI 名称是否清楚？
- 是否只传标量、`ptr+len`、`handle`？
- 是否避免暴露 backend 指针？
- 是否有 registry/context 承接生命周期？
- 是否有 `*_error` 通路？
- 是否有 guest 示例？
- 是否有 host 示例？
- 是否有端到端测试？
- 是否破坏旧 guest？

若其中三项答不出，先别合。

## 现有参考文件

- 公开 API：[include/turbo_wasm3.h](include/turbo_wasm3.h) 与 `include/turbo_wasm3_<module>.h`
- Guest ABI 宏：[include/turbo_wasm3_guest_abi.h](include/turbo_wasm3_guest_abi.h)
- 实现模块：`src/turbo_wasm3_core.c`、`src/turbo_wasm3_socket.c`、`src/turbo_wasm3_db.c`、`src/turbo_wasm3_http.c`、`src/turbo_wasm3_redis.c`、`src/turbo_wasm3_parser.c`
- 最小 db 示例：[examples/guest_db_demo.c](examples/guest_db_demo.c)
- CRUD db 示例：[examples/guest_db_crud_demo.c](examples/guest_db_crud_demo.c)
- 集成测试：[tests/test_turbo_wasm3.c](tests/test_turbo_wasm3.c)

一句收尾：
- 先设计 handle 与 ABI
- 再写 raw wrapper
- 再接 linker
- 再补 example/test

次序若反了，后面多半是一地垃圾。

若你只要可抄之骨架，不想先读长文，直去
[HOST_FUNCTION_TEMPLATE.md](HOST_FUNCTION_TEMPLATE.md)。
