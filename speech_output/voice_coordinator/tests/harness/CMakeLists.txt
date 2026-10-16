# M4.2 isolated, opt-in Harness-to-Voice approval gate.
# Production Harness/Session headers are consumed from the full SHA-pinned
# original source; the CTest supplies Harness ABI TEST DOUBLES. Do not
# mistake this conformance test for a real Harness execution or real Agent.
if(NOT BUILD_TESTING)
  message(FATAL_ERROR "Harness final admission gate requires BUILD_TESTING")
endif()
if(NOT DEFINED SPEECH_M2_SOURCE_ROOT OR
   NOT EXISTS "${SPEECH_M2_SOURCE_ROOT}/harness/include/turbo_agent_harness.h" OR
   NOT EXISTS "${SPEECH_M2_SOURCE_ROOT}/agent/include/turbo_agent_execution.h" OR
   NOT EXISTS "${SPEECH_M2_SOURCE_ROOT}/include/turbo_agent_api.h")
  message(FATAL_ERROR "Harness final gate requires the exact original public Harness/App headers")
endif()
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "Harness gate requires exact published Salts and SaltsUtils SDKs")
endif()
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::JsonParser)
  message(FATAL_ERROR "Published SaltsUtils must export canonical JsonParser")
endif()

set(_host_original "${SPEECH_M2_SOURCE_ROOT}")
set(_host_public_paths
  "${_host_original}/include"
  "${_host_original}/harness/include"
  "${_host_original}/agent/include"
  "${_host_original}/runtime/include"
  "${_host_original}/runtime_core/include"
  "${_host_original}/runtime_data/include"
  "${_host_original}/provider/include"
  "${_host_original}/agent_compiler/include"
  "${_host_original}/runtime_tools/include"
  "${_host_original}/tools/include"
  "$ENV{SALTS_ROOT}/include"
  "$ENV{SALTS_UTILS_ROOT}/include")

add_library(turbo_voice_harness_final_gate STATIC
  src/turbo_agent_voice_harness_final_gate.c)
target_compile_features(turbo_voice_harness_final_gate PUBLIC c_std_11)
set_target_properties(turbo_voice_harness_final_gate PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(turbo_voice_harness_final_gate PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE ${_host_public_paths})
target_link_libraries(turbo_voice_harness_final_gate
  PUBLIC turbo_tts_voice_coordinator
  PRIVATE Salts::JsonParser)
target_compile_options(turbo_voice_harness_final_gate PRIVATE
  -Wall -Wextra -Wpedantic -Werror)

add_executable(test_tts_harness_final_gate_mock
  tests/test_harness_final_gate_mock.c)
target_compile_features(test_tts_harness_final_gate_mock PRIVATE c_std_11)
set_target_properties(test_tts_harness_final_gate_mock PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_tts_harness_final_gate_mock PRIVATE
  ${_host_public_paths})
target_link_libraries(test_tts_harness_final_gate_mock PRIVATE
  turbo_voice_harness_final_gate Salts::JsonParser)
target_compile_options(test_tts_harness_final_gate_mock PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
add_test(NAME tts_harness_completed_final_gate_mock
  COMMAND test_tts_harness_final_gate_mock)
set_tests_properties(tts_harness_completed_final_gate_mock
  PROPERTIES TIMEOUT 20)
