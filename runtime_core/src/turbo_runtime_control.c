#include "turbo_runtime_control.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

#include <salts/clock.h>
#include <salts/thread.h>

#define TURBO_CANCEL_MAX_WAIT_SLICE_MS (UINT32_MAX - UINT64_C(1))
#define TURBO_MILLISECONDS_TO_NANOSECONDS UINT64_C(1000000)

typedef struct turbo_cancel_state_s {
  atomic_size_t ref_count;
  salts_mutex_t mutex;
  salts_cond_t cond;
  turbo_cancel_reason_t reason;
  uint64_t deadline_mono_ms;
  turbo_runtime_monotonic_ms_fn monotonic_ms;
  void *clock_user_data;
} turbo_cancel_state_t;

struct turbo_cancel_source_s {
  turbo_cancel_state_t *state;
};

struct turbo_cancel_token_s {
  atomic_size_t ref_count;
  turbo_cancel_state_t *state;
};

static uint64_t turbo_cancel_state_now_ms(const turbo_cancel_state_t *state) {
  if (state && state->monotonic_ms) {
    return state->monotonic_ms(state->clock_user_data);
  }
  return salts_monotonic_ms();
}

static int turbo_cancel_config_valid(const turbo_cancel_source_config_t *config) {
  if (!config) {
    return 1;
  }
  return config->struct_size >= sizeof(*config) &&
         config->abi_version == TURBO_RUNTIME_CONTROL_ABI_VERSION;
}

static int turbo_cancel_state_retain(turbo_cancel_state_t *state) {
  size_t current;

  if (!state) {
    return 0;
  }

  current = atomic_load_explicit(&state->ref_count, memory_order_relaxed);
  for (;;) {
    if (current == SIZE_MAX) {
      return 0;
    }
    if (atomic_compare_exchange_weak_explicit(&state->ref_count, &current, current + 1,
                                              memory_order_relaxed, memory_order_relaxed)) {
      return 1;
    }
  }
}

static void turbo_cancel_state_release(turbo_cancel_state_t *state) {
  if (!state) {
    return;
  }
  if (atomic_fetch_sub_explicit(&state->ref_count, 1, memory_order_acq_rel) != 1) {
    return;
  }

  atomic_thread_fence(memory_order_acquire);
  salts_cond_destroy(&state->cond);
  salts_mutex_destroy(&state->mutex);
  free(state);
}

static int turbo_cancel_reason_valid(turbo_cancel_reason_t reason) {
  return reason >= TURBO_CANCEL_USER && reason <= TURBO_CANCEL_PARENT;
}

static turbo_cancel_reason_t turbo_cancel_state_refresh_deadline_locked(turbo_cancel_state_t *state,
                                                                        uint64_t now_ms) {
  if (state->reason == TURBO_CANCEL_NONE && state->deadline_mono_ms != 0 &&
      now_ms >= state->deadline_mono_ms) {
    state->reason = TURBO_CANCEL_DEADLINE;
    salts_cond_broadcast(&state->cond);
  }
  return state->reason;
}

static int turbo_cancel_reason_status(turbo_cancel_reason_t reason) {
  if (reason == TURBO_CANCEL_NONE) {
    return SALTS_OK;
  }
  if (reason == TURBO_CANCEL_DEADLINE) {
    return SALTS_ETIMEDOUT;
  }
  return SALTS_ECANCELED;
}

static uint64_t turbo_cancel_saturating_add_ms(uint64_t start, uint64_t duration) {
  if (UINT64_MAX - start < duration) {
    return UINT64_MAX;
  }
  return start + duration;
}

int turbo_cancel_source_create(const turbo_cancel_source_config_t *config,
                               turbo_cancel_source_t **out_source) {
  turbo_cancel_source_t *source = NULL;
  turbo_cancel_state_t *state = NULL;

  if (!out_source) {
    return SALTS_EINVAL;
  }
  *out_source = NULL;
  if (!turbo_cancel_config_valid(config)) {
    return SALTS_EINVAL;
  }

  source = (turbo_cancel_source_t *)calloc(1, sizeof(*source));
  state = (turbo_cancel_state_t *)calloc(1, sizeof(*state));
  if (!source || !state) {
    free(source);
    free(state);
    return SALTS_ENOMEM;
  }

  salts_mutex_init(&state->mutex);
  salts_cond_init(&state->cond);
  if (!state->mutex || !state->cond) {
    salts_cond_destroy(&state->cond);
    salts_mutex_destroy(&state->mutex);
    free(state);
    free(source);
    return SALTS_ENOMEM;
  }

  atomic_init(&state->ref_count, 1);
  if (config) {
    state->deadline_mono_ms = config->deadline_mono_ms;
    state->monotonic_ms = config->monotonic_ms;
    state->clock_user_data = config->clock_user_data;
  }
  source->state = state;
  *out_source = source;
  return SALTS_OK;
}

void turbo_cancel_source_destroy(turbo_cancel_source_t *source) {
  if (!source) {
    return;
  }
  turbo_cancel_state_release(source->state);
  source->state = NULL;
  free(source);
}

int turbo_cancel_source_token(const turbo_cancel_source_t *source,
                              turbo_cancel_token_t **out_token) {
  turbo_cancel_token_t *token;

  if (!out_token) {
    return SALTS_EINVAL;
  }
  *out_token = NULL;
  if (!source || !source->state) {
    return SALTS_EINVAL;
  }
  if (!turbo_cancel_state_retain(source->state)) {
    return SALTS_ERANGE;
  }

  token = (turbo_cancel_token_t *)calloc(1, sizeof(*token));
  if (!token) {
    turbo_cancel_state_release(source->state);
    return SALTS_ENOMEM;
  }
  atomic_init(&token->ref_count, 1);
  token->state = source->state;
  *out_token = token;
  return SALTS_OK;
}

