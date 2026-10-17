cmake_minimum_required(VERSION 3.25)
project(TurboAgentM43FliteProcess LANGUAGES C)
include(CTest)
enable_testing()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "M4.3 Flite process adapter is currently Linux-only")
endif()

add_library(turbo_tts_m43_flite_process STATIC
  src/turbo_agent_tts_flite_process.c)
target_compile_features(turbo_tts_m43_flite_process PUBLIC c_std_11)
set_target_properties(turbo_tts_m43_flite_process PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(turbo_tts_m43_flite_process PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_include_directories(turbo_tts_m43_flite_process PRIVATE src)

add_library(turbo_tts_m43_source STATIC
  ../src/turbo_agent_speech_output.c)
target_compile_features(turbo_tts_m43_source PUBLIC c_std_11)
set_target_properties(turbo_tts_m43_source PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(turbo_tts_m43_source PUBLIC ../include)

add_executable(m43_process_fixture tests/process_fixture.c)
target_include_directories(m43_process_fixture PRIVATE src)
add_executable(test_m43_process tests/test_m43_process.c)
target_link_libraries(test_m43_process PRIVATE
  turbo_tts_m43_flite_process turbo_tts_m43_source)
target_compile_definitions(test_m43_process PRIVATE
  TEST_M43_HELPER_PATH="$<TARGET_FILE:m43_process_fixture>")
add_dependencies(test_m43_process m43_process_fixture)

foreach(t turbo_tts_m43_flite_process turbo_tts_m43_source
          m43_process_fixture test_m43_process)
  set_target_properties(${t} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endforeach()
add_test(NAME m43_process_source_provider
  COMMAND test_m43_process)
set_tests_properties(m43_process_source_provider
  PROPERTIES TIMEOUT 30)

# Exact real dlopen/dlclose: the shared Provider contains the entire process
# adapter, with no unresolved SpeechOutput symbols supplied by the host.
# Still uses only deterministic child fixture PCM (no Flite/model/voice).
add_library(turbo_tts_m43_flite_provider_dso SHARED
  src/turbo_agent_tts_flite_process.c
  src/turbo_agent_tts_flite_dso.c)
set_target_properties(turbo_tts_m43_flite_provider_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(turbo_tts_m43_flite_provider_dso PRIVATE
  src include ../include)
target_link_options(turbo_tts_m43_flite_provider_dso PRIVATE "LINKER:-z,defs")
target_compile_options(turbo_tts_m43_flite_provider_dso PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

add_executable(test_m43_dso tests/test_m43_dso.c)
set_target_properties(test_m43_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_m43_dso PRIVATE include ../include)
target_link_libraries(test_m43_dso PRIVATE
  turbo_tts_m43_source "${CMAKE_DL_LIBS}")
target_compile_definitions(test_m43_dso PRIVATE
  TEST_M43_HELPER_PATH="$<TARGET_FILE:m43_process_fixture>"
  TEST_M43_DSO_PATH="$<TARGET_FILE:turbo_tts_m43_flite_provider_dso>")
target_compile_options(test_m43_dso PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_dependencies(test_m43_dso m43_process_fixture
  turbo_tts_m43_flite_provider_dso)
add_test(NAME m43_process_dlopen_provider COMMAND test_m43_dso)
set_tests_properties(m43_process_dlopen_provider PROPERTIES TIMEOUT 30)

# The normal build, default speech paths and CI NEVER download Flite, an
# engine binary, a voice, a model or license-gated data. For an authorized
# local real-CPU build, provide each EXACT released library + SHA-256.
option(M43_ENABLE_REAL_FLITE "Build real offline Flite 2.2 helper (manual)" OFF)
option(M43_VENDOR_LICENSE_APPROVED "Explicit human voice/license approval" OFF)
set(M43_FLITE_INCLUDE_DIR "" CACHE PATH "Exact installed Flite 2.2 headers")
set(M43_FLITE_LIBRARIES "" CACHE STRING "Four absolute Flite libraries, voice first")
set(M43_FLITE_LIBRARY_SHA256 "" CACHE STRING "Four exact 64-char SHA-256 digests")
set(M43_FLITE_RELEASE_TAG "" CACHE STRING "Manually reviewed Flite release tag")
set(M43_FLITE_HEADER_SHA256 "" CACHE STRING "SHA-256 of exact installed flite.h")
set(M43_FLITE_LICENSE_FILE "" CACHE FILEPATH "Absolute reviewed Flite COPYING")
set(M43_FLITE_LICENSE_SHA256 "" CACHE STRING "SHA-256 of reviewed COPYING")
if(M43_ENABLE_REAL_FLITE)
  if(NOT M43_VENDOR_LICENSE_APPROVED)
    message(FATAL_ERROR "Real Flite requires explicit vendor/voice license approval")
  endif()
  if(NOT IS_ABSOLUTE "${M43_FLITE_INCLUDE_DIR}" OR
     NOT EXISTS "${M43_FLITE_INCLUDE_DIR}/flite.h")
    message(FATAL_ERROR "A pinned installed Flite 2.2 header directory is required")
  endif()
  if(NOT M43_FLITE_RELEASE_TAG STREQUAL "2.2")
    message(FATAL_ERROR "Real helper requires explicitly reviewed Flite release 2.2")
  endif()
  if(NOT IS_ABSOLUTE "${M43_FLITE_LICENSE_FILE}" OR
     NOT EXISTS "${M43_FLITE_LICENSE_FILE}")
    message(FATAL_ERROR "Exact installed Flite and voice license evidence required")
  endif()
  # These digests are independently supplied by the reviewer. They prove
  # local bytes, NOT by themselves that a binary came from the declared tag.
  foreach(label IN ITEMS HEADER LICENSE)
    if(label STREQUAL "HEADER")
      set(file "${M43_FLITE_INCLUDE_DIR}/flite.h")
      set(want "${M43_FLITE_HEADER_SHA256}")
    else()
      set(file "${M43_FLITE_LICENSE_FILE}")
      set(want "${M43_FLITE_LICENSE_SHA256}")
    endif()
    string(LENGTH "${want}" digest_len)
    if(NOT digest_len EQUAL 64 OR NOT want MATCHES "^[0-9a-fA-F]+$")
      message(FATAL_ERROR "Missing valid independent Flite ${label} SHA-256")
    endif()
    file(SHA256 "${file}" actual)
    if(NOT actual STREQUAL want)
      message(FATAL_ERROR "Flite ${label} provenance digest mismatch")
    endif()
  endforeach()
  unset(file)
  unset(want)
  unset(actual)
  list(LENGTH M43_FLITE_LIBRARIES nlibs)
  list(LENGTH M43_FLITE_LIBRARY_SHA256 nsha)
  if(NOT nlibs EQUAL 4 OR NOT nsha EQUAL 4)
    message(FATAL_ERROR "Provide four exact Flite libraries and four digests")
  endif()
  foreach(i RANGE 0 3)
    list(GET M43_FLITE_LIBRARIES ${i} lib)
    list(GET M43_FLITE_LIBRARY_SHA256 ${i} sha)
    string(LENGTH "${sha}" sha_length)
    if(NOT IS_ABSOLUTE "${lib}" OR NOT EXISTS "${lib}" OR
       NOT sha_length EQUAL 64 OR NOT sha MATCHES "^[0-9a-fA-F]+$")
      message(FATAL_ERROR "Flite library/digest ${i} invalid")
    endif()
    file(SHA256 "${lib}" actual)
    if(NOT actual STREQUAL sha)
      message(FATAL_ERROR "Flite exact library SHA-256 mismatch for ${lib}")
    endif()
  endforeach()
  add_executable(turbo_m43_flite_real_helper src/flite_native_helper.c)
  target_compile_features(turbo_m43_flite_real_helper PRIVATE c_std_11)
  set_target_properties(turbo_m43_flite_real_helper PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_include_directories(turbo_m43_flite_real_helper PRIVATE
    src include ../include "${M43_FLITE_INCLUDE_DIR}")
  target_link_libraries(turbo_m43_flite_real_helper PRIVATE
    ${M43_FLITE_LIBRARIES} m)
  target_compile_options(turbo_m43_flite_real_helper PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
