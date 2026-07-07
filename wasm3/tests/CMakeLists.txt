set(TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_turbo_wasm3.c)
set(WASM3_TEST_WASM_PATH
    "${TURBO_WASM3_VENDOR_DIR}/test/wasi/simple/test.wasm")
set(WASM3_DB_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/guest_db_demo.c")
set(WASM3_DB_TEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_db_demo_test.wasm")
set(WASM3_DB_CRUD_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/guest_db_crud_demo.c")
set(WASM3_DB_CRUD_TEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_db_crud_demo_test.wasm")
set(WASM3_HTTP_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/guest_http_demo.c")
set(WASM3_HTTP_TEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_http_demo_test.wasm")
set(WASM3_REDIS_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/guest_redis_demo.c")
set(WASM3_REDIS_TEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_redis_demo_test.wasm")
set(WASM3_PARSER_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../examples/guest_parser_demo.c")
set(WASM3_PARSER_TEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_parser_demo_test.wasm")
set(WASM3_TURBONET_GUEST_ABI_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/turbo_wasm3_guest_abi.h")

find_program(WASM3_TEST_CLANG NAMES clang clang.exe)
if(NOT WASM3_TEST_CLANG)
  message(FATAL_ERROR
    "WASM3_BUILD_TESTS requires clang to build guest ABI wasm fixtures. "
    "Install clang or configure with -DWASM3_BUILD_TESTS=OFF.")
endif()

add_custom_command(
  OUTPUT ${WASM3_DB_TEST_WASM}
  COMMAND
    ${WASM3_TEST_CLANG}
    --target=wasm32-unknown-unknown
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include
    -O2
    -nostdlib
    -Wl,--no-entry
    -Wl,--export=run_db_demo
    -Wl,--export-memory
    -Wl,--initial-memory=131072
    -Wl,--max-memory=131072
    -Wl,--allow-undefined
    -o ${WASM3_DB_TEST_WASM}
    ${WASM3_DB_GUEST_SOURCE}
  DEPENDS ${WASM3_DB_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
  VERBATIM)

add_custom_command(
  OUTPUT ${WASM3_DB_CRUD_TEST_WASM}
  COMMAND
    ${WASM3_TEST_CLANG}
    --target=wasm32-unknown-unknown
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include
    -O2
    -nostdlib
    -Wl,--no-entry
    -Wl,--export=run_db_crud_demo
    -Wl,--export-memory
    -Wl,--initial-memory=131072
    -Wl,--max-memory=131072
    -Wl,--allow-undefined
    -o ${WASM3_DB_CRUD_TEST_WASM}
    ${WASM3_DB_CRUD_GUEST_SOURCE}
  DEPENDS ${WASM3_DB_CRUD_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
  VERBATIM)

add_custom_command(
  OUTPUT ${WASM3_HTTP_TEST_WASM}
  COMMAND
    ${WASM3_TEST_CLANG}
    --target=wasm32-unknown-unknown
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include
    -O2
    -nostdlib
    -Wl,--no-entry
    -Wl,--export=run_http_demo
    -Wl,--export-memory
    -Wl,--initial-memory=131072
    -Wl,--max-memory=131072
    -Wl,--allow-undefined
    -o ${WASM3_HTTP_TEST_WASM}
    ${WASM3_HTTP_GUEST_SOURCE}
  DEPENDS ${WASM3_HTTP_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
  VERBATIM)

add_custom_command(
  OUTPUT ${WASM3_REDIS_TEST_WASM}
  COMMAND
    ${WASM3_TEST_CLANG}
    --target=wasm32-unknown-unknown
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include
    -O2
    -nostdlib
    -Wl,--no-entry
    -Wl,--export=run_redis_demo
    -Wl,--export-memory
    -Wl,--initial-memory=131072
    -Wl,--max-memory=131072
    -Wl,--allow-undefined
    -o ${WASM3_REDIS_TEST_WASM}
    ${WASM3_REDIS_GUEST_SOURCE}
  DEPENDS ${WASM3_REDIS_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
  VERBATIM)

add_custom_command(
  OUTPUT ${WASM3_PARSER_TEST_WASM}
  COMMAND
    ${WASM3_TEST_CLANG}
    --target=wasm32-unknown-unknown
    -I${CMAKE_CURRENT_SOURCE_DIR}/../include
    -O2
    -nostdlib
    -Wl,--no-entry
    -Wl,--export=run_json_demo
    -Wl,--export=run_csv_demo
    -Wl,--export=run_xml_demo
    -Wl,--export=run_ini_demo
    -Wl,--export-memory
    -Wl,--initial-memory=131072
    -Wl,--max-memory=131072
    -Wl,--allow-undefined
    -o ${WASM3_PARSER_TEST_WASM}
    ${WASM3_PARSER_GUEST_SOURCE}
  DEPENDS ${WASM3_PARSER_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
  VERBATIM)

cmake_add_test(
  SOURCES ${TEST_SOURCES}
  FOLDER "wasm3/tests"
  LIBS TurboNet::TinyTest turbo_wasm3)

if(TARGET test_turbo_wasm3)
  add_custom_target(turbo_wasm3_guest_db_test DEPENDS ${WASM3_DB_TEST_WASM})
  set_target_properties(turbo_wasm3_guest_db_test
                        PROPERTIES FOLDER "wasm3/tests")
  add_dependencies(test_turbo_wasm3 turbo_wasm3_guest_db_test)

  add_custom_target(turbo_wasm3_guest_db_crud_test
                    DEPENDS ${WASM3_DB_CRUD_TEST_WASM})
  set_target_properties(turbo_wasm3_guest_db_crud_test
                        PROPERTIES FOLDER "wasm3/tests")
  add_dependencies(test_turbo_wasm3 turbo_wasm3_guest_db_crud_test)

  add_custom_target(turbo_wasm3_guest_http_test
                    DEPENDS ${WASM3_HTTP_TEST_WASM})
  set_target_properties(turbo_wasm3_guest_http_test
                        PROPERTIES FOLDER "wasm3/tests")
  add_dependencies(test_turbo_wasm3 turbo_wasm3_guest_http_test)

  add_custom_target(turbo_wasm3_guest_redis_test
                    DEPENDS ${WASM3_REDIS_TEST_WASM})
  set_target_properties(turbo_wasm3_guest_redis_test
                        PROPERTIES FOLDER "wasm3/tests")
  add_dependencies(test_turbo_wasm3 turbo_wasm3_guest_redis_test)

  add_custom_target(turbo_wasm3_guest_parser_test
                    DEPENDS ${WASM3_PARSER_TEST_WASM})
  set_target_properties(turbo_wasm3_guest_parser_test
                        PROPERTIES FOLDER "wasm3/tests")
  add_dependencies(test_turbo_wasm3 turbo_wasm3_guest_parser_test)
  if(WIN32)
    target_link_libraries(test_turbo_wasm3 PRIVATE ws2_32)
  endif()
  target_compile_definitions(
    test_turbo_wasm3 PRIVATE
    WASM3_TEST_WASM_PATH=\"${WASM3_TEST_WASM_PATH}\")
  target_compile_definitions(
    test_turbo_wasm3 PRIVATE
    WASM3_DB_TEST_WASM_PATH=\"${WASM3_DB_TEST_WASM}\"
    WASM3_DB_CRUD_TEST_WASM_PATH=\"${WASM3_DB_CRUD_TEST_WASM}\"
    WASM3_HTTP_TEST_WASM_PATH=\"${WASM3_HTTP_TEST_WASM}\"
    WASM3_REDIS_TEST_WASM_PATH=\"${WASM3_REDIS_TEST_WASM}\"
    WASM3_PARSER_TEST_WASM_PATH=\"${WASM3_PARSER_TEST_WASM}\")
endif()