int turbo_cancel_source_cancel(turbo_cancel_source_t *source, turbo_cancel_reason_t reason) {
  int status = SALTS_OK;

  if (!source || !source->state || !turbo_cancel_reason_valid(reason)) {
    return SALTS_EINVAL;
  }

  salts_mutex_lock(&source->state->mutex);
  turbo_cancel_state_refresh_deadline_locked(source->state,
                                             turbo_cancel_state_now_ms(source->state));
  if (source->state->reason != TURBO_CANCEL_NONE) {
    status = SALTS_EALREADY;
  } else {
    source->state->reason = reason;
    salts_cond_broadcast(&source->state->cond);
  }
  salts_mutex_unlock(&source->state->mutex);
  return status;
}

turbo_cancel_reason_t turbo_cancel_source_reason(const turbo_cancel_source_t *source) {
  turbo_cancel_reason_t reason;

  if (!source || !source->state) {
    return TURBO_CANCEL_NONE;
  }
  salts_mutex_lock(&source->state->mutex);
  reason = turbo_cancel_state_refresh_deadline_locked(source->state,
                                                      turbo_cancel_state_now_ms(source->state));
  salts_mutex_unlock(&source->state->mutex);
  return reason;
}

turbo_cancel_token_t *turbo_cancel_token_retain(turbo_cancel_token_t *token) {
  size_t current;

  if (!token || !token->state) {
    return NULL;
  }
  current = atomic_load_explicit(&token->ref_count, memory_order_relaxed);
  for (;;) {
    if (current == 0 || current == SIZE_MAX) {
      return NULL;
    }
    if (atomic_compare_exchange_weak_explicit(&token->ref_count, &current, current + 1,
                                              memory_order_relaxed, memory_order_relaxed)) {
      return token;
    }
  }
}

void turbo_cancel_token_release(turbo_cancel_token_t *token) {
  if (!token) {
    return;
  }
  if (atomic_fetch_sub_explicit(&token->ref_count, 1, memory_order_acq_rel) != 1) {
    return;
  }
  turbo_cancel_state_release(token->state);
  token->state = NULL;
  free(token);
}

int turbo_cancel_token_check(const turbo_cancel_token_t *token) {
  turbo_cancel_reason_t reason;

  if (!token || !token->state) {
    return SALTS_EINVAL;
  }
  salts_mutex_lock(&token->state->mutex);
  reason = turbo_cancel_state_refresh_deadline_locked(token->state,
                                                      turbo_cancel_state_now_ms(token->state));
  salts_mutex_unlock(&token->state->mutex);
  return turbo_cancel_reason_status(reason);
}

turbo_cancel_reason_t turbo_cancel_token_reason(const turbo_cancel_token_t *token) {
  turbo_cancel_reason_t reason;

  if (!token || !token->state) {
    return TURBO_CANCEL_NONE;
  }
  salts_mutex_lock(&token->state->mutex);
  reason = turbo_cancel_state_refresh_deadline_locked(token->state,
                                                      turbo_cancel_state_now_ms(token->state));
  salts_mutex_unlock(&token->state->mutex);
  return reason;
}

uint64_t turbo_cancel_token_deadline_mono_ms(const turbo_cancel_token_t *token) {
  if (!token || !token->state) {
    return 0;
  }
  return token->state->deadline_mono_ms;
}

turbo_cancel_wait_status_t turbo_cancel_token_wait(const turbo_cancel_token_t *token,
                                                   uint64_t timeout_ms) {
  turbo_cancel_state_t *state;
  uint64_t now_ms;
  uint64_t caller_deadline_ms = UINT64_MAX;
  int has_caller_deadline = timeout_ms != TURBO_RUNTIME_WAIT_INFINITE;
  turbo_cancel_wait_status_t status = TURBO_CANCEL_WAIT_INVALID_ARGUMENT;

  if (!token || !token->state) {
    return TURBO_CANCEL_WAIT_INVALID_ARGUMENT;
  }
  state = token->state;
  now_ms = turbo_cancel_state_now_ms(state);
  if (has_caller_deadline) {
    caller_deadline_ms = turbo_cancel_saturating_add_ms(now_ms, timeout_ms);
  }

  salts_mutex_lock(&state->mutex);
  for (;;) {
    uint64_t wake_deadline_ms = UINT64_MAX;
    uint64_t wait_ms;

    now_ms = turbo_cancel_state_now_ms(state);
    if (turbo_cancel_state_refresh_deadline_locked(state, now_ms) != TURBO_CANCEL_NONE) {
      status = TURBO_CANCEL_WAIT_SIGNALED;
      break;
    }
    if (has_caller_deadline && now_ms >= caller_deadline_ms) {
      status = TURBO_CANCEL_WAIT_TIMEOUT;
      break;
    }

    if (has_caller_deadline) {
      wake_deadline_ms = caller_deadline_ms;
    }
    if (state->deadline_mono_ms != 0 && state->deadline_mono_ms < wake_deadline_ms) {
      wake_deadline_ms = state->deadline_mono_ms;
    }

    if (wake_deadline_ms == UINT64_MAX) {
      salts_cond_wait(&state->cond, &state->mutex);
      continue;
    }

    wait_ms = wake_deadline_ms - now_ms;
    if (wait_ms > TURBO_CANCEL_MAX_WAIT_SLICE_MS) {
      wait_ms = TURBO_CANCEL_MAX_WAIT_SLICE_MS;
    }
    (void)salts_cond_timedwait(&state->cond, &state->mutex,
                               wait_ms * TURBO_MILLISECONDS_TO_NANOSECONDS);
  }
  salts_mutex_unlock(&state->mutex);
  return status;
}
