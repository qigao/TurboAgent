#include "turbo_agent_harness.h"
#include "turbo_agent_voice_coordinator.h"
#include "turbo_agent_voice_harness_final_gate.h"
#include "tts_test_dso.h"

#include <salts/clock.h>
#include <salts/thread_pool.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Actual production Harness/Agent/Session from the original immutable C11
 * source, actual loadable TTS Provider .so and real pthread worker.
 * The input M1/M2/M3 positive is separately exercised in this SAME test
 * process (test_real_harness.c). Neither program opens an audio device.
 * This struct must mirror the test-only borrowed Playback sink in that TU. */
struct salts_playback {
  uint8_t bytes[4800];
  size_t used;
  size_t max_write;
  unsigned writes;
  unsigned clears;
  unsigned drains;
};

/* Implemented by original-M3 + M1/M2 source conformance in the same
 * executable. It supplies the ONLY checked 42 generation input gate. */
int m42_real_m3_checked_final_for_tts_dso(
    turbo_voice_coordinator_t *voice, char *transcript, size_t capacity);

#define CHECK_DSO(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL combined Harness/real TTS DSO line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)

typedef struct held_transport {
  atomic_int entered;
  atomic_int released;
} held_transport_t;

static int reply_ok(char **out) {
  static const char json[] =
      "{\"id\":\"m42-dso-real-harness\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"ok\"}]}]}";
  if (!out) return -1;
  *out = (char *)malloc(sizeof(json));
  if (!*out) return -1;
  memcpy(*out, json, sizeof(json));
  return 0;
}

static int local_transport(const char *request_json, char **response,
                           void *userdata) {
  (void)request_json;
  (void)userdata;
  return reply_ok(response);
}
static int held_local_transport(const char *request_json, char **response,
                                void *userdata) {
  (void)request_json;
  held_transport_t *hold = (held_transport_t *)userdata;
  atomic_store_explicit(&hold->entered, 1, memory_order_release);
  const uint64_t limit = cmeta_monotonic_ms() + 15000u;
  while (!atomic_load_explicit(&hold->released, memory_order_acquire)) {
    if (cmeta_monotonic_ms() > limit) return -1;
    cmeta_thread_yield();
  }
  return reply_ok(response);
}

static turbo_agent_harness_t *new_real_harness(
    cmeta_threadpool_t *pool, turbo_agent_transport_fn transport,
    void *userdata) {
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t cfg;
  session.runtime_store = turbo_agent_runtime_store_memory_create();
  session.agent_config.model = "local-dso-lifecycle-fixture";
  session.agent_config.transport_fn = transport;
  session.agent_config.transport_user_data = userdata;
  session.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
  app.session_config = &session;
  turbo_agent_harness_config_init(&cfg);
  cfg.app_config = &app;
  cfg.executor = pool;
  return turbo_agent_harness_create(&cfg);
}

static int start_and_complete(turbo_agent_harness_t *h,
                              const char *checked_input,
                              turbo_agent_harness_execution_t **out) {
  if (turbo_agent_harness_start_text(h, checked_input, NULL, out) !=
      SALTS_OK || !*out)
    return -1;
  if (turbo_agent_harness_execution_wait(*out, 20000u) != SALTS_OK)
    return -1;
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  int result = SALTS_EIO;
  if (turbo_agent_harness_execution_get_status(*out, &status) != SALTS_OK ||
      status != TURBO_AGENT_EXECUTION_COMPLETED ||
      turbo_agent_harness_execution_result_code(*out, &result) != SALTS_OK ||
      result != SALTS_OK)
    return -1;
  return 0;
}

