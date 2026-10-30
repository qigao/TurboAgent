# A custom main is required: crash workers intentionally skip every destructor.
# CTest checks the exact exit code, then launches independent restart verifiers.
add_executable(tool_journal_process_fixture tool_journal_process_fixture.c)
target_link_libraries(tool_journal_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(tool_journal_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")

foreach(cut IN ITEMS before_planned after_planned before_admitted after_admitted
    before_started after_started after_effect before_committed after_committed
    before_delivery)
  add_test(NAME test_tool_journal_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:tool_journal_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_process_crash.cmake")
  set_tests_properties(test_tool_journal_crash_${cut} PROPERTIES
    TIMEOUT 90 LABELS "agent;recovery;process")
endforeach()

# Separate OS-process SQLite owner contention followed by a fresh-process
# Tool Journal replay. This does not permit concurrent SQLite writers.
add_executable(sqlite_owner_process_fixture sqlite_owner_process_fixture.c)
target_link_libraries(sqlite_owner_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(sqlite_owner_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")
foreach(profile IN ITEMS normal crash_after_effect)
  if(profile STREQUAL "normal")
    set(test_name test_tool_journal_sqlite_cross_process_owner)
  else()
    set(test_name test_tool_journal_sqlite_cross_process_owner_crash)
  endif()
  add_test(NAME ${test_name}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:sqlite_owner_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/sqlite_owner_cases"
      "-DCASE=${profile}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_sqlite_owner_process.cmake")
  set_tests_properties(${test_name} PROPERTIES
    TIMEOUT 60 LABELS "agent;recovery;sqlite;process;owner")
endforeach()

add_executable(session_inbox_process_fixture session_inbox_process_fixture.c)
target_link_libraries(session_inbox_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(session_inbox_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")
foreach(cut IN ITEMS after_claim in_model before_applied after_applied
    before_delivery after_following_run)
  add_test(NAME test_session_inbox_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:session_inbox_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/inbox_cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_inbox_process_crash.cmake")
  set_tests_properties(test_session_inbox_crash_${cut} PROPERTIES
    TIMEOUT 90 LABELS "agent;inbox;recovery;process")
endforeach()
