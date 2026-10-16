#include "turbo_agent_voice_harness_final_gate.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_voice_coordinator.h"

#include <json_parser.h>
#include <salts/clock.h>
#include <salts/thread_pool.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Actual pinned TurboAgent::Harness/Agent/Session, NOT Harness function
 * doubles. Model inference uses a local deterministic transport, no network,
 * vendor model weights or tools. Native Playback remains TEST-ONLY fake
 * conforming to the exact published SaltsUtils public header. */
#define VERIFY(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL M4.2 real Harness line %d: %s\n", __LINE__, #x); \
  return 1; \
} } while (0)

struct salts_playback {
  uint8_t bytes[4800];
  size_t used;
  size_t max_write;
  unsigned writes;
  unsigned clears;
  unsigned drains;
};
int salts_playback_write(salts_playback_t *p, const void *pcm,
                         size_t bytes, size_t *written) {
  if (!written) return SALTS_PLAYBACK_ERR_FORMAT;
  *written = 0u;
  if (!p || !pcm || bytes % 2u) return SALTS_PLAYBACK_ERR_FORMAT;
  size_t space = sizeof(p->bytes) - p->used;
  size_t n = space < bytes ? space : bytes;
  if (n > p->max_write) n = p->max_write;
  n &= ~(size_t)1u;
  memcpy(p->bytes + p->used, pcm, n);
  p->used += n;
  p->writes++;
  *written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  p->clears++;
  memset(p->bytes, 0, sizeof(p->bytes));
  p->used = 0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || timeout_ms == 0u) return SALTS_PLAYBACK_ERR_FORMAT;
  p->drains++;
  return p->used ? SALTS_PLAYBACK_ERR_TIMEOUT : SALTS_PLAYBACK_OK;
}

typedef struct local_tts {
  uint64_t generation;
  unsigned begins;
  unsigned polls;
  unsigned cancels;
  unsigned frames;
} local_tts_t;
static int tts_begin(void *context, uint64_t generation,
                     const char *approved, size_t bytes,
                     const char *voice, const char *language,
                     const turbo_tts_format_t *format) {
  local_tts_t *p = context;
  if (!p || generation != 42u || !approved || bytes != 2u ||
      memcmp(approved, "ok", 2u) != 0 || !voice || !language ||
      !format || format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_samples != 480u)
    return -1;
  p->begins++;
  p->generation = generation;
  p->frames = 0u;
  return 0;
}
static int tts_poll(void *context, uint64_t generation,
                    int16_t *destination, size_t capacity,
                    size_t *out_samples, turbo_tts_poll_t *out_kind) {
  local_tts_t *p = context;
  if (!p || p->generation != generation || !destination ||
      !out_samples || !out_kind || capacity != 480u)
    return -1;
  p->polls++;
  if (!p->frames) {
    for (size_t i = 0u; i < 480u; ++i)
      destination[i] = (int16_t)(400u + i);
    *out_samples = 480u;
    *out_kind = TURBO_TTS_PCM;
    p->frames++;
  } else {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_TERMINAL;
  }
  return 0;
}
static void tts_cancel(void *context, uint64_t generation) {
  local_tts_t *p = context;
  if (p && p->generation == generation) p->cancels++;
}

