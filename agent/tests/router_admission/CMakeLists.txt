add_executable(router_admission_process_fixture router_admission_fixture.c)
target_link_libraries(router_admission_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(router_admission_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")
foreach(cut IN ITEMS before_write after_write lost_ack queue_rejected in_model before_delivery after_observation)
  add_test(NAME test_router_admission_crash_${cut}
    COMMAND "${CMAKE_COMMAND}"
      "-DWORKER=$<TARGET_FILE:router_admission_process_fixture>"
      "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/cases"
      "-DCUT=${cut}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run_admission.cmake")
  set_tests_properties(test_router_admission_crash_${cut} PROPERTIES
    TIMEOUT 60 LABELS "agent;recovery;process;router-admission")
endforeach()

add_executable(router_group_process_fixture router_group_fixture.c)
target_link_libraries(router_group_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(router_group_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")
foreach(cut IN ITEMS before_write after_write lost_ack after_handoff after_delivery)
  foreach(kind IN ITEMS input collect)
    add_test(NAME test_router_group_crash_${cut}_${kind}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:router_group_process_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/group_cases"
        "-DCUT=${cut}" "-DKIND=${kind}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_group.cmake")
    set_tests_properties(test_router_group_crash_${cut}_${kind} PROPERTIES
      TIMEOUT 90 LABELS "agent;recovery;process;router-group-observation")
  endforeach()
endforeach()

add_executable(router_review_process_fixture router_review_fixture.c)
target_link_libraries(router_review_process_fixture PRIVATE TurboAgent::Agent)
set_target_properties(router_review_process_fixture PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF FOLDER "Agent/tests")
foreach(cut IN ITEMS before_write after_write lost_ack queue_rejected in_tool before_delivery after_observation
    handoff_before_write handoff_after_write handoff_lost_ack handoff_after_take handoff_after_delivery)
  foreach(decision IN ITEMS approve reject)
    if(cut STREQUAL "in_tool" AND decision STREQUAL "reject")
      continue()
    endif()
    add_test(NAME test_router_review_crash_${cut}_${decision}
      COMMAND "${CMAKE_COMMAND}"
        "-DWORKER=$<TARGET_FILE:router_review_process_fixture>"
        "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/review_cases"
        "-DCUT=${cut}" "-DDECISION=${decision}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_review.cmake")
    set_tests_properties(test_router_review_crash_${cut}_${decision} PROPERTIES
      TIMEOUT 75 LABELS "agent;recovery;process;router-review-intent")
    if(cut MATCHES "^handoff_")
      set_property(TEST test_router_review_crash_${cut}_${decision} APPEND PROPERTY LABELS "router-result-handoff")
    endif()
  endforeach()
endforeach()
