include(CMakeDependentOption)

set(CMAKE_COLOR_DIAGNOSTICS ON)

# building the tests
option(ENABLE_TESTS "Enable the tests" ON)
set(BUILD_TESTS_DEFAULT "${ENABLE_TESTS}")

# Address Sanitizer - only enabled for Debug builds
cmake_dependent_option(
    ENABLE_ASAN "Enable Address Sanitizer" ON
    "CMAKE_BUILD_TYPE STREQUAL Debug" OFF
)

# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ${BUILD_TESTS_DEFAULT})
option(ENABLE_LANGCHAIN "Build the langchain module" ON)
option(ENABLE_CODING_TOOLS "Build the bounded coding tool pack" ON)
cmake_dependent_option(
    BUILD_BENCHMARKS "Build benchmark executables" ON
    "BUILD_TESTS" OFF
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
