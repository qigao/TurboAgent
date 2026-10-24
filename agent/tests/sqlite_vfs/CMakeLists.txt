add_executable(tool_journal_vfs_fixture journal_vfs_fixture.c sqlite_fault_vfs.c production_sqlite_store.c)
target_link_libraries(tool_journal_vfs_fixture PRIVATE TurboAgent::Agent unofficial::sqlite3::sqlite3)
set_target_properties(tool_journal_vfs_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")

foreach(phase IN ITEMS started committed)
  foreach(fault IN ITEMS none journal_full journal_write journal_sync database_write database_sync
      journal_partial database_partial journal_write_after database_write_after
      journal_sync_after database_sync_after crash_journal_sync_before crash_journal_sync_after
      crash_database_sync_before crash_database_sync_after crash_journal_partial crash_database_partial
      journal_invalidate_before journal_invalidate_after crash_journal_invalidate_before crash_journal_invalidate_after)
    add_test(NAME test_sqlite_vfs_${phase}_${fault}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:tool_journal_vfs_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
        "-DPHASE=${phase}" "-DFAULT=${fault}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_vfs.cmake")
    set_tests_properties(test_sqlite_vfs_${phase}_${fault} PROPERTIES
      TIMEOUT 90 LABELS "agent;recovery;process;sqlite-vfs")
  endforeach()
endforeach()

foreach(phase IN ITEMS started committed)
  foreach(recovery IN ITEMS write_before write_after sync_before sync_after partial_then_sync write_error sync_error)
    add_test(NAME test_sqlite_vfs_recovery_${phase}_${recovery}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:tool_journal_vfs_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/recovery_cases"
        "-DPHASE=${phase}" "-DFAULT=crash_database_sync_after" "-DRECOVERY_FAULT=${recovery}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_vfs.cmake")
    set_tests_properties(test_sqlite_vfs_recovery_${phase}_${recovery} PROPERTIES
      TIMEOUT 90 LABELS "agent;recovery;process;sqlite-vfs;sqlite-recovery-restart")
  endforeach()
endforeach()
