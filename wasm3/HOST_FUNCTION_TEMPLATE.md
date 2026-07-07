# Turbo wasm3 Host Function Template

此文不讲哲学，只给可抄之骨架。

适用场景：
- 你要加一组新的 host imports
- 你要做一层 `handle` registry
- 你要给 guest 一只最小可跑示例

本文以 `academy.kv_*` 为假例。

## 1. 先定 guest imports

guest 先只看 ABI，不看 backend。

```c
__attribute__((import_module("academy"), import_name("kv_open")))
extern int32_t academy_kv_open(const char *target, uint32_t target_len,
                               uint32_t *out_handle);

__attribute__((import_module("academy"), import_name("kv_get")))
extern int32_t academy_kv_get(uint32_t handle, const char *key,
                              uint32_t key_len, char *buffer,
                              uint32_t buffer_size, uint32_t *out_written);

__attribute__((import_module("academy"), import_name("kv_set")))
extern int32_t academy_kv_set(uint32_t handle, const char *key,
                              uint32_t key_len, const char *value,
                              uint32_t value_len);

__attribute__((import_module("academy"), import_name("kv_error")))
extern int32_t academy_kv_error(uint32_t handle, char *buffer,
                                uint32_t buffer_size, uint32_t *out_written);

__attribute__((import_module("academy"), import_name("kv_close")))
extern int32_t academy_kv_close(uint32_t handle);
```

规则：
- 资源对象走 `handle`
- 文本与二进制都走 `ptr + len`
- 可变长输出都走 `out_written`
- 详细错误单独走 `*_error`

## 2. 公开头模板

若此模块要正式对外，往 [include/turbo_wasm3.h](include/turbo_wasm3.h) 或对应的 `include/turbo_wasm3_<module>.h` 加：

```c
typedef struct academy_kv_registry_s academy_kv_registry_t;

CXX_C_API academy_kv_registry_t *
academy_kv_registry_create(size_t initial_capacity);

CXX_C_API void
academy_kv_registry_destroy(academy_kv_registry_t *registry);

CXX_C_API int
academy_kv_registry_open(academy_kv_registry_t *registry,
                         const char *target, uint32_t *out_handle);

CXX_C_API int
academy_kv_registry_get(academy_kv_registry_t *registry, uint32_t handle,
                        const char *key, char *buffer, size_t buffer_size,
                        uint32_t *out_written);

CXX_C_API int
academy_kv_registry_set(academy_kv_registry_t *registry, uint32_t handle,
                        const char *key, const char *value, size_t value_len);

CXX_C_API int
academy_kv_registry_error(academy_kv_registry_t *registry, uint32_t handle,
                          char *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int
academy_kv_registry_close(academy_kv_registry_t *registry, uint32_t handle);

CXX_C_API M3Result
academy_kv_linker(turbo_wasm3_vm_t *vm, IM3Module module, void *user_data);
```

## 3. registry 数据结构模板

放在 `src/` 下的对应模块源文件；新增 host 模块时优先新建独立 `src/turbo_wasm3_<module>.c`，共享内部类型和 helper 通过 `src/turbo_wasm3_internal.h` 使用。

```c
typedef struct academy_kv_entry_s {
  uint32_t handle;
  void *backend_object;
  char *last_error;
} academy_kv_entry_t;

struct academy_kv_registry_s {
  uint32_t next_handle;
  size_t capacity;
  size_t count;
  academy_kv_entry_t *entries;
  void *user_data;
};
```

最小 helper：

```c
static academy_kv_entry_t *
academy_kv_find_entry(academy_kv_registry_t *registry, uint32_t handle);

static int
academy_kv_reserve(academy_kv_registry_t *registry, size_t required);

static void
academy_kv_clear_error(academy_kv_entry_t *entry);

static int
academy_kv_set_error(academy_kv_entry_t *entry, const char *message,
                     uint32_t message_len);
```

## 4. raw host wrapper 模板

### open

```c
m3ApiRawFunction(academy_host_kv_open) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *target = NULL;
  uint32_t handle = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArgMem(const uint8_t *, target_data)
  m3ApiGetArg(uint32_t, target_len)
  m3ApiGetArgMem(uint32_t *, out_handle)

  if (!vm || !out_handle) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_handle, sizeof(uint32_t));
  if (target_len != 0) {
    m3ApiCheckMem(target_data, target_len);
  }
  m3ApiWriteMem32(out_handle, 0);

  target = turbo_wasm3_copy_guest_bytes(target_data, target_len);
  if (!target) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = academy_kv_registry_open(vm->host_user_data, target, &handle);
  free(target);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_handle, handle);
  m3ApiReturn(0);
}
```

### get

