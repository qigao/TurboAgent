# turbo_wasm3

`turbo_wasm3` is the TurboNet integration layer for `wasm3`.

For host-function design and implementation, see
[HOST_FUNCTION_GUIDE.md](HOST_FUNCTION_GUIDE.md).
For copy-paste scaffolding, see
[HOST_FUNCTION_TEMPLATE.md](HOST_FUNCTION_TEMPLATE.md).

## Layout

- `vendor/wasm3`: upstream wasm3 source, tests, docs, platforms, and assets
- `vendor/wasm3/source`: interpreter core used by the `m3` target
- `wasm3`: TurboNet integration layer
- `wasm3/include`: public Turbo wasm3 API and guest ABI helper headers
- `wasm3/src`: Turbo wasm3 bridge implementation and private helpers
- `shared/utils`: filesystem, path, and clock primitives used by the simple WASI host
- `CoroNet`: coroutine socket implementation
- `wasm3/src/turbo_wasm3_socket.c`: the bridge that maps CoroNet sockets onto WASI socket calls

## Why this split

`wasm3` should not depend directly on `CoroNet`.

That would turn a vendor library into a project-specific fork. The bridge keeps the dependency direction clean:

- `m3` stays reusable
- `CoroNet` stays unaware of vendor layout
- TurboNet owns the integration policy in one place

## Minimal use

```c
#include "turbo_wasm3.h"

turbo_wasm3_vm_t *vm = turbo_wasm3_vm_create(64 * 1024, NULL, 16);
IM3Module module = NULL;

turbo_wasm3_vm_reset_preopens(vm);
turbo_wasm3_vm_set_preopen(vm, 3, "/", "C:/sandbox");
turbo_wasm3_vm_load_module_file(vm, "guest.wasm", "guest", &module);
turbo_wasm3_vm_set_wasi_args(vm, argc, argv);
turbo_wasm3_vm_register_socket(vm, socket, &wasi_fd);
```

The host still creates sockets. The guest only receives WASI file descriptor integers.
The VM wrapper owns a per-VM WASI context, copies wasm bytes, and keeps them alive for the lifetime of loaded modules.
Preopen directories are also per-VM and may be reset or replaced before module load.

## Host ABI

WASI is not enough for every host feature. `turbo_wasm3` now exposes a thin host-linker layer:

- add host linkers with `turbo_wasm3_vm_add_host_linker()`
- attach opaque host state with `turbo_wasm3_vm_set_host_user_data()`
- enable the built-in `TurboNet` imports with `turbo_wasm3_vm_enable_host()`

The built-in `TurboNet` module is intentionally small:

- `abi_version() -> i32`
- `clock_time_ms() -> i64`
- `socket_send(fd, ptr, len, out_sent) -> i32`
- `socket_recv(fd, ptr, len, out_recv) -> i32`
- `socket_release(fd) -> i32`
- `db_open(ptr, len, out_handle) -> i32`
- `db_close(handle) -> i32`
- `db_exec(handle, ptr, len, out_changes) -> i32`
- `db_error(handle, ptr, len, out_written) -> i32`
- `db_stmt_error(stmt, ptr, len, out_written) -> i32`
- `db_prepare(db, ptr, len, out_stmt) -> i32`
- `db_bind_i64(stmt, index, value) -> i32`
- `db_bind_f64(stmt, index, value) -> i32`
- `db_bind_null(stmt, index) -> i32`
- `db_bind_blob(stmt, index, ptr, len) -> i32`
- `db_bind_text(stmt, index, ptr, len) -> i32`
- `db_step(stmt, out_state) -> i32`
- `db_column_type(stmt, index, out_type) -> i32`
- `db_column_i64(stmt, index, out_value) -> i32`
- `db_column_f64(stmt, index, out_value) -> i32`
- `db_column_blob(stmt, index, ptr, len, out_written) -> i32`
- `db_column_text(stmt, index, ptr, len, out_written) -> i32`
- `db_reset(stmt) -> i32`
- `db_finalize(stmt) -> i32`

