cmake_minimum_required(VERSION 3.20)
project(langchain LANGUAGES C)

# Configure module path to find CmakeUtils
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")

# Collect all sources
include(CmakeUtils)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
cmake_add_source(
  SOURCES
  EXCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_agent_runtime_v1_command.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_agent_runtime_v1_file_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_agent_runtime_v1_memory_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_agent_runtime_v1_observability.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/turbo_agent_runtime_v1_util.c)

# Create the library
add_library(langchain SHARED ${SOURCES})
target_compile_definitions(langchain PRIVATE SHARED_CXX)
# Configure include directories
target_include_directories(
  langchain
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_include_directories(langchain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Link dependencies
target_link_libraries(
  langchain
  PUBLIC TurboAgent::RuntimeCore
         TurboAgent::RuntimeData
         TurboAgent::RuntimeTools
         Salts::JsonParser
         Salts::UriParser
         CHttp::Client
         CHttp::Server
         Salts::Core
         Salts::Mustache
         unofficial::sqlite3::sqlite3
  PRIVATE OpenSSL::Crypto)

# Use the utility to configure common settings (Folder, Version, etc.)
cmake_config_target(langchain FOLDER "LangChain" EXPORT_NAME LangChain
                    ALIAS TurboAgent::LangChain)

# Harness is an additive host-facing facade implemented by LangChain. Keep it
# as an interface target so consumers can depend on the narrower capability
# name without introducing a second runtime binary or duplicated state owner.
add_library(turbo_agent_harness INTERFACE)
target_link_libraries(turbo_agent_harness INTERFACE langchain)
cmake_config_target(turbo_agent_harness FOLDER "LangChain" EXPORT_NAME Harness
                    ALIAS TurboAgent::Harness)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
                      DESTINATION include)

if(BUILD_EXAMPLES)
  file(GLOB LANGCHAIN_EXAMPLE_SOURCES CONFIGURE_DEPENDS "examples/*.c")
  foreach(example_source ${LANGCHAIN_EXAMPLE_SOURCES})
    get_filename_component(example_name ${example_source} NAME_WE)
    if(NOT TARGET ${example_name})
      add_executable(${example_name} ${example_source})
      target_link_libraries(${example_name} PRIVATE langchain)
      set_target_properties(${example_name} PROPERTIES FOLDER
                                                       "LangChain/examples")
    endif()
  endforeach()
endif()

if(BUILD_TESTS)
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(
    SOURCES
    ${TEST_SOURCES}
    LIBS
    langchain
    Salts::TinyTest
    INCLUDES
    include
    tests
    FOLDER
    "LangChain/tests")
endif()
