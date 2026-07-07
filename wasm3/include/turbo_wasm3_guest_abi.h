#ifndef TURBO_WASM3_GUEST_ABI_H
#define TURBO_WASM3_GUEST_ABI_H

#if defined(__clang__) || defined(__GNUC__)
#define TURBONET_WASM_IMPORT(module, name)                                      \
  __attribute__((import_module(module), import_name(name)))
#define TURBONET_WASM_EXPORT(name) __attribute__((export_name(name)))
#else
#define TURBONET_WASM_IMPORT(module, name)
#define TURBONET_WASM_EXPORT(name)
#endif

#define TURBONET_IMPORT(name) TURBONET_WASM_IMPORT("TurboNet", name)
#define TURBONET_EXPORT(name) TURBONET_WASM_EXPORT(name)

#endif /* TURBO_WASM3_GUEST_ABI_H */