typedef struct voice_fixture {
  local_tts_t provider;
  struct salts_playback sink;
  turbo_tts_output_t *source;
  turbo_voice_coordinator_t *voice;
} voice_fixture_t;
static int voice_create(voice_fixture_t *f) {
  turbo_tts_config_t cfg;
  turbo_tts_provider_t provider;
  memset(f, 0, sizeof(*f));
  f->sink.max_write = 160u;
  turbo_tts_config_init(&cfg);
  turbo_tts_provider_init(&provider);
  provider.context = &f->provider;
  provider.begin = tts_begin;
  provider.poll_pcm = tts_poll;
  provider.request_cancel = tts_cancel;
  if (turbo_tts_create(&cfg, &provider, &f->source) != TURBO_TTS_OK)
    return -1;
  if (turbo_voice_create(f->source, &f->sink, &f->voice) != TURBO_VOICE_OK)
    return -1;
  if (turbo_voice_begin_input(f->voice, 42u) != TURBO_VOICE_OK ||
      turbo_voice_asr_event(f->voice, 42u,
                            TURBO_VOICE_ASR_CHECKED_FINAL) != TURBO_VOICE_OK ||
      turbo_voice_input_quiesced(f->voice, 42u) != TURBO_VOICE_OK)
    return -1;
  return 0;
}
static int voice_destroy(voice_fixture_t *f) {
  if (!f || !f->voice || !f->source) return -1;
  if (turbo_voice_destroy(&f->voice) != TURBO_VOICE_OK) return -1;
  return turbo_tts_destroy(&f->source) == TURBO_TTS_OK ? 0 : -1;
}

/* These are real TurboAgent public transport callback signatures. */
static int make_local_reply(char **out_response_json) {
  static const char response[] =
      "{\"id\":\"m42_real_harness\",\"output\":[{\"type\":\"message\","
      "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
      "\"text\":\"ok\"}]}]}";
  if (!out_response_json) return -1;
  *out_response_json = malloc(sizeof(response));
  if (!*out_response_json) return -1;
  memcpy(*out_response_json, response, sizeof(response));
  return 0;
}
static int success_transport(const char *request_json,
                             char **out_response_json, void *context) {
  (void)request_json;
  (void)context;
  return make_local_reply(out_response_json);
}
typedef struct cancel_gate {
  atomic_int entered;
  atomic_int open;
} cancel_gate_t;
static int cancellable_transport(const char *request_json,
                                 char **out_response_json, void *context) {
  cancel_gate_t *gate = context;
  (void)request_json;
  atomic_store_explicit(&gate->entered, 1, memory_order_release);
  while (!atomic_load_explicit(&gate->open, memory_order_acquire))
    cmeta_thread_yield();
  return make_local_reply(out_response_json);
}

static turbo_agent_harness_t *create_harness(cmeta_threadpool_t *pool,
                                             turbo_agent_transport_fn transport,
                                             void *transport_context) {
  turbo_agent_session_config_t session = {0};
  turbo_agent_app_config_t app = {0};
  turbo_agent_harness_config_t cfg;
  session.runtime_store = turbo_agent_runtime_store_memory_create();
  session.agent_config.model = "local-fixture";
  session.agent_config.transport_fn = transport;
  session.agent_config.transport_user_data = transport_context;
  session.workflow_kind = TURBO_AGENT_SESSION_WORKFLOW_LOOP;
  app.session_config = &session;
  turbo_agent_harness_config_init(&cfg);
  cfg.app_config = &app;
  cfg.executor = pool;
  return turbo_agent_harness_create(&cfg);
}
static cmeta_threadpool_t *create_pool(void) {
  cmeta_threadpool_config_t cfg = {
      .num_threads = 1u, .queue_capacity = 1u
  };
  return cmeta_threadpool_create_with_config(&cfg);
}

