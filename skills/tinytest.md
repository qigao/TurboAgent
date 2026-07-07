# TinyTest 测试框架指南

## 适用场景

使用 TinyTest 编写 TurboScript/TurboNet 的 C/C++ 单元测试、回归测试、行为测试和小型 benchmark。

事实源：
- 头文件：`$TURBONET_ROOT/turbonet/tinytest/tinytest.h`
- CMake target：`TurboNet::TinyTest`
- 本地示例：`$TURBONET_ROOT/turbonet/tinytest/example.c`、`example.cpp`

## 基本原则

- 新测试优先写成单独可执行文件，直接 `#include "tinytest.h"`。
- 默认让 TinyTest 提供 `main()`；不要定义 `TINYTEST_NO_MAIN`，除非正在维护 TinyTest 框架本身。
- 每个 `it(...)` 只验证一个可描述行为；复杂 setup 放到 `before_each()`。
- 变量若需要在 `before_all()`、`before_each()` 和测试体之间共享，必须声明为 `static`。
- 断言优先使用具体 `check_*`，不要把复杂表达式塞进 `check(...)`。
- 临时调试可以用 `fit(...)` / `it_only(...)` 聚焦，提交前必须删除。

## CMake 接入

```cmake
add_executable(my_module_test test_my_module.c)
target_link_libraries(my_module_test PRIVATE TurboNet::TinyTest)
add_test(NAME my_module_test COMMAND my_module_test)
```

C++ 测试同样链接 `TurboNet::TinyTest`，并显式设置需要的 C++ 标准：

```cmake
add_executable(my_module_cpp_test test_my_module.cpp)
target_link_libraries(my_module_cpp_test PRIVATE TurboNet::TinyTest)
set_target_properties(my_module_cpp_test PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
add_test(NAME my_module_cpp_test COMMAND my_module_cpp_test)
```

## C 测试模板

```c
#include "tinytest.h"

suite("ring buffer") {
  static ring_buffer_t rb;

  before_each() {
    ring_buffer_init(&rb, 16);
  }

  after_each() {
    ring_buffer_destroy(&rb);
  }

  group("push") {
    it("stores one value") {
      check_int_eq(ring_buffer_push(&rb, 42), 0);
      check_size_eq(ring_buffer_size(&rb), 1);
    }

    it("rejects overflow") {
      for (int i = 0; i < 16; ++i) {
        check_int_eq(ring_buffer_push(&rb, i), 0);
      }
      check_int_ne(ring_buffer_push(&rb, 99), 0);
    }
  }
}
```

也可使用 BDD 命名：

```c
spec("import resolver") {
  given("an empty module cache") {
    static module_cache_t cache;

    before_each() {
      module_cache_init(&cache);
    }

    when("resolving a known module") {
      then("returns the cached entry") {
        module_cache_insert(&cache, "math", 7);
        check_int_eq(module_cache_find(&cache, "math"), 7);
      }
    }
  }
}
```

## C++ 测试模板

```cpp
#include "tinytest.h"

#include <stdexcept>
#include <string>
#include <vector>

suite("token parser") {
  it("splits comma separated input") {
    std::vector<std::string> actual = split_tokens("a,b,c", ',');
    std::vector<std::string> expected = {"a", "b", "c"};
    check_eq(actual, expected);
  }

  it("reports invalid delimiter") {
    check_throws_as(split_tokens("a,b", '\0'), std::invalid_argument);
    check_throws_with(split_tokens("a,b", '\0'), "delimiter");
  }
}
```

## 断言选择

通用：
- `check(expr)`：布尔断言，可带格式化说明。
- `check_warn(expr)`：非致命警告，适合一次收集多个弱条件。
- `info(...)` / `capture(var, fmt)`：给失败输出追加上下文。

