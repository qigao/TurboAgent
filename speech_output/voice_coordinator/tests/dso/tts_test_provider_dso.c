#include "tts_test_dso.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* C11 test fixture: a real dlopen-loaded DSO with one real Linux pthread inference worker.
 * No synthetic TTS model, no mic/speaker, no new host PCM queue or device.
 * The sole worker owns ONE 480-sample generation-scoped PCM block.
 * The host invokes all Provider callbacks on its serial control owner. */
struct turbo_tts_test_dso {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t worker;
  uint64_t generation;
  int begun;
  int thread_created;
  int worker_started;
  int worker_done;
  int allow_pcm;
  int frame_ready;
  int cancel_requested;
  int finish_requested;
  int allow_settle;
  int late_pcm;
  int16_t frame[TURBO_TTS_SAMPLES_PER_FRAME];
  turbo_tts_test_dso_counts_t counts;
};

/* Safe because host joins worker, destroys context, then dlclose on its owner. */
static atomic_uint *unload_observer = NULL;

__attribute__((destructor))
static void test_provider_dso_unload(void) {
  if (unload_observer != NULL)
    (void)atomic_fetch_add_explicit(unload_observer, 1u,
                                     memory_order_release);
}

static void set_unload_observer(atomic_uint *observer) {
  unload_observer = observer;
}

static void *worker_main(void *param) {
  turbo_tts_test_dso_t *ctx = (turbo_tts_test_dso_t *)param;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return NULL;
  ctx->worker_started = 1;
  ctx->counts.worker_started = 1u;
  pthread_cond_broadcast(&ctx->changed);

  while (!ctx->allow_pcm && !ctx->cancel_requested)
    (void)pthread_cond_wait(&ctx->changed, &ctx->mutex);
  if (!ctx->cancel_requested) {
    for (size_t i = 0u; i < TURBO_TTS_SAMPLES_PER_FRAME; ++i)
      ctx->frame[i] = (int16_t)(1000u + (uint32_t)i);
    ctx->frame_ready = 1;
    pthread_cond_broadcast(&ctx->changed);
  }

  while (!ctx->cancel_requested && !ctx->finish_requested)
    (void)pthread_cond_wait(&ctx->changed, &ctx->mutex);
  while (!ctx->allow_settle)
    (void)pthread_cond_wait(&ctx->changed, &ctx->mutex);

  /* Terminal is a notification that this worker has no more callbacks
   * or PCM. The host must still pthread_join before actual DSO unload. */
  ctx->worker_done = 1;
  ctx->counts.worker_done = 1u;
  pthread_cond_broadcast(&ctx->changed);
  (void)pthread_mutex_unlock(&ctx->mutex);
  return NULL;
}

