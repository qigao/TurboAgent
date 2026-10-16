cmake_minimum_required(VERSION 3.25)
project(TurboAgentOriginalSaltsCaptureSourceTSan LANGUAGES C CXX)
include(CTest)
enable_testing()

# Original immutable SaltsUtils v4.3.0-rc.11 Linux Capture sources and the
# genuine upstream miniaudio v0.11.22 backend are compiled with TSan.
# No fake salts_capture_* functions and no physical devices/fallback.
if(NOT DEFINED M42_CAPTURE_SOURCE_ROOT OR
   NOT EXISTS "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_linux.c" OR
   NOT EXISTS "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_audio_miniaudio.c" OR
   NOT EXISTS "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_screen_linux.c" OR
   NOT EXISTS "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_video_linux.c" OR
   NOT EXISTS "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_video.c" OR
   NOT DEFINED M42_MINIAUDIO_SOURCE_ROOT OR
   NOT EXISTS "${M42_MINIAUDIO_SOURCE_ROOT}/miniaudio.h" OR
   NOT DEFINED M42_M2_SOURCE_ROOT OR
   NOT EXISTS "${M42_M2_SOURCE_ROOT}/speech_input/capture/src/turbo_agent_speech_capture_salts.c" OR
   NOT DEFINED M42_CURRENT_SOURCE OR
   NOT EXISTS "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/tests/virtual_capture/test_virtual_pulse_native_capture.c")
  message(FATAL_ERROR "Missing unchanged pinned original Capture, miniaudio, and M1/M2 sources")
endif()
if(NOT DEFINED ENV{SALTS_ROOT} OR
   NOT IS_DIRECTORY "$ENV{SALTS_ROOT}" OR
   NOT DEFINED ENV{SALTS_UTILS_ROOT} OR
   NOT IS_DIRECTORY "$ENV{SALTS_UTILS_ROOT}")
  message(FATAL_ERROR "Published exact Salts/SaltsUtils native ABI roots required")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Original Capture source TSan is Linux-specific")
endif()

find_package(Salts CONFIG REQUIRED
  PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
find_package(X11 REQUIRED COMPONENTS Xext)
if(NOT TARGET Salts::Core OR NOT TARGET Salts::Platform)
  message(FATAL_ERROR "Published Salts::Core/Platform native ABI is required")
endif()

# Match the original Linux SaltsUtils Capture production source closure.
# Audio, video and screen dispatcher definitions are all linked, not replaced
# with simplified test stubs. Only the audited virtual AUDIO ID may be opened.
add_library(m42_original_capture_source_tsan SHARED
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_video.c"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/miniaudio_impl.c"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_audio_miniaudio.c"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_linux.c"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_video_linux.c"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/capture_screen_linux.c")
set_target_properties(m42_original_capture_source_tsan PROPERTIES
  OUTPUT_NAME salts_capture
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET default)
target_compile_definitions(m42_original_capture_source_tsan PRIVATE
  SALTS_CAPTURE_BUILD SALTS_CAPTURE_SHARED SALTS_HAS_PIPEWIRE SALTS_HAS_X11)
target_include_directories(m42_original_capture_source_tsan PRIVATE
  "${M42_CAPTURE_SOURCE_ROOT}/capture/include"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src"
  "${M42_MINIAUDIO_SOURCE_ROOT}"
  "$ENV{SALTS_UTILS_ROOT}/include"
  "$ENV{SALTS_ROOT}/include"
  ${X11_INCLUDE_DIR})
target_link_libraries(m42_original_capture_source_tsan PRIVATE
  Salts::Core Threads::Threads PkgConfig::PIPEWIRE
  ${X11_LIBRARIES} ${X11_Xext_LIB} "${CMAKE_DL_LIBS}" m)
# Only suppress upstream miniaudio compiler warnings, never TSan reports.
set_source_files_properties(
  "${M42_CAPTURE_SOURCE_ROOT}/capture/src/miniaudio_impl.c"
  PROPERTIES COMPILE_OPTIONS "-w")

# The immutable original M1/M2 C11 build targets remain unchanged.
set(ENABLE_SPEECH_CAPTURE_BRIDGE ON CACHE BOOL "Original M2 bridge" FORCE)
set(ENABLE_SPEECH_CAPTURE_NATIVE OFF CACHE BOOL "No implicit mic" FORCE)
add_subdirectory("${M42_M2_SOURCE_ROOT}/speech_input"
  "${CMAKE_BINARY_DIR}/original-m1-m2")

add_library(m42_original_capture_native_adapter_tsan STATIC
  "${M42_M2_SOURCE_ROOT}/speech_input/capture/src/turbo_agent_speech_capture_salts.c")
set_target_properties(m42_original_capture_native_adapter_tsan PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(m42_original_capture_native_adapter_tsan PRIVATE
  "${M42_M2_SOURCE_ROOT}/speech_input/capture/include"
  "${M42_M2_SOURCE_ROOT}/speech_input/include"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/include"
  "$ENV{SALTS_UTILS_ROOT}/include")
target_link_libraries(m42_original_capture_native_adapter_tsan PUBLIC
  turbo_agent_speech_capture_bridge
  PRIVATE m42_original_capture_source_tsan)
target_compile_options(m42_original_capture_native_adapter_tsan PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

# This is the same strict #203 device-ID audit and true callback hold vs stop.
# The dynamic source Capture DSO is the ONLY actual OS Capture owner.
add_executable(test_m42_original_capture_source_tsan
  "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/tests/virtual_capture/test_virtual_pulse_native_capture.c")
set_target_properties(test_m42_original_capture_source_tsan PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_m42_original_capture_source_tsan PRIVATE
  "${M42_M2_SOURCE_ROOT}/speech_input/include"
  "${M42_M2_SOURCE_ROOT}/speech_input/capture/include"
  "${M42_CAPTURE_SOURCE_ROOT}/capture/include"
  "$ENV{SALTS_UTILS_ROOT}/include")
target_link_libraries(test_m42_original_capture_source_tsan PRIVATE
  m42_original_capture_native_adapter_tsan
  m42_original_capture_source_tsan
  Salts::Platform Threads::Threads)
target_compile_options(test_m42_original_capture_source_tsan PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(test_m42_original_capture_source_tsan PRIVATE
  M42_CAPTURE_SOURCE_TSAN=1)
add_test(NAME m42_original_capture_source_tsan_virtual_start_stop
  COMMAND test_m42_original_capture_source_tsan)
set_tests_properties(m42_original_capture_source_tsan_virtual_start_stop
  PROPERTIES TIMEOUT 40)
