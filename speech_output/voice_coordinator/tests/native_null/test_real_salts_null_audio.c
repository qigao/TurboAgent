#include "turbo_agent_voice_coordinator.h"
#include <salts_playback.h>
#include <salts/clock.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Original release SaltsUtils v4.3.0-rc.11 playback sources are compiled with
 * SALTS_PLAYBACK_TEST_NULL_BACKEND (the original upstream test-only switch).
 * This file DOES NOT implement salts_playback_*; a REAL miniaudio native
 * callback, device state and original SPSC ring service all operations.
 *
 * This is deliberately an explicit, non-physical test device. Separately,
 * the real installed Salts::Capture DSO checks OS context/invalid device IDs;
 * no microphone is opened and no physical audio playback is claimed. */
#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL native Salts M4.2 null-device line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)

typedef struct native_states {
  salts_playback_state_t states[16];
  unsigned count;
} native_states_t;
static void observe_state(salts_playback_t *p, salts_playback_state_t state,
                          void *userdata) {
  (void)p;
  native_states_t *trace = (native_states_t *)userdata;
  if (trace->count < sizeof(trace->states) / sizeof(trace->states[0]))
    trace->states[trace->count++] = state;
}

/* These explicit test hooks are present only in the SOURCE's original
 * SALTS_PLAYBACK_TEST_NULL_BACKEND branch, not in the released library. */
void salts_playback_test_simulate_unexpected_stop(salts_playback_t *);
void salts_playback_test_stop_during_next_start(salts_playback_t *);

static salts_playback_config_t native_format(uint32_t buffer_ms) {
  salts_playback_config_t fmt = {
    24000u, 1u, SALTS_PLAYBACK_FORMAT_S16, buffer_ms
  };
  return fmt;
}
static int open_native_null(salts_playback_t **out, uint32_t buffer_ms) {
  salts_playback_config_t config = native_format(buffer_ms);
  *out = NULL;
  if (salts_playback_create(NULL, &config, out) != SALTS_PLAYBACK_OK)
    return -1;
  return *out == NULL ? -1 : 0;
}

typedef struct tts_provider {
  uint64_t generation;
  uint32_t starts;
  uint32_t pcm_calls;
  uint32_t polls;
  uint32_t cancels;
  int emitted;
  int canceled;
} tts_provider_t;
static int tts_begin(void *p, uint64_t generation,
                     const char *approved, size_t size, const char *voice,
                     const char *language, const turbo_tts_format_t *format) {
  tts_provider_t *t = (tts_provider_t *)p;
  if (!t || generation != 42u || !approved || size != 2u ||
      memcmp(approved, "ok", 2u) != 0 || !voice || !language || !format ||
      format->sample_rate_hz != 24000u || format->channels != 1u ||
      format->bits_per_sample != 16u || format->frame_samples != 480u)
    return -1;
  t->generation = generation;
  t->starts++;
  t->emitted = 0;
  t->canceled = 0;
  return 0;
}
static int tts_poll(void *p, uint64_t generation, int16_t *destination,
                    size_t capacity, size_t *out_samples,
                    turbo_tts_poll_t *out_kind) {
  tts_provider_t *t = (tts_provider_t *)p;
  if (!t || generation != t->generation || !destination ||
      capacity != TURBO_TTS_SAMPLES_PER_FRAME || !out_samples || !out_kind)
    return -1;
  t->polls++;
  if (t->canceled) {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_POLL_CANCELED;
    return 0;
  }
  if (t->emitted) {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_TERMINAL;
    return 0;
  }
  for (size_t i = 0u; i < capacity; ++i)
    destination[i] = (int16_t)(1000u + i);
  t->pcm_calls++;
  t->emitted = 1;
  *out_samples = capacity;
  *out_kind = TURBO_TTS_PCM;
  return 0;
}
static void tts_cancel(void *p, uint64_t generation) {
  tts_provider_t *t = (tts_provider_t *)p;
  if (t && t->generation == generation && !t->canceled) {
    t->canceled = 1;
    t->cancels++;
  }
}
static int make_voice(tts_provider_t *p, salts_playback_t *native,
                      turbo_tts_output_t **out_source,
                      turbo_voice_coordinator_t **out_voice) {
  turbo_tts_config_t cfg;
  turbo_tts_provider_t hooks;
  turbo_tts_config_init(&cfg);
  turbo_tts_provider_init(&hooks);
  hooks.context = p;
  hooks.begin = tts_begin;
  hooks.poll_pcm = tts_poll;
  hooks.request_cancel = tts_cancel;
  if (turbo_tts_create(&cfg, &hooks, out_source) != TURBO_TTS_OK)
    return -1;
  return turbo_voice_create(*out_source, native, out_voice);
}
static int approve_checked(tts_provider_t *p, salts_playback_t *native,
                           turbo_tts_output_t **out_source,
                           turbo_voice_coordinator_t **out_voice) {
  if (make_voice(p, native, out_source, out_voice) != TURBO_VOICE_OK)
    return -1;
  if (turbo_voice_begin_input(*out_voice, 42u) != TURBO_VOICE_OK ||
      turbo_voice_asr_event(*out_voice, 42u,
                            TURBO_VOICE_ASR_CHECKED_FINAL) != TURBO_VOICE_OK ||
      turbo_voice_input_quiesced(*out_voice, 42u) != TURBO_VOICE_OK ||
      turbo_voice_approved_final(*out_voice, 42u, "ok", 2u,
                                 "local", "en") != TURBO_VOICE_OK)
    return -1;
  return 0;
}
static int destroy_voice(turbo_tts_output_t **source,
                         turbo_voice_coordinator_t **voice) {
  return turbo_voice_destroy(voice) == TURBO_VOICE_OK &&
         turbo_tts_destroy(source) == TURBO_TTS_OK ? 0 : -1;
}

