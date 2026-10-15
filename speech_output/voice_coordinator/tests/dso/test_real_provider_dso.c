#include "turbo_agent_voice_coordinator.h"
#include "tts_test_dso.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A deterministic test-only fake playback uses the exact pinned PUBLIC Salts
 * header. It does not create any real output device or native sound callback.
 * This test's real boundaries are dlopen/dlclose and a true C11 TTS worker. */
struct salts_playback {
  uint8_t ring[4800];
  size_t size;
  unsigned writes;
  unsigned clears;
};
int salts_playback_write(salts_playback_t *p, const void *pcm, size_t bytes,
                         size_t *out_written) {
  if (!out_written) return SALTS_PLAYBACK_ERR_FORMAT;
  *out_written = 0u;
  if (!p || !pcm || (bytes % sizeof(int16_t))) return SALTS_PLAYBACK_ERR_FORMAT;
  p->writes++;
  size_t available = sizeof(p->ring) - p->size;
  size_t n = bytes < available ? bytes : available;
  memcpy(p->ring + p->size, pcm, n);
  p->size += n;
  *out_written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  p->clears++;
  memset(p->ring, 0, sizeof(p->ring));
  p->size = 0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || timeout_ms == 0u) return SALTS_PLAYBACK_ERR_FORMAT;
  return p->size == 0u ? SALTS_PLAYBACK_OK : SALTS_PLAYBACK_ERR_TIMEOUT;
}

#define REQUIRE(expr) do { if (!(expr)) { \
  fprintf(stderr, "FAIL real M4.2 DSO line %d: %s\n", __LINE__, #expr); \
  return 1; } } while (0)

