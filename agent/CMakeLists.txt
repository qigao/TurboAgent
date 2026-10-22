cmake_minimum_required(VERSION 3.20)
project(agent LANGUAGES C)

include(CmakeUtils)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
cmake_add_source(SOURCES)
add_library(turbo_agent SHARED ${SOURCES})
target_compile_definitions(turbo_agent PRIVATE SHARED_CXX)
target_include_directories(turbo_agent
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(turbo_agent
  PUBLIC TurboAgent::Runtime TurboAgent::RuntimeTools TurboAgent::Provider
         Salts::JsonParser Salts::UriParser Salts::DotEnvParser
         CHttp::Client CHttp::Server Salts::Core
         unofficial::sqlite3::sqlite3
  PRIVATE Salts::CYamlJsonAdapter Salts::CMeta)
cmake_config_target(turbo_agent FOLDER "Agent" EXPORT_NAME Agent
                    ALIAS TurboAgent::Agent)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_EXAMPLES)
  file(GLOB AGENT_EXAMPLE_SOURCES CONFIGURE_DEPENDS "examples/*.c")
  foreach(example_source ${AGENT_EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_source} NAME_WE)
    add_executable(${example_name} ${example_source})
    target_link_libraries(${example_name} PRIVATE TurboAgent::Agent)
    set_target_properties(${example_name} PROPERTIES FOLDER "Agent/examples")
  endforeach()
endif()

if(BUILD_TESTS)
  # Compile the documented Host example in normal test builds as well.
  if(NOT BUILD_EXAMPLES)
    add_executable(workspace_document_render examples/workspace_document_render.c)
    target_link_libraries(workspace_document_render PRIVATE TurboAgent::Agent)
    set_target_properties(workspace_document_render PROPERTIES FOLDER "Agent/examples")
  endif()
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "tests/test_*.c")
  cmake_add_test(SOURCES ${TEST_SOURCES}
    LIBS TurboAgent::Agent Salts::TinyTest
    INCLUDES include tests FOLDER "Agent/tests")
endif()
