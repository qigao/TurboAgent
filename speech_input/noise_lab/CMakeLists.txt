cmake_minimum_required(VERSION 3.20)
project(TurboAgentSpeexDSPNoiseLab LANGUAGES C)
include(CTest)

# Completely isolated diagnostic: this directory is NOT included by the
# production speech_input/CMakeLists.txt or any installed/exported SDK target.
# Lack of the explicit native C library MUST fail configure (no fallback).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp)
if(NOT SPEEXDSP_VERSION STREQUAL "1.2.1")
  message(FATAL_ERROR
    "SpeexDSP diagnostic requires independently reviewed 1.2.1, found ${SPEEXDSP_VERSION}")
endif()

add_library(ta_speex_ns_candidate STATIC
  src/ta_speex_ns_candidate.c)
target_include_directories(ta_speex_ns_candidate PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ta_speex_ns_candidate PUBLIC PkgConfig::SPEEXDSP)
set_target_properties(ta_speex_ns_candidate PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ta_speex_ns_candidate PRIVATE
    -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
endif()

if(BUILD_TESTING)
  add_executable(test_ta_speex_ns_candidate
    tests/test_ta_speex_ns_candidate.c)
  target_link_libraries(test_ta_speex_ns_candidate PRIVATE
    ta_speex_ns_candidate m)
  set_target_properties(test_ta_speex_ns_candidate PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_ta_speex_ns_candidate PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
  endif()
  add_test(NAME ta_speex_ns_candidate_contract
    COMMAND test_ta_speex_ns_candidate)
  set_tests_properties(ta_speex_ns_candidate_contract
    PROPERTIES TIMEOUT 30)
endif()

# Linux-only private raw PCM diagnostic, separate from the C11 DSP core.
# It uses open(O_EXCL, mode=0600), never logs source/output paths or audio.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ta_speex_ns_raw_probe
    tools/ta_speex_ns_raw_probe.c)
  target_link_libraries(ta_speex_ns_raw_probe PRIVATE
    ta_speex_ns_candidate m)
  set_target_properties(ta_speex_ns_raw_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ta_speex_ns_raw_probe PRIVATE
      -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion)
  endif()
endif()
