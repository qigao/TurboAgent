cmake_minimum_required(VERSION 3.20)
project(runtime LANGUAGES C)

include(CmakeUtils)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
add_library(turbo_runtime SHARED
  src/turbo_event.c
  src/turbo_event_log.c
  src/turbo_graph.c
  src/turbo_graph_run_log.c
  src/turbo_runtime_store.c
  src/turbo_runtime_store_sqlite.c
  src/turbo_runtime_segment.c
  src/turbo_runtime_history.c
  src/turbo_runtime_head.c
  src/turbo_runtime_diagram.c
  src/turbo_state_graph.c)
target_compile_definitions(turbo_runtime PRIVATE SHARED_CXX)
target_include_directories(turbo_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_runtime PUBLIC
  TurboAgent::RuntimeCore TurboAgent::RuntimeData)
target_link_libraries(turbo_runtime PRIVATE unofficial::sqlite3::sqlite3)
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
            tests/test_turbo_runtime_store_sqlite.c
            tests/test_turbo_runtime_segment.c
            tests/test_turbo_runtime_history.c
            tests/test_turbo_runtime_diagram.c
    LIBS TurboAgent::Runtime Salts::TinyTest
    FOLDER "Runtime/tests")
  target_link_libraries(test_turbo_runtime_store_sqlite PRIVATE unofficial::sqlite3::sqlite3)
endif()


# Qualify native Salts Provider DSO unload independently of the optional
# PraktorTools library. In particular, Windows Praktor is OFF and macOS only
# builds a targeted runtime test rather than enabling the whole test suite.
if(BUILD_TESTS OR ENABLE_PROVIDER_DSO_CONFORMANCE)
  if(NOT ANDROID)
    set(_provider_dso_fixture_dir "${CMAKE_SOURCE_DIR}/praktor_tools/tests")
    add_library(turboagent_component_dso_n SHARED
      "${_provider_dso_fixture_dir}/praktor_dso_provider.c")
    add_library(turboagent_component_dso_n1 SHARED
      "${_provider_dso_fixture_dir}/praktor_dso_provider.c")
    foreach(_fixture IN ITEMS turboagent_component_dso_n turboagent_component_dso_n1)
      target_include_directories(${_fixture} PRIVATE "${_provider_dso_fixture_dir}")
      target_link_libraries(${_fixture} PRIVATE
        Salts::ComponentPluginABI Salts::PluginABI)
      set_target_properties(${_fixture} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        FOLDER "Runtime/tests/fixtures")
    endforeach()
    target_compile_definitions(turboagent_component_dso_n PRIVATE
      PRAKTOR_DSO_EPOCH=41
      "PRAKTOR_DSO_PLUGIN_ID=\\\"test.turboagent.component.provider.N\\\"")
    target_compile_definitions(turboagent_component_dso_n1 PRIVATE
      PRAKTOR_DSO_EPOCH=42
      "PRAKTOR_DSO_PLUGIN_ID=\\\"test.turboagent.component.provider.N1\\\"")

    cmake_add_test(
      SOURCES tests/test_turbo_component_dso_lifecycle.c
      LIBS Salts::ComponentPlugin Salts::Plugin Salts::Core Salts::TinyTest
      INCLUDES "${_provider_dso_fixture_dir}"
      FOLDER "Runtime/tests")
    target_compile_definitions(test_turbo_component_dso_lifecycle PRIVATE
      "PRAKTOR_DSO_N_PATH=\\\"$<TARGET_FILE:turboagent_component_dso_n>\\\""
      "PRAKTOR_DSO_N1_PATH=\\\"$<TARGET_FILE:turboagent_component_dso_n1>\\\"")
    add_dependencies(test_turbo_component_dso_lifecycle
      turboagent_component_dso_n turboagent_component_dso_n1)
    set_tests_properties(test_turbo_component_dso_lifecycle
      PROPERTIES TIMEOUT 60)
    unset(_fixture)
    unset(_provider_dso_fixture_dir)
  endif()
endif()