static int test_actual_completed_harness(void) {
  voice_fixture_t f;
  cmeta_threadpool_t *pool = create_pool();
  VERIFY(pool);
  turbo_agent_harness_t *harness = create_harness(pool, success_transport, NULL);
  VERIFY(harness);
  VERIFY(voice_create(&f) == 0);
  turbo_agent_harness_execution_t *exec = NULL;
  VERIFY(turbo_agent_harness_start_text(harness, "checked ASR input",
                                         NULL, &exec) == SALTS_OK);
  VERIFY(exec != NULL);
  VERIFY(turbo_agent_harness_execution_wait(exec, 20000u) == SALTS_OK);
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  int result_code = SALTS_EIO;
  VERIFY(turbo_agent_harness_execution_get_status(exec, &status) == SALTS_OK);
  VERIFY(status == TURBO_AGENT_EXECUTION_COMPLETED);
  VERIFY(turbo_agent_harness_execution_result_code(exec, &result_code) == SALTS_OK);
  VERIFY(result_code == SALTS_OK);
  const char *current_id =
      turbo_agent_app_last_run_id(turbo_agent_harness_app(harness));
  VERIFY(current_id && current_id[0]);
  char expected[160];
  size_t length = strlen(current_id);
  VERIFY(length > 0u && length < sizeof(expected));
  memcpy(expected, current_id, length + 1u);

  /* A real execution is complete, but TTS must not run on unapproved text
   * or an unrelated run ID; those denials must not consume the result. */
  VERIFY(turbo_voice_admit_harness_completed_final(
      f.voice, 42u, harness, exec, expected, 0, "local", "en") ==
      TURBO_VOICE_EBUSY);
  VERIFY(turbo_voice_admit_harness_completed_final(
      f.voice, 42u, harness, exec, "stale-run", 1, "local", "en") ==
      TURBO_VOICE_ESTALE);
  VERIFY(f.provider.begins == 0u && f.sink.writes == 0u);
  VERIFY(turbo_voice_admit_harness_completed_final(
      f.voice, 42u, harness, exec, expected, 1, "local", "en") ==
      TURBO_VOICE_OK);
  VERIFY(f.provider.begins == 1u);
  json_value_t *summary = NULL, *state = NULL;
  VERIFY(turbo_agent_harness_execution_take_result(exec, &summary, &state) ==
         SALTS_EALREADY);
  VERIFY(summary == NULL && state == NULL);

  size_t written = 0u;
  for (unsigned i = 0u; i < 20u &&
       turbo_voice_get_state(f.voice) != TURBO_VOICE_DRAIN_PENDING; ++i)
    VERIFY(turbo_voice_pump(f.voice, 1u, &written) == TURBO_VOICE_OK);
  VERIFY(turbo_voice_get_state(f.voice) == TURBO_VOICE_DRAIN_PENDING);
  VERIFY(f.sink.used == 960u && f.provider.polls == 2u);
  VERIFY(turbo_voice_drain(f.voice, 10u) == TURBO_VOICE_EBUSY);
  f.sink.used = 0u; /* TEST-ONLY native ring callback consumption */
  VERIFY(turbo_voice_drain(f.voice, 10u) == TURBO_VOICE_OK);
  VERIFY(turbo_voice_get_state(f.voice) == TURBO_VOICE_COMPLETED);

  VERIFY(voice_destroy(&f) == 0);
  turbo_agent_harness_execution_release(exec);
  turbo_agent_harness_release(harness);
  cmeta_threadpool_destroy(pool);
  puts("PASS real Harness COMPLETED + canonical final -> Host-approved TTS/drain");
  return 0;
}

