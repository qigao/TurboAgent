cmake_minimum_required(VERSION 3.10)
project(cjwt C)

set(CJWT_VERSION_MAJOR 2)
set(CJWT_VERSION_MINOR 3)
set(CJWT_VERSION_PATCH 0)
set(CJWT_VERSION
    "${CJWT_VERSION_MAJOR}.${CJWT_VERSION_MINOR}.${CJWT_VERSION_PATCH}")

# Bitwise version: 0xAABBCC (AA=Major, BB=Minor, CC=Patch)
math(
  EXPR
  CJWT_BITWISE_VERSION
  "(${CJWT_VERSION_MAJOR} << 16) | (${CJWT_VERSION_MINOR} << 8) | ${CJWT_VERSION_PATCH}"
  OUTPUT_FORMAT HEXADECIMAL)

# Configure version header
configure_file(include/cjwt/cjwtver.h.in include/cjwt/cjwtver.h @ONLY)

# Dependencies
find_package(OpenSSL REQUIRED)
find_package(base64 REQUIRED)

set(CJWT_SOURCES src/cjwt.c src/print.c src/utils.c)

if(OPENSSL_VERSION VERSION_LESS "3.0.0")
  list(APPEND CJWT_SOURCES src/jws_openssl_1.c)
else()
  list(APPEND CJWT_SOURCES src/jws_openssl_3.c src/jwe_openssl_3.c)
endif()

add_library(cjwt STATIC ${CJWT_SOURCES})

target_include_directories(
  cjwt
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/cjwt>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
         $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include/cjwt>)

target_link_libraries(cjwt PUBLIC OpenSSL::SSL OpenSSL::Crypto Salts::JsonParser
                                  aklomp::base64)

set_target_properties(cjwt PROPERTIES FOLDER vendor/cjwt)

add_subdirectory(tests)
add_subdirectory(examples)
