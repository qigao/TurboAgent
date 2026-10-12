cmake_minimum_required(VERSION 3.25)
project(TurboAgentInstalledConsumer LANGUAGES C)

# Explicitly define the policy used for upper-case SDK *_ROOT variables.
# The actual packages are still pinned via *_DIR and checked below; the
# environment is not permission to silently load an unrelated SDK.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

if(NOT DEFINED EXPECTED_TURBOAGENT_PREFIX OR NOT IS_DIRECTORY "${EXPECTED_TURBOAGENT_PREFIX}")
  message(FATAL_ERROR "An exact unpacked SDK prefix is required")
endif()
file(REAL_PATH "${EXPECTED_TURBOAGENT_PREFIX}" _expected)

# Check every explicitly supplied SDK config, including optional components
# not imported by the current feature profile. This also makes unused *_DIR
# arguments an error instead of silently accepting misleading configuration.
foreach(_dependency IN ITEMS Salts SaltsUtils Chttp TurboWasm Praktor TurboScript)
  if(NOT DEFINED ${_dependency}_DIR)
    message(FATAL_ERROR "Missing explicit ${_dependency}_DIR SDK binding")
  endif()
  if(NOT EXISTS "${${_dependency}_DIR}/${_dependency}Config.cmake")
    message(FATAL_ERROR "Missing exact ${_dependency} package config at ${${_dependency}_DIR}")
  endif()
endforeach()

find_package(TurboAgent CONFIG REQUIRED
  PATHS "${_expected}"
  NO_DEFAULT_PATH)
file(REAL_PATH "${TurboAgent_DIR}" _resolved_config)
string(FIND "${_resolved_config}/" "${_expected}/" _prefix_pos)
if(NOT _prefix_pos EQUAL 0)
  message(FATAL_ERROR "TurboAgent config escaped expected SDK prefix: ${_resolved_config}")
endif()
if(NOT TARGET TurboAgent::Runtime)
  message(FATAL_ERROR "Missing installed TurboAgent::Runtime target")
endif()

add_executable(turboagent_installed_consumer main.c)
target_link_libraries(turboagent_installed_consumer PRIVATE TurboAgent::Runtime)

# Imported installed libraries do not automatically make their transitive
# shared-library directories visible to the Linux loader. Test with a
# deliberately explicit deployment graph, not a source/build-tree fallback or
# an ambient LD_LIBRARY_PATH. Use the unpacked package and restored SDK roots.
set(_runtime_dirs "${_expected}/lib")
foreach(_root_name IN ITEMS
    SALTS_ROOT SALTS_UTILS_ROOT CHTTP_ROOT
    TURBOWASM_ROOT PRAKTOR_ROOT TURBOSCRIPT_ROOT)
  if(NOT DEFINED ENV{${_root_name}} OR "$ENV{${_root_name}}" STREQUAL "")
    message(FATAL_ERROR "Missing required runtime SDK root ${_root_name}")
  endif()
  file(REAL_PATH "$ENV{${_root_name}}" _root)
  if(NOT IS_DIRECTORY "${_root}/lib")
    message(FATAL_ERROR "Missing runtime library directory for ${_root_name}: ${_root}/lib")
  endif()
  list(APPEND _runtime_dirs "${_root}/lib")
endforeach()
# The isolated vcpkg prefix holds third-party native libraries (GmSSL/SQLite).
foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
  if(IS_DIRECTORY "${_prefix}/lib")
    list(APPEND _runtime_dirs "${_prefix}/lib")
  endif()
endforeach()
list(REMOVE_DUPLICATES _runtime_dirs)
set_target_properties(turboagent_installed_consumer PROPERTIES
  BUILD_RPATH "${_runtime_dirs}")

enable_testing()
add_test(NAME turboagent.installed_consumer COMMAND turboagent_installed_consumer)