static int test_actual_cancelled_harness(void) {
  cancel_gate_t transport = {0};
  atomic_init(&transport.entered, 0);
  atomic_init(&transport.open, 0);
  cmeta_threadpool_t *pool = create_pool();
  VERIFY(pool);
  turbo_agent_harness_t *harness =
      create_harness(pool, cancellable_transport, &transport);
  VERIFY(harness);
  voice_fixture_t f;
  VERIFY(voice_create(&f) == 0);
  turbo_agent_harness_execution_t *exec = NULL;
  VERIFY(turbo_agent_harness_start_text(harness, "cancel me", NULL,
                                         &exec) == SALTS_OK);
  VERIFY(exec != NULL);
  uint64_t deadline = cmeta_monotonic_ms() + 15000u;
  while (!atomic_load_explicit(&transport.entered, memory_order_acquire) &&
         cmeta_monotonic_ms() < deadline)
    cmeta_thread_yield();
  VERIFY(atomic_load_explicit(&transport.entered, memory_order_acquire));
  VERIFY(turbo_agent_harness_execution_cancel(exec, TURBO_CANCEL_USER) ==
         SALTS_OK);
  atomic_store_explicit(&transport.open, 1, memory_order_release);
  VERIFY(turbo_agent_harness_execution_wait(exec, 20000u) == SALTS_OK);
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  VERIFY(turbo_agent_harness_execution_get_status(exec, &status) == SALTS_OK);
  VERIFY(status == TURBO_AGENT_EXECUTION_CANCELLED);
  VERIFY(turbo_voice_admit_harness_completed_final(
      f.voice, 42u, harness, exec, "cancelled-run", 1, "local", "en") ==
      TURBO_VOICE_EBUSY);
  VERIFY(f.provider.begins == 0u && f.sink.writes == 0u);
  json_value_t *summary = NULL, *state = NULL;
  VERIFY(turbo_agent_harness_execution_take_result(exec, &summary, &state) ==
         SALTS_OK);
  VERIFY(summary && strcmp(json_get_string(summary, "status"),
                            "cancelled") == 0);
  turbo_runtime_json_destroy(state);
  turbo_runtime_json_destroy(summary);
  VERIFY(turbo_voice_interrupt(f.voice, 42u) == TURBO_VOICE_OK);
  VERIFY(voice_destroy(&f) == 0);
  turbo_agent_harness_execution_release(exec);
  turbo_agent_harness_release(harness);
  cmeta_threadpool_destroy(pool);
  puts("PASS real Harness CANCELLED cannot authorize speech or device write");
  return 0;
}

static int test_actual_timed_out_harness(void) {
  cmeta_threadpool_t *pool = create_pool();
  VERIFY(pool);
  turbo_agent_harness_t *harness =
      create_harness(pool, success_transport, NULL);
  VERIFY(harness);
  voice_fixture_t f;
  VERIFY(voice_create(&f) == 0);
  turbo_agent_harness_run_options_t opts;
  turbo_agent_harness_run_options_init(&opts);
  opts.deadline_mono_ms = cmeta_monotonic_ms();
  turbo_agent_harness_execution_t *exec = NULL;
  VERIFY(turbo_agent_harness_start_text(harness, "expired", &opts, &exec) ==
         SALTS_OK);
  VERIFY(turbo_agent_harness_execution_wait(exec, 20000u) == SALTS_OK);
  turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
  VERIFY(turbo_agent_harness_execution_get_status(exec, &status) == SALTS_OK);
  VERIFY(status == TURBO_AGENT_EXECUTION_TIMED_OUT);
  VERIFY(turbo_voice_admit_harness_completed_final(
      f.voice, 42u, harness, exec, "expired-run", 1, "local", "en") ==
      TURBO_VOICE_EBUSY);
  VERIFY(f.provider.begins == 0u && f.sink.writes == 0u);
  json_value_t *summary = NULL, *state = NULL;
  VERIFY(turbo_agent_harness_execution_take_result(exec, &summary, &state) ==
         SALTS_OK);
  VERIFY(summary && strcmp(json_get_string(summary, "status"),
                            "timed_out") == 0);
  turbo_runtime_json_destroy(state);
  turbo_runtime_json_destroy(summary);
  VERIFY(turbo_voice_interrupt(f.voice, 42u) == TURBO_VOICE_OK);
  VERIFY(voice_destroy(&f) == 0);
  turbo_agent_harness_execution_release(exec);
  turbo_agent_harness_release(harness);
  cmeta_threadpool_destroy(pool);
  puts("PASS real Harness TIMED_OUT cannot authorize TTS");
  return 0;
}

int main(void) {
  VERIFY(test_actual_completed_harness() == 0);
  VERIFY(test_actual_cancelled_harness() == 0);
  VERIFY(test_actual_timed_out_harness() == 0);
  puts("PASS real bounded executor/Agent/Session/Harness FINAL ownership gates");
  return 0;
}
