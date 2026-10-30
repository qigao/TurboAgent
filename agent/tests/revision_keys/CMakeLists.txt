cmake_minimum_required(VERSION 3.20)

# This exact test/source pair also runs without SDKs or header stubs. It checks
# the private identity format only, not TurboDB persistence or Agent ACLs.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(TurboAgentRevisionKeys LANGUAGES C)
  enable_testing()
endif()

add_executable(test_turbo_agent_revision_keys
  revision_keys_test.c ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_revision_keys PRIVATE ../../src)
set_target_properties(test_turbo_agent_revision_keys PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_revision_keys PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_revision_keys PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_revision_keys COMMAND test_turbo_agent_revision_keys)
set_tests_properties(test_turbo_agent_revision_keys PROPERTIES TIMEOUT 20)

add_executable(test_turbo_agent_revision_generation
  revision_generation_test.c ../../src/turbo_agent_revision_generation.c
  ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_revision_generation PRIVATE ../../src)
set_target_properties(test_turbo_agent_revision_generation PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_revision_generation PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_revision_generation PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_revision_generation COMMAND test_turbo_agent_revision_generation)
set_tests_properties(test_turbo_agent_revision_generation PROPERTIES TIMEOUT 20)

add_executable(test_turbo_agent_revision_read
  revision_read_test.c ../../src/turbo_agent_revision_read.c
  ../../src/turbo_agent_revision_generation.c ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_revision_read PRIVATE ../../src)
set_target_properties(test_turbo_agent_revision_read PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_revision_read PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_revision_read PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_revision_read COMMAND test_turbo_agent_revision_read)
set_tests_properties(test_turbo_agent_revision_read PROPERTIES TIMEOUT 20)

add_executable(test_turbo_agent_revision_write
  revision_write_test.c ../../src/turbo_agent_revision_write.c
  ../../src/turbo_agent_legacy_provenance.c
  ../../src/turbo_agent_legacy_knowledge_migration.c
  ../../src/turbo_agent_source_revision_record.c
  ../../src/turbo_agent_revision_generation.c ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_revision_write PRIVATE ../../src)
set_target_properties(test_turbo_agent_revision_write PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_revision_write PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_revision_write PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_revision_write COMMAND test_turbo_agent_revision_write)
set_tests_properties(test_turbo_agent_revision_write PROPERTIES TIMEOUT 20)

add_executable(test_turbo_agent_source_revision_record
  source_revision_record_test.c ../../src/turbo_agent_source_revision_record.c)
target_include_directories(test_turbo_agent_source_revision_record PRIVATE ../../src)
set_target_properties(test_turbo_agent_source_revision_record PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_source_revision_record PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_source_revision_record PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_source_revision_record COMMAND test_turbo_agent_source_revision_record)
set_tests_properties(test_turbo_agent_source_revision_record PROPERTIES TIMEOUT 20)

add_executable(test_turbo_agent_legacy_knowledge_migration
  legacy_knowledge_migration_test.c ../../src/turbo_agent_legacy_knowledge_migration.c
  ../../src/turbo_agent_source_revision_record.c ../../src/turbo_agent_revision_generation.c
  ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_legacy_knowledge_migration PRIVATE ../../src)
set_target_properties(test_turbo_agent_legacy_knowledge_migration PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_legacy_knowledge_migration PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_legacy_knowledge_migration PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_legacy_knowledge_migration COMMAND test_turbo_agent_legacy_knowledge_migration)
set_tests_properties(test_turbo_agent_legacy_knowledge_migration PROPERTIES TIMEOUT 20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  find_package(SQLite3 REQUIRED)
  set(_ta_revision_sqlite_target SQLite::SQLite3)
elseif(TARGET unofficial::sqlite3::sqlite3)
  set(_ta_revision_sqlite_target unofficial::sqlite3::sqlite3)
endif()
if(_ta_revision_sqlite_target)
  add_executable(test_turbo_agent_legacy_knowledge_sqlite
    legacy_knowledge_sqlite_test.c ../../src/turbo_agent_legacy_knowledge_sqlite.c
    ../../src/turbo_agent_legacy_knowledge_migration.c ../../src/turbo_agent_source_revision_record.c
    ../../src/turbo_agent_revision_generation.c ../../src/turbo_agent_revision_keys.c)
  target_include_directories(test_turbo_agent_legacy_knowledge_sqlite PRIVATE ../../src)
  target_link_libraries(test_turbo_agent_legacy_knowledge_sqlite PRIVATE ${_ta_revision_sqlite_target})
  set_target_properties(test_turbo_agent_legacy_knowledge_sqlite PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(test_turbo_agent_legacy_knowledge_sqlite PRIVATE /W4 /WX)
  else()
    target_compile_options(test_turbo_agent_legacy_knowledge_sqlite PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Werror)
  endif()
  add_test(NAME test_turbo_agent_legacy_knowledge_sqlite
    COMMAND test_turbo_agent_legacy_knowledge_sqlite
      "${CMAKE_CURRENT_BINARY_DIR}/legacy-knowledge.db")
  set_tests_properties(test_turbo_agent_legacy_knowledge_sqlite PROPERTIES TIMEOUT 20)
endif()

# Exercise exact-source legacy provenance and single-owner publication with
# the real private C11 production codecs. No generated consumer/installer.
add_executable(test_turbo_agent_legacy_provenance
  legacy_provenance_test.c ../../src/turbo_agent_legacy_provenance.c
  ../../src/turbo_agent_legacy_knowledge_migration.c
  ../../src/turbo_agent_revision_write.c
  ../../src/turbo_agent_source_revision_record.c
  ../../src/turbo_agent_revision_generation.c ../../src/turbo_agent_revision_keys.c)
target_include_directories(test_turbo_agent_legacy_provenance PRIVATE ../../src)
set_target_properties(test_turbo_agent_legacy_provenance PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
  target_compile_options(test_turbo_agent_legacy_provenance PRIVATE /W4 /WX)
else()
  target_compile_options(test_turbo_agent_legacy_provenance PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
add_test(NAME test_turbo_agent_legacy_provenance
  COMMAND test_turbo_agent_legacy_provenance)
set_tests_properties(test_turbo_agent_legacy_provenance PROPERTIES TIMEOUT 20)
