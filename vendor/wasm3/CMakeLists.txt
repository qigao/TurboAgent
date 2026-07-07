cmake_minimum_required(VERSION 3.20)
project(wasm3_vendor LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(CmakeUtils)

# Detect WasiEnv
if(DEFINED ENV{WASI_CC} OR DEFINED WASI_SDK_PREFIX)
  set(WASIENV 1)
endif()

# Detect MinGW
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU")
  set(MINGW 1)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "set build type to Release")
endif()

if(WASIENV)
  set(BUILD_WASI "metawasi" CACHE STRING "WASI implementation")
elseif(EMSCRIPTEN OR EMSCRIPTEN_LIB)
  set(BUILD_WASI "none" CACHE STRING "WASI implementation")
else()
  set(BUILD_WASI "simple" CACHE STRING "WASI implementation")
endif()
set_property(CACHE BUILD_WASI PROPERTY STRINGS none simple metawasi)

option(WASM3_BUILD_NATIVE "Build wasm3 with machine-specific optimisations" OFF)
option(WASM3_BUILD_APP "Build the wasm3 CLI executable" OFF)

set(TURBO_WASM3_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(OUT_FILE "wasm3")

if(NOT APP_DIR)
  set(APP_DIR "${TURBO_WASM3_VENDOR_DIR}/platforms/app")
endif()

if(CLANG OR CLANG_SUFFIX)
  set(CMAKE_C_COMPILER "clang${CLANG_SUFFIX}")
  set(CMAKE_CXX_COMPILER "clang++${CLANG_SUFFIX}")

  if(BUILD_FUZZ)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")
    set(OUT_FILE "wasm3-fuzzer")
    set(APP_DIR "${TURBO_WASM3_VENDOR_DIR}/platforms/app_fuzz")
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "set build type to Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=fuzzer,address")
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=fuzzer,address")
  endif()
endif()

if(WASIENV)
  set(OUT_FILE "wasm3.wasm")
endif()

if(CLANG_CL)
  set(CMAKE_C_COMPILER "clang-cl")
  set(CMAKE_CXX_COMPILER "clang-cl")
  set(CMAKE_LINKER "lld-link")
endif()

if(EMSCRIPTEN OR EMSCRIPTEN_LIB)
  set(CMAKE_C_COMPILER "emcc")
  set(CMAKE_CXX_COMPILER "em++")

  if(EMSCRIPTEN_LIB)
    set(APP_DIR "${TURBO_WASM3_VENDOR_DIR}/platforms/emscripten_lib")
    set(OUT_FILE "wasm3.wasm")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -s STANDALONE_WASM")
  else()
    set(APP_DIR "${TURBO_WASM3_VENDOR_DIR}/platforms/emscripten")
    set(OUT_FILE "wasm3.html")
  endif()
endif()

if(BUILD_32BIT)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS}   -m32")
endif()

message("----")
message("Generator:  ${CMAKE_GENERATOR}")
message("Compiler:   ${CMAKE_C_COMPILER_ID}")
message("Build Type: ${CMAKE_BUILD_TYPE}")

include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

file(GLOB app_srcs "${APP_DIR}/*.c")
if(WASM3_BUILD_APP)
  add_executable(${OUT_FILE} ${app_srcs})
  set_target_properties(${OUT_FILE} PROPERTIES FOLDER "wasm3")
  target_compile_features(${OUT_FILE} PRIVATE c_std_11)
endif()

set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG=1")

if(EMSCRIPTEN OR EMSCRIPTEN_LIB)

  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -s GLOBAL_BASE=1024 -s TOTAL_STACK=2MB -s INITIAL_MEMORY=4MB -s ALLOW_MEMORY_GROWTH"
  )
  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -s EXPORTED_FUNCTIONS=\"[\\\"_malloc\\\",\\\"_free\\\",\\\"_main\\\"]\""
  )

  set(CMAKE_C_FLAGS_RELEASE
      "${CMAKE_C_FLAGS_RELEASE} -O3 -flto -Wfatal-errors -s ASSERTIONS=0")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE
      "${CMAKE_C_FLAGS_RELEASE} --strip-all --gc-sections")

  if(WASM_EXT)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -mbulk-memory -mnontrapping-fptoint -msign-ext -mtail-call"
    )
  endif()

elseif(WASIENV)

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Dd_m3HasTracer")
  set(CMAKE_C_FLAGS_RELEASE
      "${CMAKE_C_FLAGS_RELEASE} -O3 -Wfatal-errors -fomit-frame-pointer")

  if(WASM_EXT)
    set(CMAKE_C_FLAGS
        "${CMAKE_C_FLAGS} -mbulk-memory -mnontrapping-fptoint -msign-ext -mtail-call"
    )
  endif()

  # LTO breaks wasm imports currently:
  # https://www.mail-archive.com/llvm-bugs@lists.llvm.org/msg36273.html
  set(CMAKE_EXE_LINKER_FLAGS
      "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,stack-size=8388608")

elseif(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT MATCHES "MSVC")

  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -Dd_m3HasTracer -D_CRT_SECURE_NO_WARNINGS /WX- /diagnostics:column"
  )

  string(REGEX REPLACE "/W[0-4]" "/W0" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

  if(CMAKE_C_COMPILER_ID MATCHES "MSVC")

    if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
      set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /d2noftol3")
    endif()

    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Oxs /Oy /Zi /Zo")
    if(WASM3_BUILD_NATIVE)
      set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /arch:AVX2")
    endif()

  else()
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /Oxs /Oy /Qvec -Clang -O3")
  endif()

  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /STACK:8388608")

else()

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Dd_m3HasTracer")
  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -Wall -Wextra -Wparentheses -Wundef -Wpointer-arith -Wstrict-aliasing=2"
  )
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=implicit-function-declaration")
  set(CMAKE_C_FLAGS
      "${CMAKE_C_FLAGS} -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-missing-field-initializers"
  )
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  elseif(CMAKE_C_COMPILER_ID MATCHES "Intel")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fp-model precise")
  elseif(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wjump-misses-init")
  endif()
  set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -ggdb -O0")

  set(CMAKE_C_FLAGS_RELEASE "-O3 -Wfatal-errors -fomit-frame-pointer")

  if(WASM3_BUILD_NATIVE)
    if(APPLE
       AND CMAKE_C_COMPILER_ID MATCHES "Clang"
       AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64")
      set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -mcpu=native")
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "x86_64")
      set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -march=native")
    endif()
  endif()

  set(CMAKE_EXE_LINKER_FLAGS_DEBUG "-O0")
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-O3")

  if(WASM3_BUILD_APP)
    target_link_libraries(${OUT_FILE} m)
  endif()

endif()

if(BUILD_WASI MATCHES "simple")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Dd_m3HasWASI")
elseif(BUILD_WASI MATCHES "metawasi")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Dd_m3HasMetaWASI")
endif()

check_ipo_supported(RESULT result)
if(WASM3_BUILD_APP AND result AND NOT WASIENV)
  set_property(TARGET ${OUT_FILE} PROPERTY INTERPROCEDURAL_OPTIMIZATION True)
  message("LTO:        ON")
elseif(WASM3_BUILD_APP)
  message("LTO:        OFF")
else()
  message("LTO:        APP DISABLED")
endif()

add_subdirectory(source)

if(WASM3_BUILD_APP)
  target_link_libraries(${OUT_FILE} m3)
endif()

message("Flags:         ${CMAKE_C_FLAGS}")
message("Debug flags:   ${CMAKE_C_FLAGS_DEBUG}")
message("Release flags: ${CMAKE_C_FLAGS_RELEASE}")
message("----")