```c
m3ApiRawFunction(academy_host_kv_get) {
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
  m3ApiWriteMem32(out_written, 0);

  key = turbo_wasm3_copy_guest_bytes(key_data, key_len);
  if (!key) {
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = academy_kv_registry_get(vm->host_user_data, handle, key, buffer,
                               (size_t)buffer_size, &written);
  free(key);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}
```

### set

```c
m3ApiRawFunction(academy_host_kv_set) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  char *key = NULL;
  char *value = NULL;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(const uint8_t *, key_data)
  m3ApiGetArg(uint32_t, key_len)
  m3ApiGetArgMem(const uint8_t *, value_data)
  m3ApiGetArg(uint32_t, value_len)

  if (!vm) {
    m3ApiReturn(TURBO_EINVAL);
  }

  if (key_len != 0) {
    m3ApiCheckMem(key_data, key_len);
  }
  if (value_len != 0) {
    m3ApiCheckMem(value_data, value_len);
  }

  key = turbo_wasm3_copy_guest_bytes(key_data, key_len);
  value = turbo_wasm3_copy_guest_bytes(value_data, value_len);
  if (!key || !value) {
    free(key);
    free(value);
    m3ApiReturn(TURBO_ENOMEM);
  }

  rc = academy_kv_registry_set(vm->host_user_data, handle, key, value, value_len);
  free(key);
  free(value);
  m3ApiReturn(rc);
}
```

### error

```c
m3ApiRawFunction(academy_host_kv_error) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);
  uint32_t written = 0;
  int rc;

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)
  m3ApiGetArgMem(char *, buffer)
  m3ApiGetArg(uint32_t, buffer_size)
  m3ApiGetArgMem(uint32_t *, out_written)

  if (!vm || !out_written) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiCheckMem(out_written, sizeof(uint32_t));
  if (buffer_size != 0) {
    m3ApiCheckMem(buffer, buffer_size);
  }
  m3ApiWriteMem32(out_written, 0);

  rc = academy_kv_registry_error(vm->host_user_data, handle, buffer,
                                 (size_t)buffer_size, &written);
  if (rc != 0) {
    m3ApiReturn(rc);
  }

  m3ApiWriteMem32(out_written, written);
  m3ApiReturn(0);
}
```

### close

```c
m3ApiRawFunction(academy_host_kv_close) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_import_vm(_ctx);

  m3ApiReturnType(int32_t)
  m3ApiGetArg(uint32_t, handle)

  if (!vm) {
    m3ApiReturn(TURBO_EINVAL);
  }

  m3ApiReturn(academy_kv_registry_close(vm->host_user_data, handle));
}
```

## 5. linker 模板

```c
M3Result academy_kv_linker(turbo_wasm3_vm_t *vm, IM3Module module, void *user_data) {
  M3Result result = m3Err_none;
  const char *mod = "academy";

  (void)user_data;

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "kv_open", "i(*i*)", &academy_host_kv_open, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "kv_get", "i(i*i*i*)", &academy_host_kv_get, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "kv_set", "i(i*i*i)", &academy_host_kv_set, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "kv_error", "i(i*i*)", &academy_host_kv_error, vm));
  if (result) {
    return result;
  }

  result = turbo_wasm3_suppress_lookup_failure(m3_LinkRawFunctionEx(
      module, mod, "kv_close", "i(i)", &academy_host_kv_close, vm));
  if (result) {
    return result;
  }

  return m3Err_none;
}
```

接入 VM：

```c
academy_kv_registry_t *registry = academy_kv_registry_create(8);
turbo_wasm3_vm_set_host_user_data(vm, registry);
turbo_wasm3_vm_add_host_linker(vm, academy_kv_linker, NULL);
```

若此 `user_data` 不止一物，勿偷懒塞全局。给它一层显式 state：

```c
typedef struct academy_host_state_s {
  academy_kv_registry_t *kv;
  void *other_service;
} academy_host_state_t;
```

## 6. registry 函数模板

### open

```c
int academy_kv_registry_open(academy_kv_registry_t *registry,
                             const char *target, uint32_t *out_handle) {
  void *backend = NULL;
  int rc;

  if (!registry || !target || !out_handle) {
    return TURBO_EINVAL;
  }

  rc = academy_kv_reserve(registry, registry->count + 1);
  if (rc != 0) {
    return rc;
  }

  rc = academy_backend_open(target, &backend);
  if (rc != 0) {
    return rc;
  }

  registry->entries[registry->count].handle = registry->next_handle++;
  registry->entries[registry->count].backend_object = backend;
  registry->entries[registry->count].last_error = NULL;
  *out_handle = registry->entries[registry->count].handle;
  registry->count++;
  return 0;
}
```

### close

