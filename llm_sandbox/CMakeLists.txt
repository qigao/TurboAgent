cmake_minimum_required(VERSION 3.20)
project(llm_sandbox LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)

cmake_add_source(SOURCES)

add_library(llm_sandbox SHARED ${SOURCES})
target_compile_definitions(llm_sandbox PRIVATE SHARED_CXX)
target_include_directories(
  llm_sandbox PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                     $<INSTALL_INTERFACE:include>)
target_link_libraries(llm_sandbox PUBLIC TurboAgent::RuntimeTools
                                         TurboWasm::Runtime)

cmake_config_target(llm_sandbox FOLDER "LLM Sandbox" EXPORT_NAME LLMSandbox
                    ALIAS TurboAgent::LLMSandbox)
add_library(TurboNet::LLMSandbox ALIAS llm_sandbox)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_TESTS)
  find_program(LLM_SANDBOX_WASM_CLANG NAMES clang clang.exe)
  if(LLM_SANDBOX_WASM_CLANG)
    get_target_property(LLM_SANDBOX_TURBOWASM_INCLUDES TurboWasm::Runtime
                        INTERFACE_INCLUDE_DIRECTORIES)
    set(LLM_SANDBOX_GUEST_INCLUDE_ARGS ${LLM_SANDBOX_TURBOWASM_INCLUDES})
    list(TRANSFORM LLM_SANDBOX_GUEST_INCLUDE_ARGS PREPEND "-I")

    function(llm_sandbox_add_guest output source max_memory)
      add_custom_command(
        OUTPUT "${output}"
        COMMAND
          ${LLM_SANDBOX_WASM_CLANG} --target=wasm32-unknown-unknown -O2
          -nostdlib ${LLM_SANDBOX_GUEST_INCLUDE_ARGS} -Wl,--no-entry
          -Wl,--export-if-defined=turbo_tool_count
          -Wl,--export-if-defined=turbo_tool_describe
          -Wl,--export-if-defined=turbo_tool_invoke
          -Wl,--initial-memory=131072 -Wl,--max-memory=${max_memory}
          -Wl,--allow-undefined -o "${output}" "${source}"
        DEPENDS "${source}"
                ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
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

    cmake_add_test(
      SOURCES tests/test_turbo_tool_runtime_wasm.c
      LIBS llm_sandbox TurboUtils::TinyTest
      FOLDER "LLM Sandbox/tests")

    cmake_add_test(
      SOURCES tests/test_turbo_wasm_tool_pack.c
      LIBS llm_sandbox TurboUtils::TinyTest
      FOLDER "LLM Sandbox/tests")

    if(TARGET langchain)
      cmake_add_test(
        SOURCES tests/test_turbo_wasm_tool_pack_workspace.c
        LIBS llm_sandbox langchain TurboUtils::TinyTest
        FOLDER "LLM Sandbox/tests")
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
                ${LLM_SANDBOX_WASM_BAD_SCHEMA_WASM})
      set_target_properties(llm_sandbox_wasm_tool_guests
                            PROPERTIES FOLDER "LLM Sandbox")
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
          LLM_SANDBOX_WASM_BAD_SCHEMA_WASM_PATH="${LLM_SANDBOX_WASM_BAD_SCHEMA_WASM}")
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
    endif()
  endif()
endif()
