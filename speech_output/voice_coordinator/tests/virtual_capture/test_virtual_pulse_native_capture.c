#include "turbo_agent_speech_capture.h"
#include <salts_capture.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

/* Test-only published SaltsUtils Capture device exercise against an isolated
 * PulseAudio null-sink MONITOR. No physical mic, no human voice, no implicit
 * default-device fallback. The actual M2 native adapter, M2 bridge and M1
 * bounded queue come from original immutable TurboAgent C11 production source.
 *
 * The virtual server is created by this branch's opt-in CI with ONLY the
 * native-protocol UNIX socket and one module-null-sink; it must expose exactly
 * one source. This test never creates a permitted default audio device. */
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL released Salts Capture virtual monitor line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)

#define MAX_CALLBACK_WAIT_MS 6000u
#define CALLBACK_PERIOD_BYTES TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES

typedef struct test_asr {
  uint64_t generation;
  unsigned began;
  unsigned accepted;
  unsigned completed;
  unsigned aborted;
  uint64_t last_timestamp_us;
} test_asr_t;

static int asr_begin(void *ctx, uint64_t generation) {
  test_asr_t *asr = (test_asr_t *)ctx;
  if (!asr || generation != asr->generation) return -1;
  asr->began++;
  return 0;
}
static int asr_accept(void *ctx, const int16_t *samples,
                      size_t sample_count, uint64_t timestamp_us,
                      turbo_agent_speech_transcript_t *out) {
  test_asr_t *asr = (test_asr_t *)ctx;
  if (!asr || !samples || sample_count != 320u || !out)
    return -1;
  if (asr->accepted && timestamp_us <= asr->last_timestamp_us)
    return -1;
  asr->last_timestamp_us = timestamp_us;
  asr->accepted++;
  *out = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_NONE, NULL, 0u
  };
  return 0;
}
static int asr_finish(void *ctx, turbo_agent_speech_transcript_t *out) {
  test_asr_t *asr = (test_asr_t *)ctx;
  if (!asr || !out || asr->accepted == 0u)
    return -1;
  asr->completed++;
  static const char text[] = "virtual input admitted";
  *out = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, text, sizeof(text) - 1u
  };
  return 0;
}
static void asr_cancel(void *ctx) {
  test_asr_t *asr = (test_asr_t *)ctx;
  if (asr) asr->aborted++;
}

typedef struct direct_capture_observer {
  atomic_uint callbacks;
  atomic_uint malformed;
  atomic_uint_fast64_t first_timestamp_us;
  atomic_uint_fast64_t last_timestamp_us;
} direct_capture_observer_t;

static void direct_raw_native_callback(salts_capture_t *handle,
                                       const uint8_t *borrowed_pcm,
                                       size_t bytes,
                                       uint64_t timestamp_us, void *userdata) {
  (void)handle;
  direct_capture_observer_t *obs = (direct_capture_observer_t *)userdata;
  if (!obs) return;
  if (!borrowed_pcm || !bytes || bytes % 2u)
    (void)atomic_fetch_add_explicit(&obs->malformed, 1u, memory_order_relaxed);
  const unsigned prior = atomic_fetch_add_explicit(
      &obs->callbacks, 1u, memory_order_acq_rel);
  if (prior == 0u) {
    atomic_store_explicit(
        &obs->first_timestamp_us, timestamp_us, memory_order_release);
  } else {
    const uint64_t previous = atomic_load_explicit(
        &obs->last_timestamp_us, memory_order_acquire);
    if (timestamp_us <= previous)
      (void)atomic_fetch_add_explicit(
          &obs->malformed, 1u, memory_order_relaxed);
  }
  atomic_store_explicit(
      &obs->last_timestamp_us, timestamp_us, memory_order_release);
  /* Borrowed PCM is never persisted or accessed after this callback. */
}
static int wait_native_callbacks(atomic_uint *counter) {
  uint64_t limit = cmeta_monotonic_ms() + MAX_CALLBACK_WAIT_MS;
  while (atomic_load_explicit(counter, memory_order_acquire) == 0u) {
    if (cmeta_monotonic_ms() >= limit) return -1;
    cmeta_sleep_ms(2u);
  }
  return 0;
}

