cmake_minimum_required(VERSION 3.27)
project(TurboAgentM42VoiceInstalledConsumer LANGUAGES C)
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "M4.2 requires pinned published Salts SDK environment")
endif()
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Playback)
  message(FATAL_ERROR "Published SDK does not export Salts::Playback")
endif()
if(NOT DEFINED TTS_M42_BUILD_ROOT)
  message(FATAL_ERROR "M4.2 exact-head build root required")
endif()
set(_voice "${TTS_M42_BUILD_ROOT}/voice_coordinator/libturbo_tts_voice_coordinator.a")
set(_bridge "${TTS_M42_BUILD_ROOT}/playback/libturbo_tts_playback_bridge.a")
set(_source "${TTS_M42_BUILD_ROOT}/libturbo_agent_speech_output.a")
foreach(archive IN ITEMS "${_voice}" "${_bridge}" "${_source}")
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "M4.2 exact-head coordinator/bridge/source archive missing: ${archive}")
  endif()
endforeach()
add_executable(tts_voice_release_link verify_installed_voice.c)
target_compile_features(tts_voice_release_link PRIVATE c_std_11)
target_compile_options(tts_voice_release_link PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_include_directories(tts_voice_release_link PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../playback/include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../include")
# Force current HEAD coordinator->bridge->SpeechOutput and the ACTUAL released
# playback target. No fake symbols or device open in this linked smoke.
target_link_libraries(tts_voice_release_link PRIVATE
  "${_voice}" "${_bridge}" "${_source}" Salts::Playback)
