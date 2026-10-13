# This is a SOURCE-SANITIZER-only CMake configuration hook, not a patch to
# either immutable upstream release checkout or its production CMakeLists.
#
# Both qigao/salts and qigao/salts-utils compile their private vendored
# Monocypher sources into different shared libraries. With ASan instrumentation,
# identically named, externally visible constants like crypto_argon2_no_extras
# cause a real global ODR collision before the RPC test starts. ASan ODR checks
# remain enabled: the correct isolation is to give the PRIVATE vendor archive
# hidden ELF visibility, preventing symbol interposition across DSO owners.
#
# This does NOT hide Salts/SaltsUtils exported public APIs and does NOT disable
# instrumentation, suppress diagnostics, change installed SDK usage, or
# modify any source file from the signed release tags. The audit separately
# demands -fvisibility=hidden on both vendor TUs and all sanitizer flags.

if(PROJECT_NAME STREQUAL "Salts")
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL set_target_properties monocypher PROPERTIES C_VISIBILITY_PRESET hidden)
elseif(PROJECT_NAME STREQUAL "SaltsUtils")
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
    CALL set_target_properties databind_monocypher PROPERTIES C_VISIBILITY_PRESET hidden)
endif()
