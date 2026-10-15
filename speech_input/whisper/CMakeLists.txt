cmake_minimum_required(VERSION 3.20)

# An installed, exact Whisper C API package is required only when explicitly
# opted in. It is NEVER fetched, built, or substituted by normal TurboAgent.
if(NOT DEFINED ENV{TURBO_WHISPER_SDK_ROOT} OR
   NOT IS_DIRECTORY "$ENV{TURBO_WHISPER_SDK_ROOT}")
  message(FATAL_ERROR
    "ENABLE_SPEECH_WHISPER requires explicit TURBO_WHISPER_SDK_ROOT; no inference fallback")
endif()

find_package(whisper 1.9.5 EXACT CONFIG REQUIRED
  PATHS "$ENV{TURBO_WHISPER_SDK_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT TARGET whisper)
  message(FATAL_ERROR "Selected whisper 1.9.5 SDK has no C API library target")
endif()

# Unlike M1/M2, this optional Provider legitimately uses Salts for the
# provider-neutral streaming SHA-256 model provenance contract.
if(TURBO_SPEECH_STANDALONE)
  if(NOT DEFINED ENV{SALTS_ROOT} OR NOT IS_DIRECTORY "$ENV{SALTS_ROOT}")
    message(FATAL_ERROR "Standalone Whisper Provider requires an installed Salts SDK")
  endif()
  find_package(Salts CONFIG REQUIRED
    PATHS "$ENV{SALTS_ROOT}"
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()
if(NOT TARGET Salts::Core)
  message(FATAL_ERROR "Whisper Provider requires the exact installed Salts::Core SHA-256 producer")
endif()
# cmeta_crypto.c and the exported cmeta_sha256_stream_* symbols are owned by
# Salts::Core (utils/src), NOT by Salts::CMeta's reflection archive.
# Link the real defining DSO and keep Linux -z defs fail-closed.

# Not part of TurboAgentTargets: whisper is an explicitly owned optional
# Provider image. Exporting a C++ GGML dependency into the ordinary TurboAgent
# C11 installed SDK would violate the default provider-neutral boundary.
add_library(turbo_agent_speech_whisper SHARED
  src/turbo_agent_speech_whisper.c)
add_library(TurboAgent::SpeechWhisper ALIAS turbo_agent_speech_whisper)
target_compile_features(turbo_agent_speech_whisper PRIVATE c_std_11)
set_target_properties(turbo_agent_speech_whisper PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_compile_definitions(turbo_agent_speech_whisper PRIVATE
  TURBO_AGENT_SPEECH_WHISPER_BUILD)
target_include_directories(turbo_agent_speech_whisper PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(turbo_agent_speech_whisper
  PRIVATE whisper Salts::Core turbo_agent_speech_input)
if(MSVC)
  target_compile_options(turbo_agent_speech_whisper PRIVATE
    /experimental:c11atomics)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(turbo_agent_speech_whisper PRIVATE
    "LINKER:-z,defs")
endif()

if(NOT TURBO_SPEECH_STANDALONE)
  include(GNUInstallDirs)
  install(TARGETS turbo_agent_speech_whisper
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/speech-provider
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/speech-provider)
  install(FILES include/turbo_agent_speech_whisper.h
    DESTINATION include)
endif()

if(BUILD_TESTING)
  add_executable(test_turbo_agent_speech_whisper
    tests/test_turbo_agent_speech_whisper.c)
  target_compile_features(test_turbo_agent_speech_whisper PRIVATE c_std_11)
  target_link_libraries(test_turbo_agent_speech_whisper PRIVATE
    turbo_agent_speech_whisper Salts::Core)
  set_target_properties(test_turbo_agent_speech_whisper PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME turbo_agent_speech_whisper_negative
    COMMAND test_turbo_agent_speech_whisper)
  set_tests_properties(turbo_agent_speech_whisper_negative
    PROPERTIES TIMEOUT 30)
endif()

# Deterministic positive Provider end-to-end conformance. Compile the SAME
# production C11 source, but bind only a fake v1.9.5 C ABI model engine.
# This does NOT assert real acoustic model performance.
if(BUILD_TESTING AND UNIX)
  find_package(Threads REQUIRED)
  add_executable(test_turbo_agent_speech_whisper_mock
    src/turbo_agent_speech_whisper.c
    tests/test_turbo_agent_speech_whisper_mock.c)
  target_include_directories(test_turbo_agent_speech_whisper_mock PRIVATE
    ${WHISPER_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  target_compile_features(test_turbo_agent_speech_whisper_mock PRIVATE c_std_11)
  set_target_properties(test_turbo_agent_speech_whisper_mock PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_turbo_agent_speech_whisper_mock PRIVATE
    turbo_agent_speech_input Salts::Core Threads::Threads)
  add_test(NAME turbo_agent_speech_whisper_mock_provider
    COMMAND test_turbo_agent_speech_whisper_mock)
  set_tests_properties(turbo_agent_speech_whisper_mock_provider PROPERTIES
    TIMEOUT 30)
endif()

# The first user-facing offline transcription adapter is explicitly selected.
# It needs the M2 strict S16LE capture bridge for WAV frames but never opens
# a microphone, downloads weights, submits an Agent Session or calls tools.
option(ENABLE_SPEECH_WHISPER_CLI
  "Build optional verified-model offline WAV-to-transcript preview" OFF)
if(ENABLE_SPEECH_WHISPER_CLI)
  if(NOT TARGET turbo_agent_speech_capture_bridge)
    message(FATAL_ERROR
      "Whisper WAV CLI requires ENABLE_SPEECH_CAPTURE_BRIDGE=ON; no duplicate S16LE adapter")
  endif()
  add_executable(turboagent-whisper-transcribe
    tools/turboagent_whisper_transcribe.c)
  target_compile_features(turboagent-whisper-transcribe PRIVATE c_std_11)
  set_target_properties(turboagent-whisper-transcribe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_link_libraries(turboagent-whisper-transcribe PRIVATE
    turbo_agent_speech_whisper turbo_agent_speech_capture_bridge)
  if(NOT TURBO_SPEECH_STANDALONE)
    include(GNUInstallDirs)
    install(TARGETS turboagent-whisper-transcribe
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(BUILD_TESTING)
    add_test(NAME turbo_agent_speech_whisper_cli_reject_missing_arguments
      COMMAND turboagent-whisper-transcribe)
    set_tests_properties(
      turbo_agent_speech_whisper_cli_reject_missing_arguments PROPERTIES
      WILL_FAIL TRUE TIMEOUT 5)
  endif()
endif()


# Real weights and provider callbacks are never introduced in normal CTest.
# This standalone strict-C11 ELF host builds without a model and never links
# directly to whisper or the production Provider. Its six actual-inference
# scenarios are invoked ONLY from the separately authorized label-only job.
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  add_executable(test_turbo_agent_speech_whisper_real_model_dso
    tests/test_speech_whisper_real_model_dso.c)
  target_compile_features(test_turbo_agent_speech_whisper_real_model_dso
    PRIVATE c_std_11)
  target_include_directories(test_turbo_agent_speech_whisper_real_model_dso
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(test_turbo_agent_speech_whisper_real_model_dso
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  target_link_libraries(test_turbo_agent_speech_whisper_real_model_dso
    PRIVATE turbo_agent_speech_input Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(test_turbo_agent_speech_whisper_real_model_dso
    PRIVATE "LINKER:-z,defs")
endif()