static int require_virtual_only_source(char *device_id, size_t id_bytes) {
  const char *explicit_opt_in = getenv("M42_VIRTUAL_PULSE_ONLY");
  const char *pulse_server = getenv("PULSE_SERVER");
  if (!explicit_opt_in || strcmp(explicit_opt_in, "1") != 0 ||
      !pulse_server || strncmp(pulse_server, "unix:", 5u) != 0)
    return -1;
  salts_capture_device_t devices[SALTS_CAPTURE_MAX_DEVICES] = {{0}};
  const int count = salts_capture_list_audio_devices(
      devices, SALTS_CAPTURE_MAX_DEVICES);
  if (count != 1) {
    fprintf(stderr, "Expected ONLY virtual capture source; got count=%d\n", count);
    return -1;
  }
  const salts_capture_device_t *dev = &devices[0];
  fprintf(stderr, "Capture isolation audit: name='%s', id='%s', type=%d\n",
          dev->name, dev->id, (int)dev->type);
  /* No empty name, physical mic or a generic host fallback may pass. */
  if (dev->type != SALTS_CAPTURE_TYPE_AUDIO ||
      !dev->id[0] || !dev->name[0] ||
      (!strstr(dev->name, "TurboAgentVirtual") &&
       !strstr(dev->name, "ta_ci_sink")))
    return -1;
  if (strlen(dev->id) >= id_bytes) return -1;
  strcpy(device_id, dev->id);
  return 0;
}

static int run_native_callback_and_join(const char *exact_device_id) {
  salts_audio_capture_config_t config = {16000, 1, 16, 20};
  salts_capture_t *capture = salts_audio_capture_create(
      exact_device_id, &config);
  CHECK(capture != NULL);
  CHECK(salts_capture_get_state(capture) == SALTS_CAPTURE_STATE_STOPPED);
  direct_capture_observer_t obs = {0};
  atomic_init(&obs.callbacks, 0u);
  atomic_init(&obs.malformed, 0u);
  atomic_init(&obs.first_timestamp_us, 0u);
  atomic_init(&obs.last_timestamp_us, 0u);
  salts_audio_capture_set_callback(capture, direct_raw_native_callback, &obs);
  CHECK(salts_capture_start(capture) == SALTS_CAPTURE_OK);
  CHECK(salts_capture_get_state(capture) == SALTS_CAPTURE_STATE_RUNNING);
  CHECK(wait_native_callbacks(&obs.callbacks) == 0);
  salts_capture_stop(capture); /* synchronous native ma_device_stop/join */
  CHECK(salts_capture_get_state(capture) == SALTS_CAPTURE_STATE_STOPPED);
  unsigned last_count = atomic_load_explicit(
      &obs.callbacks, memory_order_acquire);
  CHECK(last_count >= 1u);
  cmeta_sleep_ms(50u);
  CHECK(atomic_load_explicit(&obs.callbacks, memory_order_acquire) ==
        last_count);
  CHECK(atomic_load_explicit(&obs.malformed, memory_order_acquire) == 0u);
  salts_capture_destroy(capture);
#ifdef M42_CAPTURE_SOURCE_TSAN
  printf("PASS original source-TSan Salts Capture callback start/stop/join: native_callbacks=%u no late callbacks\n",
         last_count);
#else
  printf("PASS actual published Salts Capture callback start/stop/join: native_callbacks=%u no late callbacks\n",
         last_count);
#endif
  return 0;
}


/* Force an actual native callback to remain IN-FLIGHT during stop().
 * This exercises the OS backend's stop/join contract instead of merely
 * observing no callback after an already-fast callback returned. All control
 * calls stay serialized; callback runs on the real miniaudio device thread. */