static int test_native_callback_drain_and_states(void) {
  salts_playback_t *native = NULL;
  REQUIRE(open_native_null(&native, 10u) == 0);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_STOPPED);
  const size_t cap = salts_playback_get_available(native);
  REQUIRE(cap == 480u); /* 10ms × 24kHz × mono × S16 */
  native_states_t trace = {{0}, 0u};
  REQUIRE(salts_playback_set_state_callback(
      native, observe_state, &trace) == SALTS_PLAYBACK_OK);
  int16_t pcm[480] = {0};
  pcm[0] = 12345;
  size_t written = 1u;
  REQUIRE(salts_playback_write(native, pcm, sizeof(pcm), &written) ==
          SALTS_PLAYBACK_OK);
  REQUIRE(written == cap && salts_playback_get_buffered(native) == cap);
  pcm[0] = 0; /* original native bounded ring borrowed no source bytes */
  REQUIRE(salts_playback_drain(native, 5u) == SALTS_PLAYBACK_ERR_BUSY);
  REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_PLAYING);
  REQUIRE(salts_playback_drain(native, 2000u) == SALTS_PLAYBACK_OK);
  REQUIRE(salts_playback_get_buffered(native) == 0u);
  REQUIRE(salts_playback_pause(native) == SALTS_PLAYBACK_OK);
  REQUIRE(salts_playback_resume(native) == SALTS_PLAYBACK_OK);
  REQUIRE(salts_playback_stop(native) == SALTS_PLAYBACK_OK);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_STOPPED);
  REQUIRE(trace.count == 6u);
  REQUIRE(trace.states[0] == SALTS_PLAYBACK_STATE_STARTING &&
          trace.states[1] == SALTS_PLAYBACK_STATE_PLAYING &&
          trace.states[2] == SALTS_PLAYBACK_STATE_PAUSED &&
          trace.states[3] == SALTS_PLAYBACK_STATE_PLAYING &&
          trace.states[4] == SALTS_PLAYBACK_STATE_STOPPING &&
          trace.states[5] == SALTS_PLAYBACK_STATE_STOPPED);
  salts_playback_destroy(native);
  puts("PASS original Salts Playback 24kHz null-native callback + drain + state transitions");
  return 0;
}

