cmake_minimum_required(VERSION 3.20)
project(wasm_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(wasm_tools SHARED ${SOURCES})
target_compile_definitions(wasm_tools PRIVATE SHARED_CXX)
target_include_directories(
  wasm_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                     $<INSTALL_INTERFACE:include>)
target_link_libraries(wasm_tools
  PUBLIC TurboAgent::RuntimeTools
         TurboWasm::Runtime
  PRIVATE Salts::Core Salts::Platform)

cmake_config_target(wasm_tools FOLDER "Wasm Tools" EXPORT_NAME WasmTools
                    ALIAS TurboAgent::WasmTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  find_program(LLM_SANDBOX_WASM_CLANG NAMES clang clang.exe)
  if(NOT LLM_SANDBOX_WASM_CLANG)
    message(FATAL_ERROR
      "TurboAgent WasmTools tests require clang with wasm32-unknown-unknown support")
  endif()
    function(llm_sandbox_add_guest output source max_memory)
      add_custom_command(
        OUTPUT "${output}"
        COMMAND
          ${LLM_SANDBOX_WASM_CLANG} --target=wasm32-unknown-unknown -O2
          -nostdlib -Wl,--no-entry
          -Wl,--export-if-defined=turbo_tool_count
          -Wl,--export-if-defined=turbo_tool_describe
          -Wl,--export-if-defined=turbo_tool_invoke
          -Wl,--initial-memory=131072 -Wl,--max-memory=${max_memory}
          -Wl,--allow-undefined -o "${output}" "${source}"
        DEPENDS "${source}"
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_wasm_tools_guest.h
        VERBATIM)
    endfunction()

    set(LLM_SANDBOX_WASM_TOOL_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_test.wasm")
    set(LLM_SANDBOX_WASM_MEMORY_GROW_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_memory_grow_test.wasm")
    set(LLM_SANDBOX_WASM_BAD_INVOKE_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_bad_invoke_test.wasm")
    set(LLM_SANDBOX_WASM_MISSING_EXPORT_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_missing_export_test.wasm")
    set(LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_negative_count_test.wasm")
    set(LLM_SANDBOX_WASM_ZERO_TOOLS_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_zero_tools_test.wasm")
    set(LLM_SANDBOX_WASM_BAD_METADATA_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_bad_metadata_test.wasm")
    set(LLM_SANDBOX_WASM_BAD_SCHEMA_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_bad_schema_test.wasm")
    set(LLM_SANDBOX_WASM_BAD_EFFECTS_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_bad_effects_test.wasm")
    set(LLM_SANDBOX_WASM_CONTROL_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_control_test.wasm")
    set(LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_forbidden_import_test.wasm")
    set(LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_embedded_nul_test.wasm")

    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_TOOL_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_MEMORY_GROW_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_memory_grow.c"
      1179648)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_BAD_INVOKE_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_invoke.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_MISSING_EXPORT_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_missing_export.c"
      131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_negative_count.c"
      131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_ZERO_TOOLS_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_zero_tools.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_BAD_METADATA_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_metadata_ptr.c"
      131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_BAD_SCHEMA_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_schema.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_BAD_EFFECTS_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_bad_effects.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_CONTROL_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_control.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_forbidden_import.c" 131072)
    llm_sandbox_add_guest(
      "${LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_embedded_nul.c" 131072)

    cmake_add_test(
      SOURCES tests/test_turbo_tool_runtime_wasm.c
      LIBS wasm_tools Salts::TinyTest
      FOLDER "Wasm Tools/tests")

    cmake_add_test(
      SOURCES tests/test_turbo_wasm_tool_pack.c
      LIBS wasm_tools agent_compiler Salts::TinyTest
      FOLDER "Wasm Tools/tests")

    if(TARGET praktor_tools)
      cmake_add_test(
        SOURCES tests/test_praktor_host_tool_wasm.c
        LIBS wasm_tools praktor_tools Salts::TinyTest
        FOLDER "Wasm Tools/tests")
    endif()

    if(TARGET langchain)
      cmake_add_test(
        SOURCES tests/test_turbo_wasm_tool_pack_workspace.c
        LIBS wasm_tools langchain Salts::TinyTest
        FOLDER "Wasm Tools/tests")
    endif()

    if(TARGET test_turbo_tool_runtime_wasm)
      add_custom_target(
        llm_sandbox_wasm_tool_guests
        DEPENDS ${LLM_SANDBOX_WASM_TOOL_WASM}
                ${LLM_SANDBOX_WASM_MEMORY_GROW_WASM}
                ${LLM_SANDBOX_WASM_BAD_INVOKE_WASM}
                ${LLM_SANDBOX_WASM_MISSING_EXPORT_WASM}
                ${LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM}
                ${LLM_SANDBOX_WASM_ZERO_TOOLS_WASM}
                ${LLM_SANDBOX_WASM_BAD_METADATA_WASM}
                ${LLM_SANDBOX_WASM_BAD_SCHEMA_WASM}
                ${LLM_SANDBOX_WASM_BAD_EFFECTS_WASM}
                ${LLM_SANDBOX_WASM_CONTROL_WASM}
                ${LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM}
                ${LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM})
      set_target_properties(llm_sandbox_wasm_tool_guests
                            PROPERTIES FOLDER "Wasm Tools")
      add_dependencies(test_turbo_tool_runtime_wasm llm_sandbox_wasm_tool_guests)
      target_compile_definitions(
        test_turbo_tool_runtime_wasm
        PRIVATE
          LLM_SANDBOX_WASM_TOOL_WASM_PATH="${LLM_SANDBOX_WASM_TOOL_WASM}"
          LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH="${LLM_SANDBOX_WASM_MEMORY_GROW_WASM}"
          LLM_SANDBOX_WASM_BAD_INVOKE_WASM_PATH="${LLM_SANDBOX_WASM_BAD_INVOKE_WASM}"
          LLM_SANDBOX_WASM_MISSING_EXPORT_WASM_PATH="${LLM_SANDBOX_WASM_MISSING_EXPORT_WASM}"
          LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM_PATH="${LLM_SANDBOX_WASM_NEGATIVE_COUNT_WASM}"
          LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH="${LLM_SANDBOX_WASM_ZERO_TOOLS_WASM}"
          LLM_SANDBOX_WASM_BAD_METADATA_WASM_PATH="${LLM_SANDBOX_WASM_BAD_METADATA_WASM}"
          LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH="${LLM_SANDBOX_WASM_BAD_SCHEMA_WASM}"
          LLM_SANDBOX_WASM_BAD_EFFECTS_WASM_PATH="${LLM_SANDBOX_WASM_BAD_EFFECTS_WASM}"
          LLM_SANDBOX_WASM_CONTROL_WASM_PATH="${LLM_SANDBOX_WASM_CONTROL_WASM}"
          LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM_PATH="${LLM_SANDBOX_WASM_FORBIDDEN_IMPORT_WASM}"
          LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM_PATH="${LLM_SANDBOX_WASM_EMBEDDED_NUL_WASM}")
      if(TARGET test_turbo_wasm_tool_pack)
        add_dependencies(test_turbo_wasm_tool_pack llm_sandbox_wasm_tool_guests)
        target_compile_definitions(
          test_turbo_wasm_tool_pack
          PRIVATE
            LLM_SANDBOX_WASM_TOOL_WASM_PATH="${LLM_SANDBOX_WASM_TOOL_WASM}"
            LLM_SANDBOX_WASM_MEMORY_GROW_WASM_PATH="${LLM_SANDBOX_WASM_MEMORY_GROW_WASM}"
            LLM_SANDBOX_WASM_ZERO_TOOLS_WASM_PATH="${LLM_SANDBOX_WASM_ZERO_TOOLS_WASM}")
      endif()
      if(TARGET test_turbo_wasm_tool_pack_workspace)
        add_dependencies(test_turbo_wasm_tool_pack_workspace
                         llm_sandbox_wasm_tool_guests)
        target_compile_definitions(
          test_turbo_wasm_tool_pack_workspace
          PRIVATE
            LLM_SANDBOX_WASM_TOOL_WASM_PATH="${LLM_SANDBOX_WASM_TOOL_WASM}")
      endif()
      if(TARGET test_praktor_host_tool_wasm)
        add_dependencies(test_praktor_host_tool_wasm
                         llm_sandbox_wasm_tool_guests)
        target_compile_definitions(
          test_praktor_host_tool_wasm
          PRIVATE
            LLM_SANDBOX_WASM_TOOL_WASM_PATH="${LLM_SANDBOX_WASM_TOOL_WASM}")
      endif()
    endif()
endif()