typedef struct held_native_callback {
  pthread_mutex_t mutex;
  pthread_cond_t gate;
  salts_capture_t *capture; /* borrowed until stop worker is joined */
  int callback_entered;
  int release_callback;
  atomic_int stop_entered;
  atomic_int stop_returned;
  atomic_int callback_exited;
  atomic_int callback_deadline_failed;
  atomic_uint callbacks;
} held_native_callback_t;
static void held_native_pcm_callback(salts_capture_t *capture,
                                     const uint8_t *borrowed,
                                     size_t len, uint64_t timestamp,
                                     void *userdata) {
  (void)capture;
  (void)timestamp;
  held_native_callback_t *ctx = (held_native_callback_t *)userdata;
  if (!ctx) return;
  const unsigned index = atomic_fetch_add_explicit(
      &ctx->callbacks, 1u, memory_order_acq_rel);
  if (!borrowed || len != CALLBACK_PERIOD_BYTES)
    atomic_store_explicit(&ctx->callback_deadline_failed, 1, memory_order_release);
  if (index != 0u) return;
  if (pthread_mutex_lock(&ctx->mutex) != 0) return;
  ctx->callback_entered = 1;
  (void)pthread_cond_broadcast(&ctx->gate);
  struct timespec until;
  if (timespec_get(&until, TIME_UTC) != TIME_UTC) {
    atomic_store_explicit(&ctx->callback_deadline_failed, 1, memory_order_release);
  } else {
    until.tv_sec += 8;
    while (!ctx->release_callback) {
      if (pthread_cond_timedwait(&ctx->gate, &ctx->mutex, &until) != 0) {
        atomic_store_explicit(
            &ctx->callback_deadline_failed, 1, memory_order_release);
        break; /* watchdog RELEASES callback: no forced device destruction */
      }
    }
  }
  (void)pthread_mutex_unlock(&ctx->mutex);
  atomic_store_explicit(&ctx->callback_exited, 1, memory_order_release);
}
static void *native_stop_worker(void *userdata) {
  held_native_callback_t *ctx = (held_native_callback_t *)userdata;
  atomic_store_explicit(&ctx->stop_entered, 1, memory_order_release);
  salts_capture_stop(ctx->capture);
  atomic_store_explicit(&ctx->stop_returned, 1, memory_order_release);
  return NULL;
}
static int run_blocked_native_callback_stop_join(const char *exact_device_id) {
  salts_audio_capture_config_t config = {16000, 1, 16, 20};
  salts_capture_t *capture = salts_audio_capture_create(exact_device_id, &config);
  CHECK(capture != NULL);
  held_native_callback_t ctx = {0};
  ctx.capture = capture;
  CHECK(pthread_mutex_init(&ctx.mutex, NULL) == 0);
  CHECK(pthread_cond_init(&ctx.gate, NULL) == 0);
  atomic_init(&ctx.stop_entered, 0);
  atomic_init(&ctx.stop_returned, 0);
  atomic_init(&ctx.callback_exited, 0);
  atomic_init(&ctx.callback_deadline_failed, 0);
  atomic_init(&ctx.callbacks, 0u);
  salts_audio_capture_set_callback(capture, held_native_pcm_callback, &ctx);
  CHECK(salts_capture_start(capture) == SALTS_CAPTURE_OK);

  struct timespec limit;
  CHECK(timespec_get(&limit, TIME_UTC) == TIME_UTC);
  limit.tv_sec += 8;
  CHECK(pthread_mutex_lock(&ctx.mutex) == 0);
  while (!ctx.callback_entered) {
    if (pthread_cond_timedwait(&ctx.gate, &ctx.mutex, &limit) != 0) {
      (void)pthread_mutex_unlock(&ctx.mutex);
      CHECK(0 && "no native callback reached hold barrier");
    }
  }
  CHECK(pthread_mutex_unlock(&ctx.mutex) == 0);
  CHECK(atomic_load_explicit(&ctx.callback_exited, memory_order_acquire) == 0);

  pthread_t stopper;
  CHECK(pthread_create(&stopper, NULL, native_stop_worker, &ctx) == 0);
  const uint64_t started_limit = cmeta_monotonic_ms() + 1500u;
  while (!atomic_load_explicit(&ctx.stop_entered, memory_order_acquire)) {
    CHECK(cmeta_monotonic_ms() < started_limit);
    cmeta_sleep_ms(1u);
  }
  /* Must remain blocked while callback remains borrowed. Measure before
   * releasing, but always RELEASE before asserting, to avoid test deadlock. */
  cmeta_sleep_ms(40u);
  const int premature_stop = atomic_load_explicit(
      &ctx.stop_returned, memory_order_acquire);
  CHECK(pthread_mutex_lock(&ctx.mutex) == 0);
  ctx.release_callback = 1;
  CHECK(pthread_cond_broadcast(&ctx.gate) == 0);
  CHECK(pthread_mutex_unlock(&ctx.mutex) == 0);
  CHECK(pthread_join(stopper, NULL) == 0);
  CHECK(premature_stop == 0);
  CHECK(atomic_load_explicit(&ctx.callback_exited, memory_order_acquire) == 1);
  CHECK(atomic_load_explicit(&ctx.stop_returned, memory_order_acquire) == 1);
  CHECK(atomic_load_explicit(
      &ctx.callback_deadline_failed, memory_order_acquire) == 0);
  CHECK(salts_capture_get_state(capture) == SALTS_CAPTURE_STATE_STOPPED);
  unsigned final_count = atomic_load_explicit(
      &ctx.callbacks, memory_order_acquire);
  cmeta_sleep_ms(50u);
  CHECK(atomic_load_explicit(&ctx.callbacks, memory_order_acquire) ==
        final_count);
  CHECK(pthread_cond_destroy(&ctx.gate) == 0);
  CHECK(pthread_mutex_destroy(&ctx.mutex) == 0);
  salts_capture_destroy(capture); /* callback + stopper have BOTH joined */
  puts("PASS actual Capture stop blocks on held native callback, then joins before destroy");
  return 0;
}

