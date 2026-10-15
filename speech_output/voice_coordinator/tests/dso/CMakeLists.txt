# M4.2.1 Linux-only REAL loadable C11 Provider + real C11 worker test.
# This is NOT linked into or installed with TurboAgent.Native.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Real DSO lifecycle witness is Linux-only")
endif()
find_package(Threads REQUIRED)

add_library(tts_m42_provider_real_dso SHARED
  tts_test_provider_dso.c)
target_compile_features(tts_m42_provider_real_dso PRIVATE c_std_11)
set_target_properties(tts_m42_provider_real_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(tts_m42_provider_real_dso PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../include")
target_link_libraries(tts_m42_provider_real_dso PRIVATE Threads::Threads)

add_executable(test_tts_m42_native_dso
  test_real_provider_dso.c)
target_compile_features(test_tts_m42_native_dso PRIVATE c_std_11)
set_target_properties(test_tts_m42_native_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(test_tts_m42_native_dso PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(test_tts_m42_native_dso PRIVATE
  turbo_tts_voice_coordinator Threads::Threads "${CMAKE_DL_LIBS}")
target_compile_definitions(test_tts_m42_native_dso PRIVATE
  TEST_TTS_DSO_PATH="$<TARGET_FILE:tts_m42_provider_real_dso>")
add_dependencies(test_tts_m42_native_dso tts_m42_provider_real_dso)

foreach(target IN ITEMS tts_m42_provider_real_dso test_tts_m42_native_dso)
  target_compile_options("${target}" PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endforeach()
add_test(NAME tts_m42_native_dso
  COMMAND test_tts_m42_native_dso)
