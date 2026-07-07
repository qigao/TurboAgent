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

# SSL support
option(ENABLE_SSL "Enable SSL support" ON)
cmake_dependent_option(
    USE_OPENSSL "Use OpenSSL" ON "ENABLE_SSL;NOT USE_MBEDTLS" OFF
)
cmake_dependent_option(
    USE_MBEDTLS "Use MbedTLS" OFF "ENABLE_SSL;NOT USE_OPENSSL" OFF
)

if(ENABLE_SSL)
    if(USE_OPENSSL)
        set(SSL_BACKEND_USED "OpenSSL")
    elseif(USE_MBEDTLS)
        set(SSL_BACKEND_USED "MbedTLS")
    else()
        message(
            FATAL_ERROR
                "No valid SSL backend selected. Please enable either USE_OPENSSL or USE_MBEDTLS."
        )
    endif()
endif()
message(STATUS "SSL backend used: ${SSL_BACKEND_USED}")
# if(MSVC)
#     add_compile_options(/bigobj)
# endif()

option(BUILD_EXAMPLES "Build example programs" ON)
option(BUILD_TESTS "Build test suite" ${BUILD_TESTS_DEFAULT})
option(ENABLE_LANGCHAIN "Build the langchain module" ON)
cmake_dependent_option(
    BUILD_BENCHMARKS "Build benchmark executables" ON
    "BUILD_TESTS" OFF
)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
