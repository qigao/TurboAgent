add_library(turbo_cli_driver STATIC src/turbo_cli.c src/turbo_cli_options.c)
target_include_directories(turbo_cli_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(turbo_cli_driver PUBLIC TurboAgent::Harness)
target_link_libraries(turbo_cli_driver PRIVATE Salts::CmdParser)
set_target_properties(turbo_cli_driver PROPERTIES FOLDER "CLI")
if(ENABLE_CODING_TOOLS)
  target_link_libraries(turbo_cli_driver PRIVATE TurboAgent::CodingTools)
  target_compile_definitions(turbo_cli_driver PRIVATE TURBO_CLI_CODING_TOOLS)
endif()
add_executable(turboagent src/main.c)
target_link_libraries(turboagent PRIVATE turbo_cli_driver)
set_target_properties(turboagent PROPERTIES FOLDER "CLI")
install(TARGETS turboagent RUNTIME DESTINATION bin)
if(BUILD_TESTS AND ENABLE_CODING_TOOLS)
  cmake_add_test(SOURCES tests/test_turbo_cli.c tests/test_turbo_cli_workspace.c tests/test_turbo_cli_live.c
                 tests/test_turbo_cli_diagnostics.c tests/test_turbo_cli_completion.c
                 tests/test_turbo_cli_tool_cancel.c
                 LIBS turbo_cli_driver Salts::TinyTest
                 FOLDER "CLI/tests")
  target_include_directories(test_turbo_cli_diagnostics PRIVATE "${PROJECT_SOURCE_DIR}/agent/tests")
  target_compile_definitions(test_turbo_cli PRIVATE TURBO_CLI_EXECUTABLE="$<TARGET_FILE:turboagent>")
  add_dependencies(test_turbo_cli turboagent)
  target_compile_definitions(test_turbo_cli_live PRIVATE TURBO_CLI_EXECUTABLE="$<TARGET_FILE:turboagent>")
  add_dependencies(test_turbo_cli_live turboagent)
  target_link_libraries(test_turbo_cli_tool_cancel PRIVATE TurboAgent::CodingTools)
  target_compile_definitions(test_turbo_cli_tool_cancel PRIVATE
    CODING_PROCESS_FIXTURE="$<TARGET_FILE:coding_process_fixture>")
  add_dependencies(test_turbo_cli_tool_cancel coding_process_fixture)
  set_tests_properties(test_turbo_cli_tool_cancel PROPERTIES TIMEOUT 60)
  set_tests_properties(test_turbo_cli_live PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    LABELS "live"
    TIMEOUT 240
    SKIP_REGULAR_EXPRESSION "0 passed, 0 failed, 1 skipped")
endif()
