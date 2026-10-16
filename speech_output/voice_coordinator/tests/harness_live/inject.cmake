# Included ONLY by the independently pinned TurboAgent 1.2.0 source
# project via -DCMAKE_PROJECT_TurboAgent_INCLUDE. It does not rewrite the
# original CMakeLists nor copy/link a test Harness implementation.
if(NOT DEFINED VOICE_M42_WORKSPACE OR
   NOT EXISTS "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness_live/CMakeLists.txt")
  message(FATAL_ERROR "M4.2 genuine Harness test source root not explicitly verified")
endif()
if(NOT DEFINED VOICE_M42_PREBUILT_ROOT OR
   NOT EXISTS "${VOICE_M42_PREBUILT_ROOT}/voice_coordinator/tests/harness/libturbo_voice_harness_final_gate.a")
  message(FATAL_ERROR "M4.2 current-head built Voice/Harness gate archive missing")
endif()
if(NOT DEFINED VOICE_M42_PINNED_HARNESS_SHA OR
   NOT VOICE_M42_PINNED_HARNESS_SHA STREQUAL
       "9e51d377bd84ee91ce5a47c94f24f73e4e01bba7")
  message(FATAL_ERROR "M4.2 actual Harness requires exact verified upstream SHA")
endif()
# This callback runs after the original root project() but defers test target
# creation until the REAL Harness, Agent and Salts targets have been declared.
cmake_language(DEFER CALL add_subdirectory
  "${VOICE_M42_WORKSPACE}/speech_output/voice_coordinator/tests/harness_live"
  "${CMAKE_BINARY_DIR}/m42-live-harness")
