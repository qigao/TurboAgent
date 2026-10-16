#define _POSIX_C_SOURCE 200809L

#include "turbo_agent_harness.h"
#include "turbo_agent_voice_harness_final_gate.h"
#include "turbo_agent_speech_capture.h"
#include "turbo_agent_speech_whisper.h"
#include "tts_test_dso.h"

#include <salts_capture.h>
#include <salts_playback.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts/thread_pool.h>
#include <cmeta_crypto.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A full original-source C11 session driven by the ACTUAL published
 * Salts::Capture and Salts::Playback Linux PulseAudio backends.
 *
 * - No physical mic/speaker: only an audited private software null sink
 *   and its monitor, supplied by the isolated CI runner.
 * - Original M1/M2 source and immutable original M3 C11 Whisper provider.
 * - M3's v1.9.5 vendor inference is a deliberately TEST-ONLY ABI engine;
 *   model bytes are short test fixtures, never model weights.
 * - Original production Harness/Agent/Session DSO, real bounded executor.
 * - True dlopen'd TTS Provider DSO with a real pthread worker and
 *   settle/join/unload barrier; its PCM is deterministic, not real speech.
 * - Current HEAD M4.0/M4.1/M4.2 and strict Harness approval gate.
 * One generation, one Owner, one PCM queue/staging path, no fallback. */
#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL FULL released audio/Harness/DSO line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)

#define GENERATION 42u
#define MAX_AUDIO_WAIT_MS 6000u
#define MAX_TEXT 128u

static int require_virtual_audio_only(char *input_id, size_t id_capacity,
                                       salts_playback_device_t *output) {
  const char *guard = getenv("M42_VIRTUAL_PULSE_ONLY");
  const char *pulse = getenv("PULSE_SERVER");
  if (!guard || strcmp(guard, "1") != 0 ||
      !pulse || strncmp(pulse, "unix:", 5u) != 0) return -1;

  salts_capture_device_t capture_devices[SALTS_CAPTURE_MAX_DEVICES] = {{0}};
  const int capture_count =
      salts_capture_list_audio_devices(capture_devices, SALTS_CAPTURE_MAX_DEVICES);
  if (capture_count != 1 ||
      capture_devices[0].type != SALTS_CAPTURE_TYPE_AUDIO ||
      !strstr(capture_devices[0].name, "TurboAgentVirtual") ||
      !capture_devices[0].id[0] ||
      strlen(capture_devices[0].id) >= id_capacity)
    return -1;
  strcpy(input_id, capture_devices[0].id);

  salts_playback_device_t devices[SALTS_PLAYBACK_MAX_DEVICES] = {{0}};
  size_t output_count = 0u;
  if (salts_playback_list_devices(
          devices, SALTS_PLAYBACK_MAX_DEVICES, &output_count) != SALTS_PLAYBACK_OK ||
      output_count != 1u ||
      !strstr(devices[0].name, "TurboAgentVirtual") ||
      devices[0].id_size == 0u)
    return -1;
  *output = devices[0];
  return 0;
}

static int make_verified_test_model(turbo_agent_speech_whisper_t **out) {
  static const char bytes[] = "MOCK_C11_VALIDATED_MODEL";
  char path[] = "/tmp/ta-m42-real-duplex-model-XXXXXX";
  int descriptor = mkstemp(path);
  if (descriptor < 0) return -1;
  ssize_t wrote = write(descriptor, bytes, sizeof(bytes));
  int closed = close(descriptor);
  if (wrote != (ssize_t)sizeof(bytes) || closed != 0) {
    (void)unlink(path);
    return -1;
  }
  uint8_t sha[32];
  if (cmeta_sha256(bytes, sizeof(bytes), sha) != 0) {
    (void)unlink(path);
    return -1;
  }
  turbo_agent_speech_whisper_config_t cfg;
  turbo_agent_speech_whisper_config_init(&cfg);
  cfg.model_path = path;
  cfg.language = TURBO_AGENT_WHISPER_LANGUAGE_EN;
  cfg.cpu_threads = 1u;
  cfg.max_model_bytes = 1024u;
  cfg.max_utterance_frames = 64u;
  cfg.max_text_bytes = MAX_TEXT;
  cfg.max_decode_ms = 10000u;
  cfg.stationary_noise_guard = 0u;
  memcpy(cfg.expected_sha256, sha, sizeof(sha));
  const int rc = turbo_agent_speech_whisper_create(&cfg, out);
  cmeta_crypto_clear(sha, sizeof(sha));
  (void)unlink(path);
  return rc == TURBO_AGENT_SPEECH_OK && *out != NULL ? 0 : -1;
}

