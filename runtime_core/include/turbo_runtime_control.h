#ifndef TURBO_RUNTIME_CONTROL_H
#define TURBO_RUNTIME_CONTROL_H

#include <turbo_agent_api.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TURBO_RUNTIME_CONTROL_ABI_VERSION 1U
#define TURBO_RUNTIME_WAIT_INFINITE UINT64_MAX

typedef struct turbo_cancel_source_s turbo_cancel_source_t;
typedef struct turbo_cancel_token_s turbo_cancel_token_t;

typedef enum turbo_cancel_reason_e {
  TURBO_CANCEL_NONE = 0,
  TURBO_CANCEL_USER = 1,
  TURBO_CANCEL_DEADLINE = 2,
  TURBO_CANCEL_SHUTDOWN = 3,
  TURBO_CANCEL_PARENT = 4
} turbo_cancel_reason_t;

typedef enum turbo_cancel_wait_status_e {
  TURBO_CANCEL_WAIT_SIGNALED = 0,
  TURBO_CANCEL_WAIT_TIMEOUT = 1,
  TURBO_CANCEL_WAIT_INVALID_ARGUMENT = -1
} turbo_cancel_wait_status_t;

/**
 * @brief Monotonic clock callback used by a cancel source.
 *
 * The callback must be non-blocking and must not call back into the same cancel
 * source. Its user data is borrowed and must outlive the source and every token
 * created from it.
 */
typedef uint64_t (*turbo_runtime_monotonic_ms_fn)(void *user_data);

typedef struct turbo_cancel_source_config_s {
  uint32_t struct_size;
  uint32_t abi_version;
  /** Absolute monotonic deadline in milliseconds. Zero disables the deadline. */
  uint64_t deadline_mono_ms;
  /** Optional test/platform clock. NULL selects turbo_monotonic_ms(). */
  turbo_runtime_monotonic_ms_fn monotonic_ms;
  /** Borrowed clock context; see turbo_runtime_monotonic_ms_fn. */
  void *clock_user_data;
} turbo_cancel_source_config_t;

/**
 * @brief Create the single writable owner for one cancellation state.
 * @param config Optional versioned configuration. NULL creates no deadline.
 * @param out_source Receives the owned source on success.
 * @return TURBO_OK, TURBO_EINVAL, or TURBO_ENOMEM.
 */
CXX_C_API int turbo_cancel_source_create(const turbo_cancel_source_config_t *config,
                                         turbo_cancel_source_t **out_source);

/**
 * @brief Release the source handle.
 *
 * Destroying a source does not implicitly cancel it. Existing tokens remain
 * valid until their final release. The caller must cancel first when waiters
 * need to be woken during shutdown.
 */
CXX_C_API void turbo_cancel_source_destroy(turbo_cancel_source_t *source);

/**
 * @brief Create one owned read-only token from a source.
 * @return TURBO_OK, TURBO_EINVAL, TURBO_ENOMEM, or TURBO_ERANGE.
 */
CXX_C_API int turbo_cancel_source_token(const turbo_cancel_source_t *source,
                                        turbo_cancel_token_t **out_token);

/**
 * @brief Publish the first cancellation reason and wake every waiter.
 * @return TURBO_OK, TURBO_EINVAL, or TURBO_EALREADY when a reason already won.
 */
CXX_C_API int turbo_cancel_source_cancel(turbo_cancel_source_t *source,
                                         turbo_cancel_reason_t reason);

/** @brief Return the currently published reason, or NONE while active. */
CXX_C_API turbo_cancel_reason_t turbo_cancel_source_reason(const turbo_cancel_source_t *source);

/** @brief Retain an owned token reference. Returns NULL on invalid input/overflow. */
CXX_C_API turbo_cancel_token_t *turbo_cancel_token_retain(turbo_cancel_token_t *token);

/** @brief Release one owned token reference. */
CXX_C_API void turbo_cancel_token_release(turbo_cancel_token_t *token);

/**
 * @brief Check cancellation and materialize an expired deadline.
 * @return TURBO_OK while active, TURBO_ECANCELED for explicit cancellation,
 *         TURBO_ETIMEDOUT for deadline expiry, or TURBO_EINVAL.
 */
CXX_C_API int turbo_cancel_token_check(const turbo_cancel_token_t *token);

/** @brief Return the current reason, materializing an expired deadline first. */
CXX_C_API turbo_cancel_reason_t turbo_cancel_token_reason(const turbo_cancel_token_t *token);

/** @brief Return the configured absolute monotonic deadline, or zero. */
CXX_C_API uint64_t turbo_cancel_token_deadline_mono_ms(const turbo_cancel_token_t *token);

/**
 * @brief Wait until cancellation/deadline or until the caller's wait expires.
 *
 * timeout_ms is a relative wait duration. TURBO_RUNTIME_WAIT_INFINITE waits
 * without a caller timeout, while still honoring the token deadline.
 * TURBO_CANCEL_WAIT_SIGNALED means callers should use
 * turbo_cancel_token_check() or turbo_cancel_token_reason() for the cause.
 */
CXX_C_API turbo_cancel_wait_status_t turbo_cancel_token_wait(const turbo_cancel_token_t *token,
                                                             uint64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
