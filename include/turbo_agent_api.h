#ifndef TURBO_AGENT_API_H
#define TURBO_AGENT_API_H

#include <salts/error_codes.h>

/*
 * TurboAgent owns its linkage contract. Do not reuse SALTS_API: on Windows
 * Salts consumers receive SALTS_API=dllimport, which must never leak onto
 * TurboAgent definitions.
 *
 * Windows shared libraries are exported by CMake with WINDOWS_EXPORT_ALL_SYMBOLS.
 * Consumers therefore do not need a shared producer/consumer preprocessor state.
 */
#ifndef TURBO_AGENT_API
  #if !defined(_WIN32) && defined(__GNUC__) && __GNUC__ >= 4
    #define TURBO_AGENT_API __attribute__((visibility("default")))
  #else
    #define TURBO_AGENT_API
  #endif
#endif

#ifndef TURBO_AGENT_C_API
  #ifdef __cplusplus
    #define TURBO_AGENT_C_API extern "C" TURBO_AGENT_API
  #else
    #define TURBO_AGENT_C_API TURBO_AGENT_API
  #endif
#endif

#ifndef CXX_C_API
  #ifdef __cplusplus
    #define CXX_C_API extern "C" TURBO_AGENT_API
  #else
    #define CXX_C_API TURBO_AGENT_API
  #endif
#endif

#endif /* TURBO_AGENT_API_H */
