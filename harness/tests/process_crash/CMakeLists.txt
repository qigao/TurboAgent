# CLI modes and _Exit require a custom main, as in the Agent process fixture.
add_executable(compiled_inspect_process_fixture compiled_inspect_process_fixture.c)
target_link_libraries(compiled_inspect_process_fixture PRIVATE TurboAgent::Harness)
set_target_properties(compiled_inspect_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Harness/tests")

if(BUILD_TESTS)
  foreach(cut IN ITEMS before_claim after_claim before_run_started after_run_started
      in_callback before_committed after_committed after_run_committed before_delivery)
    add_test(NAME test_compiled_inspect_crash_${cut}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
        "-DCUT=${cut}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_process_crash.cmake")
    set_tests_properties(test_compiled_inspect_crash_${cut} PROPERTIES
      TIMEOUT 90 LABELS "harness;recovery;process")
  endforeach()
endif()

foreach(cut IN ITEMS delivery_intent_before delivery_intent_after delivery_intent_lost_ack
    delivery_intent_returned delivery_after_send delivery_receipt_before delivery_receipt_after
    delivery_receipt_lost_ack delivery_receipt_returned)
  if(BUILD_TESTS)
    add_test(NAME test_job_${cut}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
        "-DCUT=${cut}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_job_delivery_crash.cmake")
    set_tests_properties(test_job_${cut} PROPERTIES
      TIMEOUT 90 LABELS "harness;job;delivery;recovery;process")
    add_test(NAME test_job_verified_${cut}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
        "-DCUT=${cut}" "-DMODE=verified"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_job_delivery_crash.cmake")
    set_tests_properties(test_job_verified_${cut} PROPERTIES
      TIMEOUT 90 LABELS "harness;job;verification;delivery;recovery;process")
  endif()
  add_test(NAME test_job_files_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}" "-DMODE=files"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_job_delivery_crash.cmake")
  set_tests_properties(test_job_files_${cut} PROPERTIES
    TIMEOUT 90 LABELS "harness;job;file-verification;delivery;recovery;process")
endforeach()

if(NOT BUILD_TESTS)
  return()
endif()

add_executable(job_process_fixture job_process_fixture.c)
target_link_libraries(job_process_fixture PRIVATE TurboAgent::Harness)
set_target_properties(job_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Harness/tests")
foreach(cut IN ITEMS root_before root_after proposal_before proposal_after
    binding_before binding_after proposal_lost_ack binding_lost_ack)
  add_test(NAME test_job_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:job_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_job_crash.cmake")
  set_tests_properties(test_job_crash_${cut} PROPERTIES
    TIMEOUT 90 LABELS "harness;job;recovery;process")
endforeach()

# Same canonical attempt/Run crash oracle through persisted Job dispatch.
foreach(cut IN ITEMS before_claim after_claim before_run_started after_run_started
    in_callback before_committed after_committed after_run_committed before_delivery)
  add_test(NAME test_job_inspect_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}" "-DMODE=job"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_process_crash.cmake")
  set_tests_properties(test_job_inspect_crash_${cut} PROPERTIES
    TIMEOUT 90 LABELS "harness;job;recovery;process")
endforeach()

foreach(cut IN ITEMS acceptance_before acceptance_after acceptance_lost_ack acceptance_returned)
  add_test(NAME test_job_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:compiled_inspect_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_job_acceptance_crash.cmake")
  set_tests_properties(test_job_${cut} PROPERTIES
    TIMEOUT 90 LABELS "harness;job;acceptance;recovery;process")
endforeach()
