salts_idl_target(
  TARGET turboagent_typed_rpc
  IDL "${CMAKE_CURRENT_SOURCE_DIR}/turboagent_typed_rpc.schema"
  ARTIFACT_NAME "turboagent_typed_rpc"
  TRANSPORTS RPC
  PROJECTION_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/turboagent_typed_rpc.projection.json")

add_executable(test_turboagent_typed_rpc test_turboagent_typed_rpc.c)
target_link_libraries(test_turboagent_typed_rpc PRIVATE
  CHttp::Server CHttp::Client Salts::DataBind Salts::CMeta Salts::TinyTest)
target_include_directories(test_turboagent_typed_rpc PRIVATE
  "${turboagent_typed_rpc_GENERATED_DIR}"
  "${PROJECT_SOURCE_DIR}/agent/tests")
add_dependencies(test_turboagent_typed_rpc
  turboagent_typed_rpc_idl_codegen)
set_target_properties(test_turboagent_typed_rpc PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME test_turboagent_typed_rpc COMMAND test_turboagent_typed_rpc)
set_tests_properties(test_turboagent_typed_rpc PROPERTIES TIMEOUT 30)

# Provider callback is compiled only inside an unloadable shared library;
// test host sees it exclusively through a reflected Plugin export.
add_library(turboagent_rpc_provider_dso SHARED turboagent_rpc_provider_dso.c)
target_link_libraries(turboagent_rpc_provider_dso PRIVATE
  Salts::PluginABI Salts::Platform Salts::DataBind)
set_target_properties(turboagent_rpc_provider_dso PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(test_turboagent_typed_rpc PRIVATE Salts::Plugin)
target_compile_definitions(test_turboagent_typed_rpc PRIVATE
  TURBOAGENT_RPC_PROVIDER_DSO_PATH="$<TARGET_FILE:turboagent_rpc_provider_dso>")
add_dependencies(test_turboagent_typed_rpc turboagent_rpc_provider_dso)
