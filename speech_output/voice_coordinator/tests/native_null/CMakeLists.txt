cmake_minimum_required(VERSION 3.25)
project(TurboAgentM42OriginalNativeNull C)
include(CTest)
enable_testing()

# Each source path is an explicitly checked-out original RELEASE or current
# PR HEAD. No fake salts_playback_* symbols, no hidden alternate audio owner.
if(NOT DEFINED SALTSUTILS_RELEASE_SOURCE OR
   NOT EXISTS "${SALTSUTILS_RELEASE_SOURCE}/playback/src/salts_playback.c" OR
   NOT EXISTS "${SALTSUTILS_RELEASE_SOURCE}/playback/tests/test_playback_lifecycle.c" OR
   NOT EXISTS "${SALTSUTILS_RELEASE_SOURCE}/playback/include/salts_playback.h" OR
   NOT DEFINED MINIAUDIO_RELEASE_SOURCE OR
   NOT EXISTS "${MINIAUDIO_RELEASE_SOURCE}/miniaudio.h" OR
   NOT DEFINED M42_CURRENT_SOURCE OR
   NOT EXISTS "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/src/turbo_agent_voice_coordinator.c")
  message(FATAL_ERROR "Original pinned SaltsUtils/native miniaudio/M4 sources missing")
endif()
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "Exact installed Salts and SaltsUtils Native SDKs are required")
endif()