An optional HTTP linker extends the same `TurboNet` module with:

- `http_client_open(ptr, len, out_client) -> i32`
- `http_client_close(client) -> i32`
- `http_client_set_timeout(client, timeout_ms) -> i32`
- `http_request(client, method, url_ptr, url_len, body_ptr, body_len, out_resp) -> i32`
- `http_request_with_headers(client, method, url_ptr, url_len, headers_ptr, headers_len, body_ptr, body_len, out_resp) -> i32`
- `http_response_status(resp, out_status) -> i32`
- `http_response_error_code(resp, out_code) -> i32`
- `http_response_header(resp, name_ptr, name_len, ptr, len, out_written) -> i32`
- `http_response_headers(resp, ptr, len, out_written) -> i32`
- `http_response_body(resp, ptr, len, out_written) -> i32`
- `http_response_error(resp, ptr, len, out_written) -> i32`
- `http_response_close(resp) -> i32`

An optional Redis linker extends the same `TurboNet` module with:

- `redis_client_open(host_ptr, host_len, port, out_client) -> i32`
- `redis_client_close(client) -> i32`
- `redis_client_error(client, ptr, len, out_written) -> i32`
- `redis_command(client, argc, argv_ptr, argv_len_ptr, out_reply) -> i32`
- `redis_reply_close(reply) -> i32`
- `redis_reply_type(reply, out_type) -> i32`
- `redis_reply_i64(reply, out_value) -> i32`
- `redis_reply_text(reply, ptr, len, out_written) -> i32`
- `redis_reply_array_len(reply, out_len) -> i32`
- `redis_reply_array_at(reply, index, out_child_reply) -> i32`

This keeps guest code stable while leaving backend policy in TurboNet:

- filesystem and clocks still come from `shared/utils`
- socket transport still comes from `CoroNet`
- database handles now go through a shared DB registry; the built-in adapter uses `sqlite3`
- Redis commands now go through a shared reply registry backed by `tedis`
- future non-sqlite databases can still plug in through `turbo_wasm3_db_ops_t`

## Example and test

- `turbo_wasm3_example` runs `vendor/wasm3/test/wasi/simple/test.wasm` through the VM wrapper
- `turbo_wasm3_db_example` runs a standalone wasm guest that uses only `TurboNet.db_*` imports, including `double/blob/null` bindings and statement error reporting
- `turbo_wasm3_db_crud_example` runs a standalone wasm guest that performs create, read, update, and delete through the same host ABI
- `turbo_wasm3_http_example` runs a standalone wasm guest that exercises the optional HTTP host linker without needing external network success
- `test_turbo_wasm3` also verifies the optional Redis host linker against a local RESP mock server
- `test_turbo_wasm3` verifies exported calls plus per-VM WASI preopen isolation
- `test_turbo_wasm3` also verifies the shared SQLite-backed DB registry, the HTTP host registry, guest-side DB imports, guest-side HTTP imports, and end-to-end CRUD flow

Typical commands from the repo root:

```sh
cmake --build build/Ninja/Msvc --target turbo_wasm3_example turbo_wasm3_db_example test_turbo_wasm3
ctest --test-dir build/Ninja/Msvc --output-on-failure -R test_turbo_wasm3
build/Ninja/Msvc/bin/turbo_wasm3_example
build/Ninja/Msvc/bin/turbo_wasm3_db_example
```

## Current limits

- `sock_shutdown` for CoroNet-backed handles returns `NOSYS`
- the guest does not get socket creation or connect imports from this layer
- registered sockets are borrowed and must outlive the registry entry
- the built-in DB adapter supports `int64`, `double`, `text`, `blob`, and `null`, but still does not expose column names or streaming blob I/O
- the HTTP host layer now accepts newline-separated per-request headers and single-header lookup by name, but streaming callbacks, multipart, and response-header iteration are not bridged yet
- the Redis host layer currently exposes a small raw-command ABI; pub/sub and streams are not bridged yet
