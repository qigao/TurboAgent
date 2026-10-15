cmake_minimum_required(VERSION 3.27)
project(TurboAgentM4InstalledPlaybackConsumer LANGUAGES C)
if(NOT DEFINED ENV{SALTS_ROOT} OR NOT DEFINED ENV{SALTS_UTILS_ROOT})
  message(FATAL_ERROR "Exact SALTS_ROOT and SALTS_UTILS_ROOT required")
endif()
find_package(SaltsUtils CONFIG REQUIRED
  PATHS "$ENV{SALTS_UTILS_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET Salts::Playback)
  message(FATAL_ERROR "Published SaltsUtils SDK lacks Salts::Playback")
endif()
if(NOT DEFINED TTS_M41_BUILD_ROOT)
  message(FATAL_ERROR "exact current-head compiled M4.1 bridge root required")
endif()
set(_tts_bridge_archive "${TTS_M41_BUILD_ROOT}/playback/libturbo_tts_playback_bridge.a")
set(_tts_core_archive "${TTS_M41_BUILD_ROOT}/libturbo_agent_speech_output.a")
if(NOT EXISTS "${_tts_bridge_archive}" OR NOT EXISTS "${_tts_core_archive}")
  message(FATAL_ERROR "cannot import current-head M4.1 bridge and core archives")
endif()
add_executable(tts_playback_release_link verify_installed_playback.c)
target_compile_features(tts_playback_release_link PRIVATE c_std_11)
target_compile_options(tts_playback_release_link PRIVATE
  -Wall -Wextra -Wpedantic -Werror)
target_include_directories(tts_playback_release_link PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../include"
  "${CMAKE_CURRENT_LIST_DIR}/../../../include")
# Force the actual compiled C11 bridge object into this executable;
# its Salts::Playback symbols must resolve to the published imported target.
target_link_libraries(tts_playback_release_link PRIVATE
  "${_tts_bridge_archive}" "${_tts_core_archive}" Salts::Playback)
