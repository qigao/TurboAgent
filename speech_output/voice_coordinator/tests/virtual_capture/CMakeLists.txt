cmake_minimum_required(VERSION 3.25)
project(TurboAgentPublishedSaltsVirtualCapture C CXX)
include(CTest)
enable_testing()

# Original immutable M1/M2 code, current HEAD owns only this test executable.
# The REAL installed SaltsUtils Capture DSO is the SOLE OS device owner.
if(NOT DEFINED M42_PINNED_M2_SOURCE OR
   NOT EXISTS "${M42_PINNED_M2_SOURCE}/speech_input/CMakeLists.txt" OR
   NOT EXISTS "${M42_PINNED_M2_SOURCE}/speech_input/capture/src/turbo_agent_speech_capture_salts.c")
  message(FATAL_ERROR "Missing unchanged SHA-pinned M1/M2 native Salts adapter sources")
endif()
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "Pinned released Salts and SaltsUtils SDK roots required")
endif()
find_package(Salts CONFIG REQUIRED
  PATHS "$ENV{SALTS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Capture OR NOT TARGET Salts::Platform)
  message(FATAL_ERROR "Original published Capture/Platform target unresolved")
endif()
find_package(Threads REQUIRED)
set(ENABLE_SPEECH_CAPTURE_BRIDGE ON CACHE BOOL "Only original M1/M2" FORCE)
set(ENABLE_SPEECH_CAPTURE_NATIVE OFF CACHE BOOL "Never implicit physical mic" FORCE)
add_subdirectory("${M42_PINNED_M2_SOURCE}/speech_input"
  "${CMAKE_CURRENT_BINARY_DIR}/pinned-original-speech")

# Build the ORIGINAL native Salts adapter from immutable M2 source. This is
# the very adapter M2 production builds when explicitly enabled, not a fake
# Capture implementation; it retains its single bounded existing M1 queue.
add_library(m42_original_m2_capture_salts STATIC
  "${M42_PINNED_M2_SOURCE}/speech_input/capture/src/turbo_agent_speech_capture_salts.c")
target_compile_features(m42_original_m2_capture_salts PRIVATE c_std_11)
set_target_properties(m42_original_m2_capture_salts PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(m42_original_m2_capture_salts PUBLIC
  turbo_agent_speech_capture_bridge PRIVATE Salts::Capture)
target_compile_options(m42_original_m2_capture_salts PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

add_executable(test_m42_published_pulse_virtual_capture
  test_virtual_pulse_native_capture.c)
target_compile_features(test_m42_published_pulse_virtual_capture PRIVATE c_std_11)
set_target_properties(test_m42_published_pulse_virtual_capture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(test_m42_published_pulse_virtual_capture PRIVATE
  m42_original_m2_capture_salts Salts::Capture Salts::Platform Threads::Threads)
target_compile_options(test_m42_published_pulse_virtual_capture PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m42_published_pulse_virtual_capture_start_stop_join
  COMMAND test_m42_published_pulse_virtual_capture)
set_tests_properties(m42_published_pulse_virtual_capture_start_stop_join
  PROPERTIES TIMEOUT 35)

# Unlike #202's test-null source Playback, the following executable imports
# the ACTUAL published SaltsUtils Native Salts::Playback DSO. M4 C11 source
# remains from this HEAD. The null SINK is an external isolated Pulse server,
# not a compile-time substitute for the production libsalts_playback.so.
if(NOT TARGET Salts::Playback)
  message(FATAL_ERROR "Official published Salts::Playback DSO target missing")
endif()
if(NOT DEFINED M42_CURRENT_SOURCE OR
   NOT EXISTS "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/src/turbo_agent_voice_coordinator.c")
  message(FATAL_ERROR "Current HEAD M4.0/M4.1/M4.2 C11 source must be explicit")
endif()
add_library(m42_published_playback_voice_coordinator STATIC
  "${M42_CURRENT_SOURCE}/speech_output/src/turbo_agent_speech_output.c"
  "${M42_CURRENT_SOURCE}/speech_output/playback/src/turbo_agent_tts_playback_bridge.c"
  "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/src/turbo_agent_voice_coordinator.c")
set_target_properties(m42_published_playback_voice_coordinator PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(m42_published_playback_voice_coordinator PUBLIC
  "${M42_CURRENT_SOURCE}/speech_output/include"
  "${M42_CURRENT_SOURCE}/speech_output/playback/include"
  "${M42_CURRENT_SOURCE}/speech_output/voice_coordinator/include")
target_compile_options(m42_published_playback_voice_coordinator PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(m42_published_playback_voice_coordinator PUBLIC
  Salts::Playback)

add_executable(test_m42_published_pulse_native_duplex
  test_virtual_pulse_published_duplex.c)
set_target_properties(test_m42_published_pulse_native_duplex PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_options(test_m42_published_pulse_native_duplex PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(test_m42_published_pulse_native_duplex PRIVATE
  m42_published_playback_voice_coordinator
  m42_original_m2_capture_salts
  Salts::Capture Salts::Playback Salts::Platform Threads::Threads)
add_test(NAME m42_published_pulse_virtual_capture_playback_duplex
  COMMAND test_m42_published_pulse_native_duplex)
set_tests_properties(m42_published_pulse_virtual_capture_playback_duplex
  PROPERTIES TIMEOUT 35)
