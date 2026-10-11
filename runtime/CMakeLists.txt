cmake_minimum_required(VERSION 3.20)
project(runtime LANGUAGES C)

include(CmakeUtils)
add_library(turbo_runtime SHARED
  src/turbo_event.c
  src/turbo_event_log.c
  src/turbo_graph.c
  src/turbo_graph_run_log.c
  src/turbo_runtime_store.c
  src/turbo_runtime_segment.c
  src/turbo_runtime_history.c
  src/turbo_state_graph.c)
target_compile_definitions(turbo_runtime PRIVATE SHARED_CXX)
target_include_directories(turbo_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_runtime PUBLIC
  TurboAgent::RuntimeCore TurboAgent::RuntimeData)
cmake_config_target(turbo_runtime FOLDER "Runtime" EXPORT_NAME Runtime
                    ALIAS TurboAgent::Runtime)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_EXAMPLES)
  foreach(example_name state_graph_store_latest_snapshot_example
                       state_graph_store_file_latest_snapshot_example)
    add_executable(${example_name} examples/${example_name}.c)
    target_link_libraries(${example_name} PRIVATE TurboAgent::Runtime)
    set_target_properties(${example_name} PROPERTIES FOLDER "Runtime/examples")
  endforeach()
endif()

if(BUILD_TESTS)
  cmake_add_test(
    SOURCES tests/test_turbo_graph.c tests/test_turbo_graph_run_log.c
            tests/test_turbo_state_graph.c tests/test_turbo_runtime_store.c
            tests/test_turbo_runtime_segment.c
            tests/test_turbo_runtime_history.c
    LIBS TurboAgent::Runtime Salts::TinyTest
    FOLDER "Runtime/tests")
endif()
