set(sources
    "m3_api_libc.c"
    "m3_api_wasi.c"
    "m3_api_meta_wasi.c"
    "m3_api_tracer.c"
    "m3_bind.c"
    "m3_code.c"
    "m3_compile.c"
    "m3_core.c"
    "m3_env.c"
    "m3_exec.c"
    "m3_function.c"
    "m3_info.c"
    "m3_module.c"
    "m3_parse.c")
file(GLOB EXECUTOR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
add_library(m3 STATIC ${sources})
target_include_directories(
  m3 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include/wasm3>)
target_link_libraries(m3 PUBLIC TurboNet::Utils)
if(NOT WIN32)
  target_link_libraries(m3 PUBLIC m)
endif()
set_target_properties(
  m3
  PROPERTIES FOLDER "wasm3"
             C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS NO)

set(WASM3_CORE_CONFIG_ARGS FOLDER "wasm3" EXPORT_NAME Wasm3Core)
if(NOT TARGET TurboHttp::Wasm3Core)
  list(APPEND WASM3_CORE_CONFIG_ARGS ALIAS TurboHttp::Wasm3Core)
endif()
cmake_config_target(m3 ${WASM3_CORE_CONFIG_ARGS})

target_compile_features(m3 PRIVATE c_std_11)

if(CMAKE_C_COMPILER_ID MATCHES "MSVC")
  # add MSVC specific flags here
else()
  # Flags common for GCC and Clang

  # FIXME: comparison of integers of different signs: 'u32' and 'i32'
  set_source_files_properties(m3_env.c PROPERTIES COMPILE_FLAGS
                                                  -Wno-sign-compare)

  if(WASIENV)
    # FIXME: declaration of 'struct sigaction' will not be visible outside of
    # this function
    target_compile_options(m3 PUBLIC -Wno-visibility)
    # FIXME incompatible pointer types passing 'u32 *' to parameter of type
    # 'char **'
    set_source_files_properties(
      m3_api_meta_wasi.c PROPERTIES COMPILE_FLAGS
                                    -Wno-incompatible-pointer-types)
  endif()

  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Clang specific flags here
  else()
    # GCC specific flags here
  endif()
endif()

if(BUILD_WASI MATCHES "simple")
  target_compile_definitions(m3 PUBLIC d_m3HasWASI)
elseif(BUILD_WASI MATCHES "metawasi")
  target_compile_definitions(m3 PUBLIC d_m3HasMetaWASI)
endif()

cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                         DESTINATION include/wasm3)
