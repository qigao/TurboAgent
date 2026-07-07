set(WASM3_EXAMPLE_WASM_PATH
    "${TURBO_WASM3_VENDOR_DIR}/test/wasi/simple/test.wasm")
set(WASM3_EXAMPLE_PREOPEN_DIR
    "${TURBO_WASM3_VENDOR_DIR}/test/wasi/simple")
set(WASM3_DB_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_db_demo.c")
set(WASM3_DB_GUEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_db_demo.wasm")
set(WASM3_DB_CRUD_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_db_crud_demo.c")
set(WASM3_DB_CRUD_GUEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_db_crud_demo.wasm")
set(WASM3_HTTP_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_http_demo.c")
set(WASM3_HTTP_GUEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_http_demo.wasm")
set(WASM3_PARSER_GUEST_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_parser_demo.c")
set(WASM3_PARSER_GUEST_WASM
    "${CMAKE_CURRENT_BINARY_DIR}/guest_parser_demo.wasm")
set(WASM3_TURBONET_GUEST_ABI_HEADER
    "${CMAKE_CURRENT_SOURCE_DIR}/../include/turbo_wasm3_guest_abi.h")

add_executable(turbo_wasm3_example ${CMAKE_CURRENT_SOURCE_DIR}/turbo_wasm3_example.c)

set_target_properties(turbo_wasm3_example PROPERTIES FOLDER "wasm3/examples")

target_link_libraries(turbo_wasm3_example PRIVATE turbo_wasm3)

target_compile_definitions(
  turbo_wasm3_example PRIVATE
  WASM3_EXAMPLE_WASM_PATH=\"${WASM3_EXAMPLE_WASM_PATH}\"
  WASM3_EXAMPLE_PREOPEN_DIR=\"${WASM3_EXAMPLE_PREOPEN_DIR}\")

find_program(WASM3_EXAMPLE_CLANG NAMES clang clang.exe)

if(WASM3_EXAMPLE_CLANG)
  add_custom_command(
    OUTPUT ${WASM3_DB_GUEST_WASM}
    COMMAND
      ${WASM3_EXAMPLE_CLANG}
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
      -o ${WASM3_DB_GUEST_WASM}
      ${WASM3_DB_GUEST_SOURCE}
    DEPENDS ${WASM3_DB_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
    VERBATIM)

  add_custom_target(turbo_wasm3_guest_db_demo ALL
                    DEPENDS ${WASM3_DB_GUEST_WASM})
  set_target_properties(turbo_wasm3_guest_db_demo
                        PROPERTIES FOLDER "wasm3/examples")

  add_custom_command(
    OUTPUT ${WASM3_DB_CRUD_GUEST_WASM}
    COMMAND
      ${WASM3_EXAMPLE_CLANG}
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
      -o ${WASM3_DB_CRUD_GUEST_WASM}
      ${WASM3_DB_CRUD_GUEST_SOURCE}
    DEPENDS ${WASM3_DB_CRUD_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
    VERBATIM)

  add_custom_target(turbo_wasm3_guest_db_crud_demo ALL
                    DEPENDS ${WASM3_DB_CRUD_GUEST_WASM})
  set_target_properties(turbo_wasm3_guest_db_crud_demo
                        PROPERTIES FOLDER "wasm3/examples")

  add_custom_command(
    OUTPUT ${WASM3_HTTP_GUEST_WASM}
    COMMAND
      ${WASM3_EXAMPLE_CLANG}
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
      -o ${WASM3_HTTP_GUEST_WASM}
      ${WASM3_HTTP_GUEST_SOURCE}
    DEPENDS ${WASM3_HTTP_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
    VERBATIM)

  add_custom_target(turbo_wasm3_guest_http_demo ALL
                    DEPENDS ${WASM3_HTTP_GUEST_WASM})
  set_target_properties(turbo_wasm3_guest_http_demo
                        PROPERTIES FOLDER "wasm3/examples")

  add_custom_command(
    OUTPUT ${WASM3_PARSER_GUEST_WASM}
    COMMAND
      ${WASM3_EXAMPLE_CLANG}
      --target=wasm32-unknown-unknown
      -I${CMAKE_CURRENT_SOURCE_DIR}/../include
      -O2
      -nostdlib
      -Wl,--no-entry
      -Wl,--export=run_json_demo
      -Wl,--export=run_csv_demo
      -Wl,--export=run_xml_demo
      -Wl,--export-memory
      -Wl,--initial-memory=131072
      -Wl,--max-memory=131072
      -Wl,--allow-undefined
      -o ${WASM3_PARSER_GUEST_WASM}
      ${WASM3_PARSER_GUEST_SOURCE}
    DEPENDS ${WASM3_PARSER_GUEST_SOURCE} ${WASM3_TURBONET_GUEST_ABI_HEADER}
    VERBATIM)

  add_custom_target(turbo_wasm3_guest_parser_demo ALL
                    DEPENDS ${WASM3_PARSER_GUEST_WASM})
  set_target_properties(turbo_wasm3_guest_parser_demo
                        PROPERTIES FOLDER "wasm3/examples")

  add_executable(turbo_wasm3_db_example
                 ${CMAKE_CURRENT_SOURCE_DIR}/turbo_wasm3_db_example.c)
  set_target_properties(turbo_wasm3_db_example PROPERTIES FOLDER "wasm3/examples")
  add_dependencies(turbo_wasm3_db_example turbo_wasm3_guest_db_demo)
  target_link_libraries(turbo_wasm3_db_example PRIVATE turbo_wasm3)
  target_compile_definitions(
    turbo_wasm3_db_example PRIVATE
    WASM3_DB_EXAMPLE_WASM_PATH=\"${WASM3_DB_GUEST_WASM}\")

  add_executable(turbo_wasm3_db_crud_example
                 ${CMAKE_CURRENT_SOURCE_DIR}/turbo_wasm3_db_crud_example.c)
  set_target_properties(turbo_wasm3_db_crud_example PROPERTIES FOLDER "wasm3/examples")
  add_dependencies(turbo_wasm3_db_crud_example turbo_wasm3_guest_db_crud_demo)
  target_link_libraries(turbo_wasm3_db_crud_example PRIVATE turbo_wasm3)
  target_compile_definitions(
    turbo_wasm3_db_crud_example PRIVATE
    WASM3_DB_CRUD_EXAMPLE_WASM_PATH=\"${WASM3_DB_CRUD_GUEST_WASM}\")

  add_executable(turbo_wasm3_http_example
                 ${CMAKE_CURRENT_SOURCE_DIR}/turbo_wasm3_http_example.c)
  set_target_properties(turbo_wasm3_http_example PROPERTIES FOLDER "wasm3/examples")
  add_dependencies(turbo_wasm3_http_example turbo_wasm3_guest_http_demo)
  target_link_libraries(turbo_wasm3_http_example PRIVATE turbo_wasm3)
  target_compile_definitions(
    turbo_wasm3_http_example PRIVATE
    WASM3_HTTP_EXAMPLE_WASM_PATH=\"${WASM3_HTTP_GUEST_WASM}\")

  add_executable(turbo_wasm3_parser_example
                 ${CMAKE_CURRENT_SOURCE_DIR}/turbo_wasm3_parser_example.c)
  set_target_properties(turbo_wasm3_parser_example PROPERTIES FOLDER "wasm3/examples")
  add_dependencies(turbo_wasm3_parser_example turbo_wasm3_guest_parser_demo)
  target_link_libraries(turbo_wasm3_parser_example PRIVATE turbo_wasm3)
  target_compile_definitions(
    turbo_wasm3_parser_example PRIVATE
    WASM3_PARSER_EXAMPLE_WASM_PATH=\"${WASM3_PARSER_GUEST_WASM}\")
endif()
