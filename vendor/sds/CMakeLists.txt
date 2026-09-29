project(sds)

set(SDS_SOURCES
    sds.c
    sds.h
    sdsalloc.h
)

add_library(sds STATIC ${SDS_SOURCES})
add_library(TurboAgent::SDS ALIAS sds)

target_include_directories(sds PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(sds PRIVATE SDS_ALLOC_POOL=1)

set_target_properties(sds PROPERTIES FOLDER "vendor/sds" EXPORT_NAME SDS)

# Testing 
add_subdirectory(test)
 
install(TARGETS sds EXPORT TurboAgentTargets)
install(FILES sds.h sdsalloc.h DESTINATION include)