static int provider_begin(void *param, uint64_t generation,
                          const char *approved, size_t text_length,
                          const char *voice, const char *language,
                          const turbo_tts_format_t *format) {
  turbo_tts_test_dso_t *ctx = (turbo_tts_test_dso_t *)param;
  if (!ctx || !generation || !approved || !text_length || !voice ||
      !language || !format || format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_samples != TURBO_TTS_SAMPLES_PER_FRAME)
    return -1;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  if (ctx->begun) {
    (void)pthread_mutex_unlock(&ctx->mutex);
    return -1;
  }
  ctx->generation = generation;
  ctx->begun = 1;
  (void)pthread_mutex_unlock(&ctx->mutex);

  if (pthread_create(&ctx->worker, NULL, worker_main, ctx) != 0)
    return -1;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  ctx->thread_created = 1;
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static int provider_poll_pcm(void *param, uint64_t generation,
                             int16_t *dst, size_t capacity,
                             size_t *samples, turbo_tts_poll_t *kind) {
  turbo_tts_test_dso_t *ctx = (turbo_tts_test_dso_t *)param;
  if (!ctx || !dst || !samples || !kind ||
      capacity != TURBO_TTS_SAMPLES_PER_FRAME) return -1;
  *samples = 0u;
  *kind = TURBO_TTS_WOULD_BLOCK;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  if (!ctx->begun || generation != ctx->generation) {
    (void)pthread_mutex_unlock(&ctx->mutex);
    return -1;
  }
  ctx->counts.provider_polls++;
  if (ctx->cancel_requested) {
    if (ctx->late_pcm) {
      for (size_t i = 0u; i < capacity; ++i) dst[i] = (int16_t)7777;
      *samples = capacity;
      *kind = TURBO_TTS_PCM;
      ctx->late_pcm = 0;
      ctx->counts.late_pcm_polls++;
    } else if (ctx->worker_done) {
      *kind = TURBO_TTS_POLL_CANCELED;
    }
  } else if (ctx->frame_ready) {
    memcpy(dst, ctx->frame, sizeof(ctx->frame));
    memset(ctx->frame, 0, sizeof(ctx->frame));
    ctx->frame_ready = 0;
    *samples = capacity;
    *kind = TURBO_TTS_PCM;
    ctx->counts.pcm_polls++;
  } else if (ctx->worker_done) {
    *kind = TURBO_TTS_TERMINAL;
  }
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static void provider_request_cancel(void *param, uint64_t generation) {
  turbo_tts_test_dso_t *ctx = (turbo_tts_test_dso_t *)param;
  if (!ctx) return;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return;
  if (ctx->begun && ctx->generation == generation &&
      !ctx->cancel_requested) {
    ctx->cancel_requested = 1;
    ctx->late_pcm = 1; /* malicious late frame must be rejected by M4.0 */
    ctx->counts.provider_cancel_requests++;
    pthread_cond_broadcast(&ctx->changed);
  }
  (void)pthread_mutex_unlock(&ctx->mutex);
}

static int test_create(turbo_tts_test_dso_t **out_ctx) {
  if (!out_ctx) return -1;
  *out_ctx = NULL;
  turbo_tts_test_dso_t *ctx =
      (turbo_tts_test_dso_t *)calloc(1u, sizeof(*ctx));
  if (!ctx) return -1;
  if (pthread_mutex_init(&ctx->mutex, NULL) != 0) {
    free(ctx);
    return -1;
  }
  if (pthread_cond_init(&ctx->changed, NULL) != 0) {
    pthread_mutex_destroy(&ctx->mutex);
    free(ctx);
    return -1;
  }
  *out_ctx = ctx;
  return 0;
}

static int test_configure_provider(turbo_tts_test_dso_t *ctx,
                                   turbo_tts_provider_t *out) {
  if (!ctx || !out) return -1;
  memset(out, 0, sizeof(*out));
  out->struct_size = sizeof(*out);
  out->abi_version = TURBO_TTS_ABI_VERSION;
  out->context = ctx;
  out->begin = provider_begin;
  out->poll_pcm = provider_poll_pcm;
  out->request_cancel = provider_request_cancel;
  return 0;
}

static int test_grant_pcm(turbo_tts_test_dso_t *ctx) {
  if (!ctx || pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  ctx->allow_pcm = 1;
  pthread_cond_broadcast(&ctx->changed);
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static int wait_flag(turbo_tts_test_dso_t *ctx, const int *flag) {
  if (!ctx || pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  struct timespec until;
  if (timespec_get(&until, TIME_UTC) != TIME_UTC) {
    (void)pthread_mutex_unlock(&ctx->mutex);
    return -1;
  }
  until.tv_sec += 10;
  while (!*flag) {
    if (pthread_cond_timedwait(&ctx->changed, &ctx->mutex, &until) != 0) {
      (void)pthread_mutex_unlock(&ctx->mutex);
      return -1;
    }
  }
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static int test_wait_worker_started(turbo_tts_test_dso_t *ctx) {
  return ctx ? wait_flag(ctx, &ctx->worker_started) : -1;
}

static int test_wait_pcm_ready(turbo_tts_test_dso_t *ctx) {
  return ctx ? wait_flag(ctx, &ctx->frame_ready) : -1;
}

static int test_grant_settle(turbo_tts_test_dso_t *ctx) {
  if (!ctx || pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  ctx->allow_settle = 1;
  ctx->finish_requested = 1; /* normal finish if cancel not requested */
  pthread_cond_broadcast(&ctx->changed);
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static int test_wait_worker_done(turbo_tts_test_dso_t *ctx) {
  return ctx ? wait_flag(ctx, &ctx->worker_done) : -1;
}

static int test_snapshot(turbo_tts_test_dso_t *ctx,
                         turbo_tts_test_dso_counts_t *out) {
  if (!ctx || !out || pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  *out = ctx->counts;
  (void)pthread_mutex_unlock(&ctx->mutex);
  return 0;
}

static int test_destroy(turbo_tts_test_dso_t **handle) {
  if (!handle || !*handle) return -1;
  turbo_tts_test_dso_t *ctx = *handle;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return -1;
  int pending = ctx->thread_created && !ctx->worker_done;
  int thread_created = ctx->thread_created;
  (void)pthread_mutex_unlock(&ctx->mutex);
  if (pending) return -2; /* no early Provider/DSO release */
  if (thread_created) {
    if (pthread_join(ctx->worker, NULL) != 0) return -3;
  }
  memset(ctx->frame, 0, sizeof(ctx->frame));
  pthread_cond_destroy(&ctx->changed);
  pthread_mutex_destroy(&ctx->mutex);
  memset(ctx, 0, sizeof(*ctx));
  free(ctx);
  *handle = NULL;
  return 0;
}

static const turbo_tts_test_dso_api_t test_api = {
  .struct_size = sizeof(turbo_tts_test_dso_api_t),
  .abi_version = TURBO_TTS_TEST_DSO_ABI,
  .set_unload_observer = set_unload_observer,
  .create = test_create,
  .configure_provider = test_configure_provider,
  .grant_pcm = test_grant_pcm,
  .wait_worker_started = test_wait_worker_started,
  .wait_pcm_ready = test_wait_pcm_ready,
  .grant_settle = test_grant_settle,
  .wait_worker_done = test_wait_worker_done,
  .snapshot = test_snapshot,
  .destroy = test_destroy
};

__attribute__((visibility("default")))
const turbo_tts_test_dso_api_t *turbo_tts_test_dso_entry(void) {
  return &test_api;
}
