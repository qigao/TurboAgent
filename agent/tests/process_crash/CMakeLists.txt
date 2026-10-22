# A custom main is required: crash workers intentionally skip every destructor.
# CTest checks the exact exit code, then launches independent restart verifiers.
add_executable(tool_journal_process_fixture tool_journal_process_fixture.c)
target_link_libraries(tool_journal_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(tool_journal_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")

foreach(cut IN ITEMS before_planned after_planned before_started after_started
    after_effect before_committed after_committed before_delivery)
  add_test(NAME test_tool_journal_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:tool_journal_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_process_crash.cmake")
  set_tests_properties(test_tool_journal_crash_${cut} PROPERTIES
    TIMEOUT 90 LABELS "agent;recovery;process")
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
