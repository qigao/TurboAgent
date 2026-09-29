#ifndef TURBO_AGENT_API_H
#define TURBO_AGENT_API_H

#include <salts_api.h>
#include <salts/error_codes.h>

/*
 * TurboAgent owns its public API surface.  Salts supplies the platform-neutral
 * visibility primitive and canonical error codes; TurboAgent does not depend
 * on the retired TurboUtils export/error headers.
 */
#ifndef TURBO_AGENT_API
#define TURBO_AGENT_API SALTS_API
#endif

#ifndef TURBO_AGENT_C_API
#ifdef __cplusplus
#define TURBO_AGENT_C_API extern "C" TURBO_AGENT_API
#else
#define TURBO_AGENT_C_API TURBO_AGENT_API
#endif
#endif

/*
 * Existing TurboAgent headers still spell the declaration marker CXX_C_API.
 * Keep that source-level spelling local to TurboAgent while its ABI ownership
 * is migrated away from the retired dependency.
 */
#ifndef CXX_C_API
#define CXX_C_API TURBO_AGENT_API
#endif

#endif /* TURBO_AGENT_API_H */
