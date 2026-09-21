add_executable(sds_test sds_test.c)

target_link_libraries(sds_test PRIVATE
    sds 
    Salts::TinyTest
)

add_test(NAME sds_test COMMAND sds_test)

set_target_properties(sds_test PROPERTIES FOLDER "vendor/sds/test")