C 常用：
- 整数：`check_int_eq/ne/gt/ge/lt/le`、`check_uint_eq/ne`、`check_long_eq`
- 大小：`check_size_eq/ne/gt/ge/lt/le`
- 浮点：`check_float_eq/ne`、`check_double_eq/ne`、`check_float_within_rel/abs`
- 字符串：`check_str_eq/ne/contains/starts_with/ends_with`
- 内存/指针：`check_mem_eq/ne`、`check_null`、`check_not_null`、`check_ptr_eq/ne`
- 数组：`check_int_array_eq`、`check_uint8_array_eq`、`check_size_array_eq`、`check_str_array_eq`
- 位与布尔：`check_true`、`check_false`、`check_bits`、`check_hex_eq`、`check_hex64_eq`

C++ 额外可用：
- 泛型：`check_equal`、`check_not_equal`、`check_greater`、`check_less`
- 容器：`check_eq`、`check_map_eq`、`check_contains`、`check_not_contains`
- 容器形状：`check_size`、`check_empty`、`check_not_empty`
- map key：`check_map_has_key`、`check_map_not_has_key`
- 字符串对象：`check_string_eq/ne/contains/starts_with/ends_with`
- 异常：`check_throws`、`check_throws_as`、`check_throws_with`、`check_nothrow`

断言参数顺序统一为 `actual, expected`。

## 临时文件

测试文件 I/O 时优先使用 TinyTest 自带 helper，避免手写平台路径和清理逻辑：

```c
it("round-trips a small file") {
  char *path = tt_make_temp_file("ts", ".txt");
  check_not_null(path);

  check_int_eq(tt_write_file(path, "hello", 5), 0);

  size_t n = 0;
  char *data = tt_read_file(path, &n);
  check_not_null(data);
  check_size_eq(n, 5);
  check_str_eq(data, "hello");

  free(data);
  check_int_eq(tt_remove_file(path), 0);
  free(path);
}
```

临时目录树用 `tt_make_temp_dir()` 和 `tt_remove_tree()`。

## 运行与筛选

TinyTest 可执行文件支持：

```text
--list, -l
--filter <pattern>, -f <pattern>
--tap
--junit <file>
--color
--no-color
--help, -h
```

常用验证：

```text
my_module_test --list
my_module_test --filter parser
my_module_test --junit my_module_test.junit.xml
my_module_test --tap
```

## Benchmark

只在需要对比小范围实现时使用 benchmark。setup 放在 timed loop 外，除非 setup 本身就是测量对象。

```c
suite("parser benchmark") {
  bench("parse expression") {
    const char *src = "1 + 2 * 3";

    benchmark("parse small expression", 10000, 1.0) {
      parse_expression(src);
    }
  }
}
```

benchmark 只能辅助判断性能变化，不能替代正确性断言。必要时在 benchmark 前后增加 `check_*` 验证结果。

## 反模式

- 不要在测试文件里自己写 `main()`；当前 `tinytest.h` 没有公开 `run_tests(...)` helper。
- 不要提交 `fit(...)` / `it_only(...)`。
- 不要在一个 `it(...)` 里塞多个无关行为。
- 不要用裸 `check(a == b)` 替代能给出更好失败信息的 `check_int_eq(a, b)`、`check_str_eq(a, b)` 或 `check_eq(a, b)`。
- 不要依赖测试执行顺序；每个 `it(...)` 都应能通过 fixture 独立准备状态。
- 不要把平台专用文件路径、线程或时间假设写进测试；优先用 TinyTest helper、TurboNet util/coro 和可注入时钟。

## 新增测试检查清单

1. 已链接 `TurboNet::TinyTest` 或能从项目 include path 找到 `tinytest.h`。
2. 测试文件没有自定义 `main()`。
3. 每个测试用例名称描述行为结果，不描述实现步骤。
4. fixture 状态通过 `before_each()` 重置。
5. 失败信息使用具体 typed assertion 或 `info(...)` 补足上下文。
6. 过滤运行和完整运行都能通过。
7. 若生成 JUnit/TAP，验证输出文件或格式符合调用方预期。