static int run_real_harness_tts_dso(int scenario) {
  /* 0: finish-first, 1: interrupt after PCM, 2: interrupt before PCM,
   * 3: cancel original Harness before ANY TTS begins. */
  CHECK_DSO(scenario >= 0 && scenario <= 3);
  atomic_uint unload_count;
  atomic_init(&unload_count, 0u);
  void *image = dlopen(TEST_TTS_DSO_PATH, RTLD_NOW | RTLD_LOCAL);
  if (!image) {
    fprintf(stderr, "real Provider dlopen failed: %s\n", dlerror());
    return 1;
  }
  void *raw_entry = dlsym(image, "turbo_tts_test_dso_entry");
  CHECK_DSO(raw_entry != NULL);
  turbo_tts_test_dso_entry_t entry = NULL;
  _Static_assert(sizeof(entry) == sizeof(raw_entry),
                 "POSIX dlsym pointer sizes not compatible");
  memcpy(&entry, &raw_entry, sizeof(entry));
  CHECK_DSO(entry != NULL);
  const turbo_tts_test_dso_api_t *api = entry();
  CHECK_DSO(api && api->abi_version == TURBO_TTS_TEST_DSO_ABI &&
            api->struct_size == sizeof(*api));
  api->set_unload_observer(&unload_count);
  turbo_tts_test_dso_t *context = NULL;
  CHECK_DSO(api->create(&context) == 0 && context != NULL);
  turbo_tts_provider_t provider;
  CHECK_DSO(api->configure_provider(context, &provider) == 0);
  turbo_tts_config_t cfg;
  turbo_tts_config_init(&cfg);
  turbo_tts_output_t *source = NULL;
  CHECK_DSO(turbo_tts_create(&cfg, &provider, &source) == TURBO_TTS_OK);

  struct salts_playback fake_native = {0};
  fake_native.max_write = 160u; /* original M4.1 will retry exact suffix */
  turbo_voice_coordinator_t *voice = NULL;
  CHECK_DSO(turbo_voice_create(source, &fake_native, &voice) ==
            TURBO_VOICE_OK);
  CHECK_DSO(turbo_voice_begin_input(voice, 42u) == TURBO_VOICE_OK);
  char checked_input[128] = {0};
  CHECK_DSO(m42_real_m3_checked_final_for_tts_dso(
      voice, checked_input, sizeof(checked_input)) == 0);
  CHECK_DSO(strcmp(checked_input, " utterance") == 0);
  CHECK_DSO(turbo_voice_get_state(voice) == TURBO_VOICE_WAIT_APPROVAL);

  cmeta_threadpool_config_t pool_cfg = {
      .num_threads = 1u, .queue_capacity = 1u
  };
  cmeta_threadpool_t *pool = cmeta_threadpool_create_with_config(&pool_cfg);
  CHECK_DSO(pool != NULL);
  held_transport_t held;
  atomic_init(&held.entered, 0);
  atomic_init(&held.released, 0);
  turbo_agent_harness_t *h = new_real_harness(
      pool, scenario == 3 ? held_local_transport : local_transport,
      scenario == 3 ? &held : NULL);
  CHECK_DSO(h != NULL);
  turbo_agent_harness_execution_t *execution = NULL;

  if (scenario == 3) {
    /* The inference run is REALLY active while TTS Provider is loaded.
     * Cancelling Harness can never synthesize an assistant FINAL. */
    CHECK_DSO(turbo_agent_harness_start_text(
        h, checked_input, NULL, &execution) == SALTS_OK);
    const uint64_t deadline = cmeta_monotonic_ms() + 15000u;
    while (!atomic_load_explicit(&held.entered, memory_order_acquire) &&
           cmeta_monotonic_ms() < deadline)
      cmeta_thread_yield();
    CHECK_DSO(atomic_load_explicit(&held.entered, memory_order_acquire));
    CHECK_DSO(turbo_agent_harness_execution_cancel(
        execution, TURBO_CANCEL_USER) == SALTS_OK);
    atomic_store_explicit(&held.released, 1, memory_order_release);
    CHECK_DSO(turbo_agent_harness_execution_wait(execution, 20000u) ==
              SALTS_OK);
    turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
    CHECK_DSO(turbo_agent_harness_execution_get_status(
        execution, &status) == SALTS_OK &&
        status == TURBO_AGENT_EXECUTION_CANCELLED);
    CHECK_DSO(turbo_voice_admit_harness_completed_final(
        voice, 42u, h, execution, "cancelled-run", 1, "local", "en") ==
        TURBO_VOICE_EBUSY);
    CHECK_DSO(fake_native.writes == 0u);
    turbo_tts_test_dso_counts_t counts;
    CHECK_DSO(api->snapshot(context, &counts) == 0);
    CHECK_DSO(counts.worker_started == 0u &&
              counts.provider_cancel_requests == 0u &&
              counts.provider_polls == 0u);
    CHECK_DSO(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
    CHECK_DSO(turbo_voice_get_state(voice) == TURBO_VOICE_INTERRUPTED);
  } else {
    /* Production Harness COMPLETED, then strict Host FINAL admission calls
     * the genuine function pointer returned by dlopened Provider image. */
    CHECK_DSO(start_and_complete(h, checked_input, &execution) == 0);
    const char *last_id =
        turbo_agent_app_last_run_id(turbo_agent_harness_app(h));
    CHECK_DSO(last_id && last_id[0]);
    char expected_run[160];
    size_t idlen = strlen(last_id);
    CHECK_DSO(idlen && idlen < sizeof(expected_run));
    memcpy(expected_run, last_id, idlen + 1u);
    CHECK_DSO(turbo_voice_admit_harness_completed_final(
        voice, 42u, h, execution, expected_run, 0, "local", "en") ==
        TURBO_VOICE_EBUSY);
    CHECK_DSO(turbo_voice_admit_harness_completed_final(
        voice, 42u, h, execution, "stale-run", 1, "local", "en") ==
        TURBO_VOICE_ESTALE);
    CHECK_DSO(turbo_voice_admit_harness_completed_final(
        voice, 42u, h, execution, expected_run, 1, "local", "en") ==
        TURBO_VOICE_OK);
    CHECK_DSO(api->wait_worker_started(context) == 0);

    size_t written = 0u;
    if (scenario != 2) {
      CHECK_DSO(api->grant_pcm(context) == 0);
      CHECK_DSO(api->wait_pcm_ready(context) == 0);
      size_t total_written = 0u;
      for (unsigned flush_tick = 0u; flush_tick < 10u &&
           total_written < 960u; ++flush_tick) {
        CHECK_DSO(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
        total_written += written;
      }
      CHECK_DSO(total_written == 960u && fake_native.used == 960u);
      CHECK_DSO(fake_native.writes == 6u); /* six exact 160-byte writes */
      for (size_t k = 0u; k < 480u; ++k) {
        int16_t sample = 0;
        memcpy(&sample, fake_native.bytes + 2u*k, sizeof(sample));
        CHECK_DSO(sample == (int16_t)(1000u + k));
      }
    } else {
      CHECK_DSO(fake_native.writes == 0u && fake_native.used == 0u);
    }
    const unsigned before_interrupt_writes = fake_native.writes;
    if (scenario != 0) {
      CHECK_DSO(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
      turbo_voice_snapshot_t snap;
      turbo_voice_snapshot_init(&snap);
      CHECK_DSO(turbo_voice_get_snapshot(voice, &snap) == TURBO_VOICE_OK);
      CHECK_DSO(snap.state == TURBO_VOICE_CANCEL_PENDING &&
                snap.blockers == TURBO_VOICE_BLOCK_PROVIDER_SETTLE);
      CHECK_DSO(fake_native.used == 0u && fake_native.clears == 1u);
      /* The worker is alive in a genuine loadable DSO; ownership cannot
       * be reaped just because the Audio ring was cleared. */
      CHECK_DSO(turbo_voice_destroy(&voice) == TURBO_VOICE_EBUSY);
      CHECK_DSO(turbo_tts_destroy(&source) == TURBO_TTS_EBUSY);
      CHECK_DSO(api->destroy(&context) == -2 && context != NULL);
      CHECK_DSO(atomic_load_explicit(
          &unload_count, memory_order_acquire) == 0u);
      CHECK_DSO(turbo_voice_pump(voice, 1u, &written) ==
                TURBO_VOICE_EBUSY);
      CHECK_DSO(written == 0u &&
                fake_native.writes == before_interrupt_writes);
      turbo_tts_test_dso_counts_t c;
      CHECK_DSO(api->snapshot(context, &c) == 0);
      CHECK_DSO(c.worker_started == 1u && c.worker_done == 0u &&
                c.provider_cancel_requests == 1u &&
                c.late_pcm_polls == 1u);
      CHECK_DSO(turbo_voice_begin_input(voice, 43u) == TURBO_VOICE_EBUSY);
    }
    CHECK_DSO(api->grant_settle(context) == 0);
    CHECK_DSO(api->wait_worker_done(context) == 0);
    CHECK_DSO(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    if (scenario != 0) {
      CHECK_DSO(turbo_voice_get_state(voice) == TURBO_VOICE_INTERRUPTED);
      CHECK_DSO(fake_native.used == 0u &&
                fake_native.writes == before_interrupt_writes);
    } else {
      CHECK_DSO(turbo_voice_get_state(voice) == TURBO_VOICE_DRAIN_PENDING);
      CHECK_DSO(turbo_voice_drain(voice, 10u) == TURBO_VOICE_EBUSY);
      fake_native.used = 0u; /* external native callback would consume */
      CHECK_DSO(turbo_voice_drain(voice, 10u) == TURBO_VOICE_OK);
      CHECK_DSO(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
    }
  }

  /* The combined critical unload order: M4 source/Coordinator have no
   * callback pointers into Provider before its Owner and real worker join,
   * and the production Harness has itself completely settled and exited. */
  CHECK_DSO(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  CHECK_DSO(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  turbo_agent_harness_execution_release(execution);
  turbo_agent_harness_release(h);
  cmeta_threadpool_destroy(pool);
  turbo_tts_test_dso_counts_t final_counts;
  CHECK_DSO(api->snapshot(context, &final_counts) == 0);
  CHECK_DSO(final_counts.worker_done == (scenario == 3 ? 0u : 1u));
  CHECK_DSO(api->destroy(&context) == 0 && context == NULL);
  CHECK_DSO(dlclose(image) == 0);
  CHECK_DSO(atomic_load_explicit(
      &unload_count, memory_order_acquire) == 1u);
  if (scenario == 0)
    puts("PASS combined real Harness+TTS DSO: finish/PCM/drain/settle/join/dlclose");
  else if (scenario == 1)
    puts("PASS combined real Harness+TTS DSO: cancel-after-PCM/late-reject/join/dlclose");
  else if (scenario == 2)
    puts("PASS combined real Harness+TTS DSO: cancel-before-PCM/join/dlclose");
  else
    puts("PASS combined real Harness+TTS DSO: Harness-cancel/no-TTS/dlclose");
  return 0;
}

int m42_real_harness_tts_dso_conformance(void) {
  CHECK_DSO(run_real_harness_tts_dso(0) == 0);
  CHECK_DSO(run_real_harness_tts_dso(1) == 0);
  CHECK_DSO(run_real_harness_tts_dso(2) == 0);
  CHECK_DSO(run_real_harness_tts_dso(3) == 0);
  return 0;
}