static int run_original_m2_native_adapter(const char *exact_device_id,
                                          uint64_t generation) {
  test_asr_t asr = {.generation = generation};
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t hooks;
  turbo_agent_speech_config_init(&cfg);
  turbo_agent_speech_provider_v1_init(&hooks);
  cfg.queue_frames = 128u; /* bounded max, no dynamic expansion */
  cfg.max_utterance_frames = 128u;
  cfg.max_text_bytes = 128u;
  hooks.context = &asr;
  hooks.begin = asr_begin;
  hooks.accept_frame = asr_accept;
  hooks.finish = asr_finish;
  hooks.cancel = asr_cancel;

  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_salts_capture_t *native = NULL;
  CHECK(turbo_agent_speech_create(&cfg, &hooks, &input) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_create(
      input, generation, exact_device_id, &native) == TURBO_AGENT_SPEECH_OK);
  CHECK(native != NULL);
  CHECK(turbo_agent_speech_salts_capture_start(native) ==
        TURBO_AGENT_SPEECH_OK);
  /* No premature stop or destruction; native Capture still owns a callback. */
  CHECK(turbo_agent_speech_salts_capture_destroy(&native) ==
        TURBO_AGENT_SPEECH_EBUSY);
  CHECK(native != NULL);
  size_t total_pumped = 0u;
  const uint64_t limit = cmeta_monotonic_ms() + MAX_CALLBACK_WAIT_MS;
  while (!total_pumped) {
    CHECK(cmeta_monotonic_ms() < limit);
    CHECK(turbo_agent_speech_salts_capture_first_error(native) ==
          TURBO_AGENT_SPEECH_OK);
    size_t pumped = 0u;
    CHECK(turbo_agent_speech_pump(input, 8u, &pumped) ==
          TURBO_AGENT_SPEECH_OK);
    total_pumped += pumped;
    if (!total_pumped) cmeta_sleep_ms(2u);
  }
  /* Original adapter closes bridge admission BEFORE stopping the real
   * released Salts::Capture DSO. Its native ma_device_stop joins callbacks,
   * then and only then the original M1 is allowed to request FINAL. */
  CHECK(turbo_agent_speech_salts_capture_finish(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_first_error(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_destroy(&native) ==
        TURBO_AGENT_SPEECH_OK && native == NULL);
  const uint64_t drain_deadline = cmeta_monotonic_ms() + 1000u;
  while (turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_DRAINING) {
    CHECK(cmeta_monotonic_ms() < drain_deadline);
    size_t pumped = 0u;
    CHECK(turbo_agent_speech_pump(input, 128u, &pumped) ==
          TURBO_AGENT_SPEECH_OK);
    total_pumped += pumped;
  }
  CHECK(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
  char final[128] = {0};
  size_t final_bytes = 0u;
  turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
  CHECK(turbo_agent_speech_take_final(
      input, final, sizeof(final), &final_bytes, &kind) ==
      TURBO_AGENT_SPEECH_OK);
  CHECK(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
        final_bytes == strlen("virtual input admitted") &&
        strcmp(final, "virtual input admitted") == 0);
  CHECK(turbo_agent_speech_take_final(
      input, final, sizeof(final), &final_bytes, &kind) ==
      TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  CHECK(asr.began == 1u && asr.completed == 1u &&
        asr.aborted == 0u && asr.accepted == total_pumped);
  printf("PASS original M1/M2 native Capture Salts adapter: generation=%llu, frames=%u, one checked FINAL, joined callbacks\n",
         (unsigned long long)generation, asr.accepted);
  return 0;
}

static int run_original_native_cancel(const char *exact_device_id) {
  test_asr_t asr = {.generation = 43u};
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t hooks;
  turbo_agent_speech_config_init(&cfg);
  turbo_agent_speech_provider_v1_init(&hooks);
  cfg.queue_frames = 128u;
  cfg.max_utterance_frames = 128u;
  cfg.max_text_bytes = 128u;
  hooks.context = &asr;
  hooks.begin = asr_begin;
  hooks.accept_frame = asr_accept;
  hooks.finish = asr_finish;
  hooks.cancel = asr_cancel;
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_salts_capture_t *native = NULL;
  CHECK(turbo_agent_speech_create(&cfg, &hooks, &input) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_create(
      input, 43u, exact_device_id, &native) == TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_start(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_cancel(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_CANCELED);
  CHECK(turbo_agent_speech_salts_capture_finish(native) ==
        TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_salts_capture_destroy(&native) ==
        TURBO_AGENT_SPEECH_OK && native == NULL);
  CHECK(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  CHECK(asr.began == 1u && asr.completed == 0u && asr.aborted == 1u);
#ifdef M42_CAPTURE_SOURCE_TSAN
  puts("PASS original source-TSan Capture cancellation stop/join before original M1 Provider abort");
#else
  puts("PASS native published Capture cancellation stop/join before original M1 Provider abort");
#endif
  return 0;
}

int main(void) {
  char device_id[128] = {0};
  CHECK(require_virtual_only_source(device_id, sizeof(device_id)) == 0);
  CHECK(run_native_callback_and_join(device_id) == 0);
  CHECK(run_blocked_native_callback_stop_join(device_id) == 0);
  CHECK(run_original_m2_native_adapter(device_id, 42u) == 0);
  CHECK(run_original_native_cancel(device_id) == 0);
#ifdef M42_CAPTURE_SOURCE_TSAN
  puts("PASS original source-TSan Capture DSO + ORIGINAL M2 Salts adapter on isolated PulseAudio virtual-only source (NO physical microphone)");
#else
  puts("PASS released Capture DSO + ORIGINAL M2 Salts adapter on isolated PulseAudio virtual-only source (NO physical microphone)");
#endif
  return 0;
}
