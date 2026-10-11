cmake_minimum_required(VERSION 3.20)
project(coding_tools LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(CmakeUtils)
cmake_add_source(SOURCES)

salts_idl_target(
  TARGET turbo_workspace_contract
  IDL contracts/workspace.schema
  ARTIFACT_NAME workspace
  ARTIFACTS MESSAGE
  BINARY_CODEC
  FOLDER "Coding Tools/generated")

# The public target helper handles native artifacts. The custom projection uses
# the public CLI and the same explicit host SDK selection, not SDK internals.
if(SaltsUtils_IDLC_EXECUTABLE)
  set(_idlc "${SaltsUtils_IDLC_EXECUTABLE}")
else()
  find_program(_idlc NAMES salts-idlc HINTS "${SaltsUtils_IDLC_HINT}"
               NO_DEFAULT_PATH NO_CACHE REQUIRED)
endif()
get_filename_component(_idlc_bin "${_idlc}" DIRECTORY)
get_filename_component(_idlc_root "${_idlc_bin}" DIRECTORY)
if(CMAKE_CROSSCOMPILING)
  set(_idlc_salts "${SaltsUtils_IDLC_HOST_SALTS_ROOT}")
else()
  file(TO_CMAKE_PATH "$ENV{SALTS_ROOT}" _idlc_salts)
endif()
if(CMAKE_HOST_WIN32)
  set(_idlc_env "PATH=${_idlc_bin};${_idlc_root}/lib;${_idlc_salts}/bin;$ENV{PATH}")
  string(REPLACE ";" "\\;" _idlc_env "${_idlc_env}")
elseif(CMAKE_HOST_APPLE)
  set(_idlc_env "DYLD_LIBRARY_PATH=${_idlc_root}/lib:${_idlc_bin}:${_idlc_salts}/lib:$ENV{DYLD_LIBRARY_PATH}")
else()
  set(_idlc_env "LD_LIBRARY_PATH=${_idlc_root}/lib:${_idlc_bin}:${_idlc_salts}/lib:$ENV{LD_LIBRARY_PATH}")
endif()
set(_tool_schemas "${CMAKE_CURRENT_BINARY_DIR}/workspace_tool_schemas.h")
add_custom_command(
  OUTPUT "${_tool_schemas}"
  COMMAND "${CMAKE_COMMAND}" -E env "${_idlc_env}" "${_idlc}"
          "${CMAKE_CURRENT_SOURCE_DIR}/contracts/workspace.schema"
          --lang c --binary-codec --template "${CMAKE_CURRENT_SOURCE_DIR}/contracts/tool_schema.mustache"
          --output "${_tool_schemas}"
  DEPENDS contracts/workspace.schema contracts/tool_schema.mustache "${_idlc}"
  VERBATIM)

add_library(coding_tools SHARED ${SOURCES} "${_tool_schemas}")
target_compile_definitions(coding_tools PRIVATE SHARED_CXX)
target_include_directories(
  coding_tools PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                      $<INSTALL_INTERFACE:include>)
target_include_directories(coding_tools PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(coding_tools PUBLIC TurboAgent::RuntimeTools Salts::JsonParser
                                  Salts::Core
                          PRIVATE turbo_workspace_contract_native)
cmake_config_target(coding_tools FOLDER "Coding Tools" EXPORT_NAME CodingTools
                    ALIAS TurboAgent::CodingTools)
cmake_install_headers(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include)

if(BUILD_TESTS)
  add_executable(coding_process_fixture tests/coding_process_fixture.c)
  target_link_libraries(coding_process_fixture PRIVATE Salts::Core)
  set_target_properties(coding_process_fixture PROPERTIES FOLDER "Coding Tools/tests")
  file(GLOB TEST_SOURCES "tests/test_*.c")
  cmake_add_test(SOURCES ${TEST_SOURCES} LIBS coding_tools Salts::TinyTest
                 INCLUDES include FOLDER "Coding Tools/tests")
  target_link_libraries(test_turbo_coding_contract PRIVATE turbo_workspace_contract_native)
  target_compile_definitions(test_turbo_coding_workspace PRIVATE
    CODING_PROCESS_FIXTURE="$<TARGET_FILE:coding_process_fixture>")
  add_dependencies(test_turbo_coding_workspace coding_process_fixture)
endif()
