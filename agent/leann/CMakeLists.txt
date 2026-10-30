# Private C11 algorithm module within the existing TurboAgent build graph.
# No new installer/consumer project, downloaded ML runtime or probing CMake.
add_library(turbo_leann_core STATIC
  src/graph.c
  src/search.c
  src/segment.c)
target_compile_features(turbo_leann_core PUBLIC c_std_11)
target_include_directories(turbo_leann_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(turbo_leann_core PROPERTIES FOLDER "Agent/LEANN")
if(NOT MSVC)
  target_link_libraries(turbo_leann_core PUBLIC m)
endif()

if(BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(test_turbo_leann_core tests/test_turbo_leann_core.c)
  target_link_libraries(test_turbo_leann_core PRIVATE turbo_leann_core)
  add_test(NAME turbo-leann-core COMMAND test_turbo_leann_core)
  set_target_properties(test_turbo_leann_core PROPERTIES FOLDER "Agent/tests")
endif()