find_package(Salts CONFIG REQUIRED
  PATHS "$ENV{SALTS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Threads REQUIRED)
if(NOT TARGET Salts::Concurrency OR NOT TARGET Salts::Platform OR
   NOT TARGET Salts::Capture)
  message(FATAL_ERROR "Missing official published Salts runtime/SaltsUtils Capture targets")
endif()

# Original SaltsUtils rc.11's OWN existing explicit null backend for tests:
# actual ma_context, ma_device, callback thread and bounded cmeta_spsc_ring.
# Critically, this is NOT a modified or fake implementation of Playback.
add_library(m42_salts_playback_original_native_null STATIC
  "${SALTSUTILS_RELEASE_SOURCE}/playback/src/miniaudio_impl.c"
  "${SALTSUTILS_RELEASE_SOURCE}/playback/src/playback_buffer.c"
  "${SALTSUTILS_RELEASE_SOURCE}/playback/src/salts_playback.c")
set_target_properties(m42_salts_playback_original_native_null PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(m42_salts_playback_original_native_null PRIVATE
  SALTS_PLAYBACK_TEST_NULL_BACKEND=1)
target_include_directories(m42_salts_playback_original_native_null PUBLIC
  "${SALTSUTILS_RELEASE_SOURCE}/playback/include"
  PRIVATE "${SALTSUTILS_RELEASE_SOURCE}/playback/src"
          "${MINIAUDIO_RELEASE_SOURCE}")
set_source_files_properties(
  "${SALTSUTILS_RELEASE_SOURCE}/playback/src/miniaudio_impl.c"
  PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(m42_salts_playback_original_native_null
  PUBLIC Salts::Concurrency Salts::Platform
  PRIVATE Threads::Threads "${CMAKE_DL_LIBS}" m)

# Source-level TSan requires the ORIGINAL SPSC cursor acquire/release
# implementation to be instrumented too.  The default imported static
# Salts::Concurrency SDK was compiled WITHOUT TSan; leaving these atomics
# opaque creates a false-positive data race between memcpy/write and
# memcpy/read despite the correctly ordered release/acquire handshake.
# Compile the exact original published Salts release C11 implementation,
# not a shim, stub, suppression or a different ring/owner.
if(M42_ORIGINAL_AUDIO_TSAN)
  if(NOT DEFINED M42_SALTS_ORIGINAL_SOURCE_ROOT OR
     NOT EXISTS "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/src/spsc_ring.c" OR
     NOT EXISTS "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/include/salts/spsc_ring.h")
    message(FATAL_ERROR "Source TSan needs exact original Salts.Native v2.3.0-rc.16 SPSC source")
  endif()
  target_sources(m42_salts_playback_original_native_null PRIVATE
    "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/src/spsc_ring.c")
  target_include_directories(m42_salts_playback_original_native_null
    BEFORE PRIVATE "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/include")
endif()

# Compile the SAME current-HEAD experimental M4.0, M4.1 and M4.2 C11
# sources; the only audio symbols come from the ORIGINAL null-native code.
add_library(m42_native_coordinator_original_source STATIC
  "${M42_CURRENT_SOURCE}/speech_output/src/turbo_agent_speech_output.c"
  "${M42_CURRENT_SOURCE}/speech_output/playback/src/turbo_agent_tts_playback_bridge.c"
  "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/src/turbo_agent_voice_coordinator.c")
set_target_properties(m42_native_coordinator_original_source PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(m42_native_coordinator_original_source PUBLIC
  "${M42_CURRENT_SOURCE}/speech_output/include"
  "${M42_CURRENT_SOURCE}/speech_output/playback/include"
  "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/include"
  "${SALTSUTILS_RELEASE_SOURCE}/playback/include")
target_link_libraries(m42_native_coordinator_original_source PUBLIC
  m42_salts_playback_original_native_null)
target_compile_options(m42_native_coordinator_original_source PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

add_executable(test_m42_original_salts_null_native
  test_real_salts_null_audio.c)
set_target_properties(test_m42_original_salts_null_native PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(test_m42_original_salts_null_native PRIVATE
  m42_native_coordinator_original_source
  m42_salts_playback_original_native_null)
target_compile_options(test_m42_original_salts_null_native PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m42_original_salts_native_null_callback_lifecycle
  COMMAND test_m42_original_salts_null_native)
set_tests_properties(m42_original_salts_native_null_callback_lifecycle
  PROPERTIES TIMEOUT 30)

# Official published Capture independently, without ever linking the
# explicit null Playback implementation into this consumer.
add_executable(test_m42_published_capture_device_rejection
  test_published_capture_no_device.c)
set_target_properties(test_m42_published_capture_device_rejection PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(test_m42_published_capture_device_rejection
  PRIVATE Salts::Capture)
target_compile_options(test_m42_published_capture_device_rejection PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m42_published_capture_native_no_permission
  COMMAND test_m42_published_capture_device_rejection)
set_tests_properties(m42_published_capture_native_no_permission
  PROPERTIES TIMEOUT 30)
# This additional shared target uses the ORIGINAL, unchanged Playback
# implementation and native PulseAudio/miniaudio backend, not the explicit
# test-null source configuration. It is built only for the SOURCE TSan
# integration with the original production Harness; never for SDK release.
# The fullstack executable still imports the published Salts::Playback
# SONAME/ABI, but in this strictly verified TSan environment the loader
# selects the same source rebuilt WITH ThreadSanitizer. The shipped binary
# remains independently tested on #205 without this source override.
option(M42_ORIGINAL_SHARED_PLAYBACK_TSAN
  "Build original Salts Playback real native backend as source-TSan shared DSO" OFF)
if(M42_ORIGINAL_SHARED_PLAYBACK_TSAN)
  if(NOT M42_ORIGINAL_AUDIO_TSAN)
    message(FATAL_ERROR "TSan Playback DSO requires original SPSC source instrumentation")
  endif()
  add_library(m42_original_playback_source_tsan_shared SHARED
    "${SALTSUTILS_RELEASE_SOURCE}/playback/src/miniaudio_impl.c"
    "${SALTSUTILS_RELEASE_SOURCE}/playback/src/playback_buffer.c"
    "${SALTSUTILS_RELEASE_SOURCE}/playback/src/salts_playback.c"
    "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/src/spsc_ring.c")
  set_target_properties(m42_original_playback_source_tsan_shared PROPERTIES
    OUTPUT_NAME salts_playback
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(m42_original_playback_source_tsan_shared
    BEFORE PRIVATE
    "${M42_SALTS_ORIGINAL_SOURCE_ROOT}/concurrency/include"
    "${MINIAUDIO_RELEASE_SOURCE}"
    "${SALTSUTILS_RELEASE_SOURCE}/playback/src"
    "${SALTSUTILS_RELEASE_SOURCE}/playback/include")
  set_source_files_properties(
    "${SALTSUTILS_RELEASE_SOURCE}/playback/src/miniaudio_impl.c"
    PROPERTIES COMPILE_OPTIONS "-w")
  target_compile_definitions(m42_original_playback_source_tsan_shared PRIVATE
    SALTS_PLAYBACK_SHARED=1 SALTS_PLAYBACK_BUILD=1)
  target_link_libraries(m42_original_playback_source_tsan_shared
    PRIVATE Salts::Concurrency Salts::Platform Threads::Threads
    "${CMAKE_DL_LIBS}" m)
endif()

