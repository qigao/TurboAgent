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
target_link_libraries(llm_sandbox PUBLIC TurboNet::RuntimeTools PRIVATE turbo_wasm3)

cmake_config_target(llm_sandbox FOLDER "LLM Sandbox" ALIAS TurboNet::LLMSandbox)

if(BUILD_TESTS)
  find_program(LLM_SANDBOX_WASM3_CLANG NAMES clang clang.exe)
  if(LLM_SANDBOX_WASM3_CLANG)
    set(LLM_SANDBOX_WASM3_TOOL_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_test.wasm")
    set(LLM_SANDBOX_WASM3_BAD_INVOKE_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_bad_invoke_test.wasm")
    set(LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_missing_export_test.wasm")
    set(LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_negative_count_test.wasm")
    set(LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_zero_tools_test.wasm")
    set(LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_bad_metadata_ptr_test.wasm")
    set(LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_bad_schema_test.wasm")
    set(LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_bad_input_buffer_test.wasm")
    set(LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM
        "${CMAKE_CURRENT_BINARY_DIR}/guest_tool_runtime_wasm3_bad_output_buffer_test.wasm")

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_TOOL_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_TOOL_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_BAD_INVOKE_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_BAD_INVOKE_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_invoke.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_invoke.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_missing_export.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_missing_export.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_negative_count.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_negative_count.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_zero_tools.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_zero_tools.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_metadata_ptr.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_metadata_ptr.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_schema.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_schema.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_input_buffer.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_input_buffer.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    add_custom_command(
      OUTPUT ${LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM}
      COMMAND
        ${LLM_SANDBOX_WASM3_CLANG} --target=wasm32-unknown-unknown -O2
        -nostdlib -Wl,--no-entry -Wl,--export=turbo_tool_count
        -Wl,--export=turbo_tool_name -Wl,--export=turbo_tool_description
        -Wl,--export=turbo_tool_parameters -Wl,--export=turbo_tool_strict
        -Wl,--export=turbo_tool_input_ptr
        -Wl,--export=turbo_tool_input_capacity
        -Wl,--export=turbo_tool_output_ptr
        -Wl,--export=turbo_tool_output_capacity
        -Wl,--export=turbo_tool_invoke -Wl,--export-memory
        -Wl,--initial-memory=131072 -Wl,--max-memory=131072
        -Wl,--allow-undefined -o ${LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_output_buffer.c
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/guest_tool_runtime_wasm3_bad_output_buffer.c
        ${CMAKE_CURRENT_SOURCE_DIR}/include/turbo_llm_sandbox_guest.h
      VERBATIM)

    cmake_add_test(
      SOURCES
      tests/test_turbo_tool_runtime_wasm3.c
      LIBS
      llm_sandbox
      TurboNet::TinyTest
      FOLDER
      "LLM Sandbox/tests")

    if(TARGET test_turbo_tool_runtime_wasm3)
      add_custom_target(llm_sandbox_wasm3_tool_guest_test
                        DEPENDS
                          ${LLM_SANDBOX_WASM3_TOOL_WASM}
                          ${LLM_SANDBOX_WASM3_BAD_INVOKE_WASM}
                          ${LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM}
                          ${LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM}
                          ${LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM}
                          ${LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM}
                          ${LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM}
                          ${LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM}
                          ${LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM})
      set_target_properties(llm_sandbox_wasm3_tool_guest_test
                            PROPERTIES FOLDER "LLM Sandbox")
      add_dependencies(test_turbo_tool_runtime_wasm3
                       llm_sandbox_wasm3_tool_guest_test)
      target_compile_definitions(
        test_turbo_tool_runtime_wasm3
        PRIVATE
          LLM_SANDBOX_WASM3_TOOL_WASM_PATH=\"${LLM_SANDBOX_WASM3_TOOL_WASM}\"
          LLM_SANDBOX_WASM3_BAD_INVOKE_WASM_PATH=\"${LLM_SANDBOX_WASM3_BAD_INVOKE_WASM}\"
          LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM_PATH=\"${LLM_SANDBOX_WASM3_MISSING_EXPORT_WASM}\"
          LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM_PATH=\"${LLM_SANDBOX_WASM3_NEGATIVE_COUNT_WASM}\"
          LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM_PATH=\"${LLM_SANDBOX_WASM3_ZERO_TOOLS_WASM}\"
          LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM_PATH=\"${LLM_SANDBOX_WASM3_BAD_METADATA_PTR_WASM}\"
          LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM_PATH=\"${LLM_SANDBOX_WASM3_BAD_SCHEMA_WASM}\"
          LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM_PATH=\"${LLM_SANDBOX_WASM3_BAD_INPUT_BUFFER_WASM}\"
          LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM_PATH=\"${LLM_SANDBOX_WASM3_BAD_OUTPUT_BUFFER_WASM}\")
    endif()
  endif()
endif()