```c
int academy_kv_registry_close(academy_kv_registry_t *registry, uint32_t handle) {
  academy_kv_entry_t *entry;
  size_t index;
  int rc;

  if (!registry) {
    return TURBO_EINVAL;
  }

  entry = academy_kv_find_entry(registry, handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  index = (size_t)(entry - registry->entries);
  rc = academy_backend_close(entry->backend_object);
  if (rc != 0) {
    return rc;
  }

  academy_kv_clear_error(entry);
  registry->entries[index] = registry->entries[registry->count - 1];
  registry->count--;
  return 0;
}
```

### get / set / error

```c
int academy_kv_registry_get(academy_kv_registry_t *registry, uint32_t handle,
                            const char *key, char *buffer, size_t buffer_size,
                            uint32_t *out_written) {
  academy_kv_entry_t *entry;
  int rc;

  if (!registry || !key || !buffer || buffer_size == 0 || !out_written) {
    return TURBO_EINVAL;
  }

  entry = academy_kv_find_entry(registry, handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  academy_kv_clear_error(entry);
  rc = academy_backend_get(entry->backend_object, key, buffer, buffer_size,
                           out_written);
  if (rc != 0) {
    academy_kv_set_error(entry, "backend get failed", 18);
  }
  return rc;
}
```

```c
int academy_kv_registry_set(academy_kv_registry_t *registry, uint32_t handle,
                            const char *key, const char *value, size_t value_len) {
  academy_kv_entry_t *entry;
  int rc;

  if (!registry || !key || (!value && value_len != 0)) {
    return TURBO_EINVAL;
  }

  entry = academy_kv_find_entry(registry, handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  academy_kv_clear_error(entry);
  rc = academy_backend_set(entry->backend_object, key, value, value_len);
  if (rc != 0) {
    academy_kv_set_error(entry, "backend set failed", 18);
  }
  return rc;
}
```

```c
int academy_kv_registry_error(academy_kv_registry_t *registry, uint32_t handle,
                              char *buffer, size_t buffer_size,
                              uint32_t *out_len) {
  academy_kv_entry_t *entry;
  size_t len;

  if (!registry || !buffer || buffer_size == 0 || !out_len) {
    return TURBO_EINVAL;
  }

  entry = academy_kv_find_entry(registry, handle);
  if (!entry) {
    return TURBO_EBADF;
  }

  if (!entry->last_error) {
    buffer[0] = '\0';
    *out_len = 0;
    return 0;
  }

  len = strlen(entry->last_error);
  if (len >= buffer_size) {
    len = buffer_size - 1;
  }
  memcpy(buffer, entry->last_error, len);
  buffer[len] = '\0';
  *out_len = (uint32_t)len;
  return 0;
}
```

## 7. guest 测试模板

guest wasm 最小例：

```c
__attribute__((export_name("run_kv_demo")))
int32_t run_kv_demo(void) {
  static const char target[] = "memory";
  static const char key[] = "name";
  static const char value[] = "TurboNet";
  uint32_t handle = 0;
  uint32_t written = 0;
  char buffer[32];
  int32_t rc;

  rc = academy_kv_open(target, 6, &handle);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = academy_kv_set(handle, key, 4, value, 8);
  if (rc != 0) {
    return 20 + rc;
  }

  rc = academy_kv_get(handle, key, 4, buffer, sizeof(buffer), &written);
  if (rc != 0) {
    return 30 + rc;
  }

  rc = academy_kv_close(handle);
  if (rc != 0) {
    return 40 + rc;
  }

  return written == 8 ? 7 : 99;
}
```

## 8. host 可执行模板

```c
int main(void) {
  turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64U * 1024U, NULL, 16);
  academy_kv_registry_t *registry = academy_kv_registry_create(8);
  IM3Module module = NULL;
  IM3Function run = NULL;
  int32_t result_value = 0;

  turbo_wasm3_vm_set_host_user_data(vm, registry);
  turbo_wasm3_vm_add_host_linker(vm, academy_kv_linker, NULL);
  turbo_wasm3_vm_load_module_file(vm, wasm_path, "kv_demo", &module);
  m3_FindFunction(&run, turbo_wasm3_vm_get_runtime(vm), "run_kv_demo");
  m3_CallV(run);
  m3_GetResultsV(run, &result_value);

  academy_kv_registry_destroy(registry);
  turbo_wasm3_vm_destroy(vm);
  return result_value == 7 ? 0 : 1;
}
```

## 9. 测试检查单

至少测：
- open 正常
- bad handle 返回 `TURBO_EBADF`
- bad buffer 返回 `TURBO_EINVAL`
- error 文案可读
- guest 端端到端可跑
- 旧模块不受影响

现成参考：
- [guest_db_demo.c](examples/guest_db_demo.c)
- [guest_db_crud_demo.c](examples/guest_db_crud_demo.c)
- [test_turbo_wasm3.c](tests/test_turbo_wasm3.c)

## 10. 最后一句

若你发现自己想把 backend 指针直接塞给 guest，停手。

先做 `handle + registry + error channel`。
这才是可维护之路。