static int run_real_dso_case(int mode) {
  const int cancel_first = mode != 0;
  const int cancel_before_frame = mode == 2;
  atomic_uint unloads;
  atomic_init(&unloads, 0u);
  fprintf(stderr, "DSO CASE %d: begin\n", cancel_first);
  void *module = dlopen(TEST_TTS_DSO_PATH, RTLD_NOW | RTLD_LOCAL);
  if (module == NULL) {
    fprintf(stderr, "FAIL dlopen: %s\n", dlerror());
    return 1;
  }
  void *symbol = dlsym(module, "turbo_tts_test_dso_entry");
  REQUIRE(symbol != NULL);
  turbo_tts_test_dso_entry_t entry = NULL;
  _Static_assert(sizeof(entry) == sizeof(symbol),
                 "POSIX dlsym function pointer ABI mismatch");
  /* memcpy is used instead of a non-C11 void* -> function pointer cast. */
  memcpy(&entry, &symbol, sizeof(entry));
  REQUIRE(entry != NULL);
  const turbo_tts_test_dso_api_t *api = entry();
  REQUIRE(api && api->abi_version == TURBO_TTS_TEST_DSO_ABI &&
          api->struct_size == sizeof(*api));
  api->set_unload_observer(&unloads);
  fprintf(stderr, "DSO CASE %d: loaded\n", cancel_first);

  turbo_tts_test_dso_t *ctx = NULL;
  REQUIRE(api->create(&ctx) == 0 && ctx != NULL);
  turbo_tts_provider_t provider;
  REQUIRE(api->configure_provider(ctx, &provider) == 0);
  turbo_tts_config_t config;
  turbo_tts_config_init(&config);
  turbo_tts_output_t *tts = NULL;
  REQUIRE(turbo_tts_create(&config, &provider, &tts) == TURBO_TTS_OK);

  struct salts_playback device = {0};
  turbo_voice_coordinator_t *voice = NULL;
  REQUIRE(turbo_voice_create(tts, &device, &voice) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_begin_input(voice, 42u) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_asr_event(voice, 42u,
          TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_input_quiesced(voice, 42u) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_approved_final(voice, 42u, "Approved output", 15u,
                                     "fake", "en") == TURBO_VOICE_OK);
  fprintf(stderr, "DSO CASE %d: worker created\n", cancel_first);

  /* Linux DSO pthread worker must reach its start latch. Cancel-before-PCM
   * deliberately withholds grant_pcm, and therefore the ring stays empty. */
  REQUIRE(api->wait_worker_started(ctx) == 0);
  size_t written = 0u;
  if (!cancel_before_frame) {
    REQUIRE(api->grant_pcm(ctx) == 0);
    REQUIRE(api->wait_pcm_ready(ctx) == 0);
    fprintf(stderr, "DSO CASE %d: worker frame ready\n", mode);
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    REQUIRE(written == 960u && device.size == 960u);
    for (size_t i = 0u; i < 480u; ++i) {
      int16_t actual = 0;
      memcpy(&actual, device.ring + 2u*i, sizeof(actual));
      REQUIRE(actual == (int16_t)(1000u + i));
    }
  } else {
    REQUIRE(device.size == 0u && device.writes == 0u);
  }
  const unsigned original_writes = device.writes;
  fprintf(stderr, "DSO CASE %d: PCM handed off\n", cancel_first);
  turbo_voice_snapshot_t snapshot;
  turbo_voice_snapshot_init(&snapshot);

  if (cancel_first) {
    REQUIRE(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
    REQUIRE(device.size == 0u && device.clears == 1u);
    REQUIRE(turbo_voice_get_snapshot(voice, &snapshot) == TURBO_VOICE_OK);
    REQUIRE(snapshot.state == TURBO_VOICE_CANCEL_PENDING);
    REQUIRE(snapshot.blockers == TURBO_VOICE_BLOCK_PROVIDER_SETTLE);

    /* Worker has not signaled settlement. Do NOT call dlclose here: an
     * in-flight Provider DSO must remain mapped and destroys must refuse. */
    REQUIRE(api->destroy(&ctx) == -2 && ctx != NULL);
    REQUIRE(turbo_voice_destroy(&voice) == TURBO_VOICE_EBUSY);
    REQUIRE(turbo_tts_destroy(&tts) == TURBO_TTS_EBUSY);
    REQUIRE(atomic_load_explicit(&unloads, memory_order_acquire) == 0u);
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_EBUSY);
    REQUIRE(written == 0u && device.writes == original_writes);
    turbo_tts_test_dso_counts_t counts = {0};
    REQUIRE(api->snapshot(ctx, &counts) == 0);
    REQUIRE(counts.worker_started == 1u &&
            counts.provider_cancel_requests == 1u &&
            counts.late_pcm_polls == 1u &&
            counts.worker_done == 0u);
    REQUIRE(turbo_voice_begin_input(voice, 43u) == TURBO_VOICE_EBUSY);
  }

  /* External test host explicitly releases the worker/Provider terminal
   * barrier. This is the only authorization for joining and dlclose. */
  fprintf(stderr, "DSO CASE %d: release worker\n", cancel_first);
  REQUIRE(api->grant_settle(ctx) == 0);
  REQUIRE(api->wait_worker_done(ctx) == 0);
  fprintf(stderr, "DSO CASE %d: worker settled\n", cancel_first);
  REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  if (cancel_first) {
    REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_INTERRUPTED);
    REQUIRE(device.size == 0u && device.writes == original_writes);
  } else {
    REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_DRAIN_PENDING);
    REQUIRE(turbo_voice_drain(voice, 10u) == TURBO_VOICE_EBUSY);
    /* Host-owned fake device's callback now consumes the ring. */
    device.size = 0u;
    REQUIRE(turbo_voice_drain(voice, 10u) == TURBO_VOICE_OK);
    REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
  }

  REQUIRE(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  REQUIRE(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  turbo_tts_test_dso_counts_t counts = {0};
  REQUIRE(api->snapshot(ctx, &counts) == 0);
  REQUIRE(counts.worker_started == 1u && counts.worker_done == 1u);
  REQUIRE(api->destroy(&ctx) == 0 && ctx == NULL);
  fprintf(stderr, "DSO CASE %d: worker joined\n", cancel_first);
  /* No callback, borrowed PCM or worker remains after the explicit join. */
  REQUIRE(dlclose(module) == 0);
  fprintf(stderr, "DSO CASE %d: dlclose returned\n", cancel_first);
  REQUIRE(atomic_load_explicit(&unloads, memory_order_acquire) == 1u);
  printf("PASS real C11 DSO: %s, worker settled/joined, one dlclose\n",
         cancel_before_frame ? "cancel before first PCM" :
         (cancel_first ? "cancel after first PCM + late PCM discarded" :
                         "finish-first + independently drained"));
  return 0;
}

int main(void) {
  REQUIRE(run_real_dso_case(2) == 0);
  REQUIRE(run_real_dso_case(1) == 0);
  REQUIRE(run_real_dso_case(0) == 0);
  puts("PASS M4.2 real dlopen -> bounded worker -> settle -> join -> dlclose");
  return 0;
}
