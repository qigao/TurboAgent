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