static int test_m42_device_backpressure_and_finish(void) {
  salts_playback_t *native = NULL;
  REQUIRE(open_native_null(&native, 10u) == 0);
  tts_provider_t provider = {0};
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  REQUIRE(approve_checked(&provider, native, &source, &voice) == 0);
  size_t n = 0u;
  REQUIRE(turbo_voice_pump(voice, 1u, &n) == TURBO_VOICE_OK);
  REQUIRE(n == 480u); /* actual native 10ms SPSC capacity, half 20ms block */
  REQUIRE(salts_playback_get_buffered(native) == 480u);
  REQUIRE(provider.pcm_calls == 1u && provider.polls == 1u);
  REQUIRE(turbo_voice_pump(voice, 1u, &n) == TURBO_VOICE_OK && n == 0u);
  REQUIRE(provider.polls == 1u); /* M4.1 cannot poll while staged bytes pending */
  REQUIRE(salts_playback_drain(native, 1u) == SALTS_PLAYBACK_ERR_BUSY);
  REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
  size_t accepted = 480u;
  const uint64_t deadline = cmeta_monotonic_ms() + 2000u;
  while (turbo_voice_get_state(voice) != TURBO_VOICE_DRAIN_PENDING) {
    REQUIRE(cmeta_monotonic_ms() < deadline);
    n = 0u;
    REQUIRE(turbo_voice_pump(voice, 1u, &n) == TURBO_VOICE_OK);
    accepted += n;
    cmeta_sleep_ms(1u);
  }
  REQUIRE(accepted == 960u && provider.polls == 2u &&
          provider.pcm_calls == 1u);
  /* SOURCE_DONE is insufficient: the ORIGINAL null native callback must
   * really consume any last retained PCM, never a fake sink size reset. */
  REQUIRE(turbo_voice_drain(voice, 1000u) == TURBO_VOICE_OK);
  REQUIRE(salts_playback_get_buffered(native) == 0u);
  REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
  REQUIRE(destroy_voice(&source, &voice) == 0);
  REQUIRE(salts_playback_stop(native) == SALTS_PLAYBACK_OK);
  salts_playback_destroy(native);
  puts("PASS M4.2 -> original Salts miniaudio native 10ms ring short-write + callback drain");
  return 0;
}

static int test_m42_interrupt_really_clears_native(void) {
  salts_playback_t *native = NULL;
  REQUIRE(open_native_null(&native, 100u) == 0);
  REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
  tts_provider_t provider = {0};
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  REQUIRE(approve_checked(&provider, native, &source, &voice) == 0);
  size_t written = 0u;
  REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  REQUIRE(written == 960u && provider.pcm_calls == 1u);
  /* This actually asks original Salts Playback to stop its native callback,
   * clear its true ring, then restart the null native device. */
  REQUIRE(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
  REQUIRE(salts_playback_get_buffered(native) == 0u);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_PLAYING);
  REQUIRE(provider.cancels == 1u);
  written = 99u;
  REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  REQUIRE(written == 0u && turbo_voice_get_state(voice) ==
                             TURBO_VOICE_INTERRUPTED);
  REQUIRE(turbo_voice_begin_input(voice, 43u) == TURBO_VOICE_OK);
  /* The Host must not destroy a still-open new input generation. */
  REQUIRE(turbo_voice_destroy(&voice) == TURBO_VOICE_EBUSY);
  REQUIRE(turbo_voice_interrupt(voice, 43u) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_input_quiesced(voice, 43u) == TURBO_VOICE_OK);
  REQUIRE(destroy_voice(&source, &voice) == 0);
  REQUIRE(salts_playback_stop(native) == SALTS_PLAYBACK_OK);
  salts_playback_destroy(native);
  puts("PASS M4.2 interrupted on running real null miniaudio callback: clear/stop/join");
  return 0;
}

static int test_original_native_error_fail_closed(void) {
  salts_playback_t *native = NULL;
  REQUIRE(open_native_null(&native, 100u) == 0);
  REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
  salts_playback_test_simulate_unexpected_stop(native);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_ERROR);
  int16_t pcm[16] = {0};
  size_t written = 777u;
  REQUIRE(salts_playback_write(native, pcm, sizeof(pcm), &written) ==
          SALTS_PLAYBACK_ERR_DEVICE && written == 0u);
  REQUIRE(salts_playback_clear(native) == SALTS_PLAYBACK_ERR_BUSY);
  salts_playback_destroy(native); /* native callback must be joined here */

  REQUIRE(open_native_null(&native, 10u) == 0);
  salts_playback_test_stop_during_next_start(native);
  REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_ERR_DEVICE);
  REQUIRE(salts_playback_get_state(native) == SALTS_PLAYBACK_STATE_ERROR);
  salts_playback_destroy(native);
  puts("PASS original Salts Playback native unexpected-stop and start-race fail closed");
  return 0;
}

int main(void) {
  REQUIRE(test_native_callback_drain_and_states() == 0);
  REQUIRE(test_m42_device_backpressure_and_finish() == 0);
  REQUIRE(test_m42_interrupt_really_clears_native() == 0);
  REQUIRE(test_original_native_error_fail_closed() == 0);
  puts("PASS pinned original SaltsUtils Playback NULL native backend + M4.2 lifecycle");
  return 0;
}