static int capture_to_original_checked_final(const char *exact_capture_id,
                                              turbo_voice_coordinator_t *voice,
                                              char *final, size_t capacity) {
  turbo_agent_speech_whisper_t *whisper = NULL;
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_salts_capture_t *capture = NULL;
  turbo_agent_speech_provider_v1_t provider;
  turbo_agent_speech_config_t cfg;
  REQUIRE(make_verified_test_model(&whisper) == 0);
  REQUIRE(turbo_agent_speech_whisper_provider(whisper, &provider) ==
          TURBO_AGENT_SPEECH_OK);
  turbo_agent_speech_config_init(&cfg);
  cfg.queue_frames = 128u;
  cfg.max_utterance_frames = 64u;
  cfg.max_text_bytes = MAX_TEXT;
  REQUIRE(turbo_agent_speech_create(&cfg, &provider, &input) ==
          TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_INPUT);
  REQUIRE(turbo_agent_speech_salts_capture_create(
      input, GENERATION, exact_capture_id, &capture) == TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_agent_speech_salts_capture_start(capture) ==
          TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_agent_speech_salts_capture_destroy(&capture) ==
          TURBO_AGENT_SPEECH_EBUSY && capture != NULL);

  size_t total_pumped = 0u;
  const uint64_t input_deadline = cmeta_monotonic_ms() + MAX_AUDIO_WAIT_MS;
  while (total_pumped == 0u) {
    REQUIRE(cmeta_monotonic_ms() < input_deadline);
    REQUIRE(turbo_agent_speech_salts_capture_first_error(capture) ==
            TURBO_AGENT_SPEECH_OK);
    size_t pumped = 0u;
    REQUIRE(turbo_agent_speech_pump(input, 8u, &pumped) ==
            TURBO_AGENT_SPEECH_OK);
    total_pumped += pumped;
    if (!pumped) cmeta_sleep_ms(2u);
  }
  /* The last native device callback must exit BEFORE model finish, checked
   * FINAL, Harness start or any source/Provider reclamation. */
  REQUIRE(turbo_agent_speech_salts_capture_finish(capture) ==
          TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_agent_speech_salts_capture_first_error(capture) ==
          TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_agent_speech_salts_capture_destroy(&capture) ==
          TURBO_AGENT_SPEECH_OK && capture == NULL);
  const uint64_t drain_limit = cmeta_monotonic_ms() + 2000u;
  while (turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_DRAINING) {
    REQUIRE(cmeta_monotonic_ms() < drain_limit);
    size_t pumped = 0u;
    REQUIRE(turbo_agent_speech_pump(input, 128u, &pumped) ==
            TURBO_AGENT_SPEECH_OK);
    total_pumped += pumped;
  }
  REQUIRE(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
  size_t bytes = 0u;
  turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
  REQUIRE(turbo_agent_speech_take_final(
      input, final, capacity, &bytes, &kind) == TURBO_AGENT_SPEECH_OK);
  REQUIRE(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
          bytes == 10u && strcmp(final, " utterance") == 0);
  char duplicate[32];
  REQUIRE(turbo_agent_speech_take_final(
      input, duplicate, sizeof(duplicate), &bytes, &kind) ==
      TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_voice_asr_event(
      voice, GENERATION + 1u, TURBO_VOICE_ASR_CHECKED_FINAL) ==
      TURBO_VOICE_ESTALE);
  REQUIRE(turbo_voice_asr_event(
      voice, GENERATION, TURBO_VOICE_ASR_CHECKED_FINAL) ==
      TURBO_VOICE_OK);
  REQUIRE(turbo_voice_input_quiesced(voice, GENERATION) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_WAIT_APPROVAL);

  REQUIRE(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  REQUIRE(turbo_agent_speech_whisper_destroy(&whisper) ==
          TURBO_AGENT_SPEECH_OK && whisper == NULL);
  puts("PASS shipped Capture callback stop/join -> original M1/M2/M3 one-shot checked FINAL");
  return 0;
}

static int local_reply(const char *request_json, char **response, void *unused) {
  static const char reply[] =
      "{\"id\":\"full-real-virtual\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"ok\"}]}]}";
  (void)request_json;
  (void)unused;
  if (!response) return -1;
  *response = (char *)malloc(sizeof(reply));
  if (!*response) return -1;
  memcpy(*response, reply, sizeof(reply));
  return 0;
}

static turbo_agent_harness_t *create_original_harness(cmeta_threadpool_t *pool) {
  turbo_agent_session_config_t sess = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t config;
  sess.runtime_store = turbo_agent_runtime_store_memory_create();
  sess.agent_config.model = "local-deterministic-virtual";
  sess.agent_config.transport_fn = local_reply;
  sess.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
  app.session_config = &sess;
  turbo_agent_harness_config_init(&config);
  config.app_config = &app;
  config.executor = pool;
  return turbo_agent_harness_create(&config);
}

static int genuine_harness_completed_and_approved(
    turbo_agent_harness_t *h, const char *checked_input,
    turbo_voice_coordinator_t *voice,
    turbo_agent_harness_execution_t **out_execution) {
  REQUIRE(turbo_agent_harness_start_text(
      h, checked_input, NULL, out_execution) == SALTS_OK);
  REQUIRE(*out_execution != NULL);
  REQUIRE(turbo_agent_harness_execution_wait(
      *out_execution, 20000u) == SALTS_OK);
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  int code = SALTS_EIO;
  REQUIRE(turbo_agent_harness_execution_get_status(
      *out_execution, &status) == SALTS_OK &&
      status == TURBO_AGENT_EXECUTION_COMPLETED);
  REQUIRE(turbo_agent_harness_execution_result_code(
      *out_execution, &code) == SALTS_OK && code == SALTS_OK);
  const char *run_id =
      turbo_agent_app_last_run_id(turbo_agent_harness_app(h));
  REQUIRE(run_id && run_id[0]);
  char expected[160];
  size_t n = strlen(run_id);
  REQUIRE(n && n < sizeof(expected));
  memcpy(expected, run_id, n + 1u);
  REQUIRE(turbo_voice_admit_harness_completed_final(
      voice, GENERATION, h, *out_execution, expected,
      0, "local", "en") == TURBO_VOICE_EBUSY);
  REQUIRE(turbo_voice_admit_harness_completed_final(
      voice, GENERATION, h, *out_execution, "old-run",
      1, "local", "en") == TURBO_VOICE_ESTALE);
  REQUIRE(turbo_voice_admit_harness_completed_final(
      voice, GENERATION, h, *out_execution, expected,
      1, "local", "en") == TURBO_VOICE_OK);
  return 0;
}

static int run_published_fullstack(int cancel_after_pcm) {
  char capture_id[128] = {0};
  salts_playback_device_t exact_playback = {0};
  REQUIRE(require_virtual_audio_only(capture_id, sizeof(capture_id),
                                     &exact_playback) == 0);
  salts_playback_config_t fmt = {
    24000u, 1u, SALTS_PLAYBACK_FORMAT_S16,
    cancel_after_pcm ? 100u : 10u
  };
  salts_playback_t *native = NULL;
  REQUIRE(salts_playback_create(&exact_playback, &fmt, &native) ==
          SALTS_PLAYBACK_OK && native != NULL);

  atomic_uint unloaded;
  atomic_init(&unloaded, 0u);
  void *image = dlopen(TEST_TTS_DSO_PATH, RTLD_NOW | RTLD_LOCAL);
  REQUIRE(image != NULL);
  void *symbol = dlsym(image, "turbo_tts_test_dso_entry");
  REQUIRE(symbol != NULL);
  turbo_tts_test_dso_entry_t entry = NULL;
  _Static_assert(sizeof(entry) == sizeof(symbol),
                 "POSIX function pointer size expected");
  memcpy(&entry, &symbol, sizeof(entry));
  const turbo_tts_test_dso_api_t *api = entry();
  REQUIRE(api && api->abi_version == TURBO_TTS_TEST_DSO_ABI &&
          api->struct_size == sizeof(*api));
  api->set_unload_observer(&unloaded);
  turbo_tts_test_dso_t *ctx = NULL;
  REQUIRE(api->create(&ctx) == 0 && ctx != NULL);
  turbo_tts_provider_t hooks;
  REQUIRE(api->configure_provider(ctx, &hooks) == 0);
  turbo_tts_config_t cfg;
  turbo_tts_config_init(&cfg);
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  REQUIRE(turbo_tts_create(&cfg, &hooks, &source) == TURBO_TTS_OK);
  REQUIRE(turbo_voice_create(source, native, &voice) == TURBO_VOICE_OK);
  REQUIRE(turbo_voice_begin_input(voice, GENERATION) == TURBO_VOICE_OK);
  char recognized[MAX_TEXT] = {0};
  REQUIRE(capture_to_original_checked_final(
      capture_id, voice, recognized, sizeof(recognized)) == 0);

  cmeta_threadpool_config_t pool_cfg = {
    .num_threads=1u, .queue_capacity=1u
  };
  cmeta_threadpool_t *pool =
      cmeta_threadpool_create_with_config(&pool_cfg);
  REQUIRE(pool != NULL);
  turbo_agent_harness_t *h = create_original_harness(pool);
  REQUIRE(h != NULL);
  turbo_agent_harness_execution_t *execution = NULL;
  REQUIRE(genuine_harness_completed_and_approved(
      h, recognized, voice, &execution) == 0);
  REQUIRE(api->wait_worker_started(ctx) == 0);
  REQUIRE(api->grant_pcm(ctx) == 0);
  REQUIRE(api->wait_pcm_ready(ctx) == 0);

  size_t written = 0u;
  if (!cancel_after_pcm) {
    /* Native 10ms ring accepts HALF the sole 20ms 960-byte frame. */
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    REQUIRE(written == 480u && salts_playback_get_buffered(native) == 480u);
    written = 99u;
    REQUIRE(turbo_voice_pump(voice, 1u, &written) ==
            TURBO_VOICE_OK && written == 0u);
    REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
    size_t accepted = 480u;
    const uint64_t limit = cmeta_monotonic_ms() + 3000u;
    while (accepted < 960u) {
      REQUIRE(cmeta_monotonic_ms() < limit);
      written = 0u;
      REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
      accepted += written;
      cmeta_sleep_ms(1u);
    }
    REQUIRE(accepted == 960u);
    REQUIRE(api->grant_settle(ctx) == 0);
    REQUIRE(api->wait_worker_done(ctx) == 0);
    while (turbo_voice_get_state(voice) != TURBO_VOICE_DRAIN_PENDING) {
      REQUIRE(cmeta_monotonic_ms() < limit);
      REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
      cmeta_sleep_ms(1u);
    }
    REQUIRE(turbo_voice_drain(voice, 1000u) == TURBO_VOICE_OK);
    REQUIRE(salts_playback_get_buffered(native) == 0u);
    REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
  } else {
    /* Real device actively playing; clearing it must not release the
     * uncooperative Provider DSO worker or submit any late PCM. */
    REQUIRE(salts_playback_start(native) == SALTS_PLAYBACK_OK);
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    REQUIRE(written == 960u);
    REQUIRE(turbo_voice_interrupt(voice, GENERATION) == TURBO_VOICE_OK);
    REQUIRE(salts_playback_get_buffered(native) == 0u);
    REQUIRE(salts_playback_get_state(native) ==
            SALTS_PLAYBACK_STATE_PLAYING);
    REQUIRE(turbo_voice_destroy(&voice) == TURBO_VOICE_EBUSY);
    REQUIRE(turbo_tts_destroy(&source) == TURBO_TTS_EBUSY);
    REQUIRE(api->destroy(&ctx) == -2 && ctx != NULL);
    REQUIRE(atomic_load_explicit(
        &unloaded, memory_order_acquire) == 0u);
    REQUIRE(turbo_voice_begin_input(voice, GENERATION + 1u) ==
            TURBO_VOICE_EBUSY);
    written = 99u;
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_EBUSY);
    REQUIRE(written == 0u);
    turbo_tts_test_dso_counts_t counters = {0};
    REQUIRE(api->snapshot(ctx, &counters) == 0);
    REQUIRE(counters.provider_cancel_requests == 1u &&
            counters.late_pcm_polls == 1u && counters.worker_done == 0u);
    REQUIRE(api->grant_settle(ctx) == 0);
    REQUIRE(api->wait_worker_done(ctx) == 0);
    REQUIRE(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    REQUIRE(turbo_voice_get_state(voice) == TURBO_VOICE_INTERRUPTED);
    REQUIRE(salts_playback_get_buffered(native) == 0u);
  }

  REQUIRE(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  REQUIRE(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  turbo_agent_harness_execution_release(execution);
  turbo_agent_harness_release(h);
  cmeta_threadpool_destroy(pool);
  REQUIRE(salts_playback_stop(native) == SALTS_PLAYBACK_OK);
  salts_playback_destroy(native);
  REQUIRE(api->destroy(&ctx) == 0 && ctx == NULL);
  REQUIRE(dlclose(image) == 0);
  REQUIRE(atomic_load_explicit(
      &unloaded, memory_order_acquire) == 1u);

  if (cancel_after_pcm)
    puts("PASS full published Capture+Playback + actual Harness + true TTS DSO: cancel/clear/late-reject/join/unload");
  else
    puts("PASS full published Capture+Playback + actual Harness + true TTS DSO: 480-byte backpressure/native drain/join/unload");
  return 0;
}
int main(void) {
  REQUIRE(run_published_fullstack(0) == 0);
  REQUIRE(run_published_fullstack(1) == 0);
  puts("PASS same-process actual M1/M2/M3 -> Harness -> loaded TTS worker -> BOTH published Salts audio DSOs");
  return 0;
}
