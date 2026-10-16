#include "turbo_agent_voice_harness_final_gate.h"
#include "turbo_agent_harness.h"
#include "turbo_agent_voice_coordinator.h"
#include "turbo_agent_speech_capture.h"

#include <json_parser.h>
#include <salts/clock.h>
#include <salts/thread_pool.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
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
static int voice_create_pending(voice_fixture_t *f) {
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
  if (turbo_voice_begin_input(f->voice, 42u) != TURBO_VOICE_OK)
    return -1;
  return 0;
}
static int voice_create(voice_fixture_t *f) {
  if (voice_create_pending(f) != 0 ||
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


/* M4.2 end-to-end single-process qualification:
 * ORIGINAL SpeechInput M1 + CaptureBridge M2 and ORIGINAL Harness/Session
 * from the same exact immutable source revision; current-head Voice/M4.1
 * borrow handles. ASR inference, local transport and Playback are the ONLY
 * deterministic test doubles. No device open, network or extra PCM queue. */
typedef struct inprocess_asr {
  unsigned begins, frames, finishes, cancels;
  int first_sample, second_sample;
  int no_speech;
} inprocess_asr_t;
static int inprocess_begin(void *context, uint64_t generation) {
  inprocess_asr_t *s = (inprocess_asr_t *)context;
  if (!s || generation != 42u) return -1;
  s->begins++;
  return 0;
}
static int inprocess_frame(void *context, const int16_t *pcm,
                           size_t count, uint64_t timestamp_us,
                           turbo_agent_speech_transcript_t *out) {
  inprocess_asr_t *s = (inprocess_asr_t *)context;
  if (!s || !pcm || count != 320u || timestamp_us != 1000u || !out)
    return -1;
  s->frames++;
  s->first_sample = pcm[0];
  s->second_sample = pcm[1];
  *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_PARTIAL, "speculative", 11u
  };
  return 0;
}
static int inprocess_finish(void *context,
                            turbo_agent_speech_transcript_t *out) {
  inprocess_asr_t *s = (inprocess_asr_t *)context;
  if (!s || !out) return -1;
  s->finishes++;
  if (s->no_speech) {
    *out = (turbo_agent_speech_transcript_t){
        TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH, NULL, 0u
    };
  } else {
    *out = (turbo_agent_speech_transcript_t){
        TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, "voice input", 11u
    };
  }
  return 0;
}
static void inprocess_cancel(void *context) {
  inprocess_asr_t *s = (inprocess_asr_t *)context;
  if (s) s->cancels++;
}

typedef struct inprocess_capture {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  turbo_agent_speech_capture_bridge_t *bridge; /* borrowed */
  int start, callback_done, can_exit, callback_result;
  size_t callback_bytes;
} inprocess_capture_t;
static void *inprocess_capture_worker(void *context) {
  inprocess_capture_t *s = (inprocess_capture_t *)context;
  (void)pthread_mutex_lock(&s->mutex);
  while (!s->start) (void)pthread_cond_wait(&s->condition, &s->mutex);
  (void)pthread_mutex_unlock(&s->mutex);

  uint8_t raw[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES] = {0};
  raw[0] = 0x34u; raw[1] = 0x12u; /* signed 0x1234 LE */
  raw[2] = 0xffu; raw[3] = 0xffu; /* signed -1 */
  s->callback_result = turbo_agent_speech_capture_bridge_offer(
      s->bridge, raw, s->callback_bytes, 1000u);
  memset(raw, 0xaa, sizeof(raw)); /* borrowed data reused after offer */

  (void)pthread_mutex_lock(&s->mutex);
  s->callback_done = 1;
  (void)pthread_cond_broadcast(&s->condition);
  while (!s->can_exit)
    (void)pthread_cond_wait(&s->condition, &s->mutex);
  (void)pthread_mutex_unlock(&s->mutex);
  return NULL;
}
static int inprocess_capture_init(inprocess_capture_t *s,
                                   turbo_agent_speech_capture_bridge_t *bridge,
                                   size_t bytes) {
  memset(s, 0, sizeof(*s));
  s->bridge = bridge;
  s->callback_bytes = bytes;
  if (pthread_mutex_init(&s->mutex, NULL) != 0) return -1;
  if (pthread_cond_init(&s->condition, NULL) != 0) {
    pthread_mutex_destroy(&s->mutex);
    return -1;
  }
  return 0;
}
static void inprocess_capture_allow(inprocess_capture_t *s, int end_callback) {
  (void)pthread_mutex_lock(&s->mutex);
  if (end_callback) s->can_exit = 1;
  else s->start = 1;
  (void)pthread_cond_broadcast(&s->condition);
  (void)pthread_mutex_unlock(&s->mutex);
}
static int inprocess_capture_wait(inprocess_capture_t *s) {
  struct timespec limit;
  if (timespec_get(&limit, TIME_UTC) != TIME_UTC) return -1;
  limit.tv_sec += 10;
  if (pthread_mutex_lock(&s->mutex) != 0) return -1;
  while (!s->callback_done) {
    if (pthread_cond_timedwait(&s->condition, &s->mutex, &limit) != 0) {
      (void)pthread_mutex_unlock(&s->mutex);
      return -1;
    }
  }
  (void)pthread_mutex_unlock(&s->mutex);
  return 0;
}
static void inprocess_capture_destroy(inprocess_capture_t *s) {
  (void)pthread_cond_destroy(&s->condition);
  (void)pthread_mutex_destroy(&s->mutex);
}
static int inprocess_new_input(inprocess_asr_t *asr,
                               turbo_agent_speech_input_t **out_input) {
  turbo_agent_speech_config_t config;
  turbo_agent_speech_provider_v1_t callbacks;
  turbo_agent_speech_config_init(&config);
  turbo_agent_speech_provider_v1_init(&callbacks);
  config.queue_frames = 4u; /* real M1 allocated queue, no second queue */
  callbacks.context = asr;
  callbacks.begin = inprocess_begin;
  callbacks.accept_frame = inprocess_frame;
  callbacks.finish = inprocess_finish;
  callbacks.cancel = inprocess_cancel;
  return turbo_agent_speech_create(&config, &callbacks, out_input);
}

typedef enum inprocess_case {
  COMPOSED_FINAL_AND_HARNESS = 0,
  COMPOSED_NO_SPEECH = 1,
  COMPOSED_INVALID_PCM = 2,
  COMPOSED_HARNESS_CANCEL = 3,
  COMPOSED_HARNESS_TIMEOUT = 4,
  COMPOSED_INPUT_CANCEL = 5
} inprocess_case_t;

static int test_actual_input_harness_output(inprocess_case_t mode) {
  inprocess_asr_t asr = {0};
  asr.no_speech = mode == COMPOSED_NO_SPEECH;
  voice_fixture_t voice;
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_capture_bridge_t *capture = NULL;
  VERIFY(inprocess_new_input(&asr, &input) == TURBO_AGENT_SPEECH_OK);
  VERIFY(turbo_agent_speech_capture_bridge_create(
      input, 42u, &capture) == TURBO_AGENT_SPEECH_OK);
  VERIFY(voice_create_pending(&voice) == 0);
  VERIFY(turbo_agent_speech_capture_bridge_begin(capture) ==
         TURBO_AGENT_SPEECH_OK);
  inprocess_capture_t worker;
  VERIFY(inprocess_capture_init(&worker, capture,
      mode == COMPOSED_INVALID_PCM ?
      TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES - 1u :
      TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES) == 0);
  pthread_t thread;
  VERIFY(pthread_create(&thread, NULL, inprocess_capture_worker,
                        &worker) == 0);
  inprocess_capture_allow(&worker, 0);
  VERIFY(inprocess_capture_wait(&worker) == 0);
  VERIFY(worker.callback_result ==
         (mode == COMPOSED_INVALID_PCM ? TURBO_AGENT_SPEECH_EINVAL :
                                         TURBO_AGENT_SPEECH_OK));

  /* This is a real callback and a real M1 queue; no ASR provider has been
   * allowed to create a Harness run, tool authority or TTS while incomplete. */
  VERIFY(turbo_voice_get_state(voice.voice) == TURBO_VOICE_INPUT);
  VERIFY(voice.provider.begins == 0u && voice.sink.writes == 0u);
  turbo_voice_snapshot_t snap;
  turbo_voice_snapshot_init(&snap);
  VERIFY(turbo_voice_get_snapshot(voice.voice, &snap) == TURBO_VOICE_OK);
  VERIFY(snap.generation == 42u &&
         (snap.blockers & TURBO_VOICE_BLOCK_INPUT_QUIESCE) != 0u);
  size_t pumped = 0u;
  if (mode != COMPOSED_INVALID_PCM) {
    VERIFY(turbo_agent_speech_pump(input, 1u, &pumped) ==
           TURBO_AGENT_SPEECH_OK && pumped == 1u);
    char partial[32] = {0};
    size_t length = 0u;
    uint64_t revision = 0u;
    VERIFY(turbo_agent_speech_read_partial(input, partial, sizeof(partial),
           &length, &revision) == TURBO_AGENT_SPEECH_OK);
    VERIFY(length == 11u && revision == 1u &&
           strcmp(partial, "speculative") == 0);
    turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
    VERIFY(turbo_agent_speech_take_final(input, partial, sizeof(partial),
           &length, &kind) == TURBO_AGENT_SPEECH_ESTATE);
    VERIFY(asr.first_sample == 0x1234 && asr.second_sample == -1);
  }
  VERIFY(turbo_agent_speech_capture_bridge_stop_admission(capture) ==
         TURBO_AGENT_SPEECH_OK);
  VERIFY(turbo_voice_get_state(voice.voice) == TURBO_VOICE_INPUT);
  /* Stop device/callback gate first, then wait until callback thread fully
   * exits. Cannot call input_quiesced solely because offer() returned. */
  inprocess_capture_allow(&worker, 1);
  VERIFY(pthread_join(thread, NULL) == 0);
  inprocess_capture_destroy(&worker);
  uint8_t late[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES] = {0};
  VERIFY(turbo_agent_speech_capture_bridge_offer(
      capture, late, sizeof(late), 2000u) == TURBO_AGENT_SPEECH_ESTATE);

  int finish_result;
  if (mode == COMPOSED_INPUT_CANCEL) {
    finish_result = turbo_agent_speech_capture_bridge_cancel(capture);
    VERIFY(finish_result == TURBO_AGENT_SPEECH_OK);
  } else {
    finish_result = turbo_agent_speech_capture_bridge_finish(capture);
    VERIFY(finish_result == (mode == COMPOSED_INVALID_PCM ?
                            TURBO_AGENT_SPEECH_EINVAL :
                            TURBO_AGENT_SPEECH_OK));
  }
  VERIFY(turbo_agent_speech_capture_bridge_destroy(&capture) ==
         TURBO_AGENT_SPEECH_OK);

  if (mode == COMPOSED_INVALID_PCM || mode == COMPOSED_INPUT_CANCEL) {
    VERIFY(turbo_agent_speech_get_state(input) ==
           TURBO_AGENT_SPEECH_CANCELED);
    VERIFY(asr.cancels == 1u && asr.finishes == 0u);
    VERIFY(turbo_voice_interrupt(voice.voice, 42u) == TURBO_VOICE_OK);
    VERIFY(turbo_voice_input_quiesced(voice.voice, 42u) == TURBO_VOICE_OK);
  } else {
    VERIFY(turbo_agent_speech_pump(input, 1u, &pumped) ==
           TURBO_AGENT_SPEECH_OK && pumped == 0u);
    VERIFY(asr.finishes == 1u && asr.cancels == 0u);
    char transcript[64] = {0};
    size_t n = 0u;
    turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
    VERIFY(turbo_agent_speech_take_final(input, transcript,
           sizeof(transcript), &n, &kind) == TURBO_AGENT_SPEECH_OK);
    if (mode == COMPOSED_NO_SPEECH) {
      VERIFY(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH &&
             n == 0u && transcript[0] == '\0');
      VERIFY(turbo_voice_interrupt(voice.voice, 42u) == TURBO_VOICE_OK);
      VERIFY(turbo_voice_input_quiesced(voice.voice, 42u) == TURBO_VOICE_OK);
    } else {
      VERIFY(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
             n == 11u && strcmp(transcript, "voice input") == 0);
      VERIFY(turbo_agent_speech_take_final(input, transcript,
             sizeof(transcript), &n, &kind) == TURBO_AGENT_SPEECH_ESTATE);
      VERIFY(turbo_voice_asr_event(voice.voice, 43u,
             TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_ESTALE);
      VERIFY(turbo_voice_asr_event(voice.voice, 42u,
             TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_OK);
      VERIFY(turbo_voice_input_quiesced(voice.voice, 42u) == TURBO_VOICE_OK);
      VERIFY(turbo_voice_get_state(voice.voice) == TURBO_VOICE_WAIT_APPROVAL);

      /* The ONLY actual Harness execution occurs AFTER checked FINAL and
       * the native Capture callback has joined. The ASR transcript is
       * untrusted user input; Harness remains the sole tool authority. */
      cancel_gate_t gate = {0};
      atomic_init(&gate.entered, 0);
      atomic_init(&gate.open, 0);
      cmeta_threadpool_t *pool = create_pool();
      VERIFY(pool);
      turbo_agent_harness_t *harness = create_harness(
          pool, mode == COMPOSED_HARNESS_CANCEL ?
                    cancellable_transport : success_transport,
          mode == COMPOSED_HARNESS_CANCEL ? &gate : NULL);
      VERIFY(harness);
      turbo_agent_harness_execution_t *execution = NULL;
      turbo_agent_harness_run_options_t options;
      turbo_agent_harness_run_options_init(&options);
      if (mode == COMPOSED_HARNESS_TIMEOUT)
        options.deadline_mono_ms = cmeta_monotonic_ms();
      VERIFY(turbo_agent_harness_start_text(
          harness, transcript, &options, &execution) == SALTS_OK);
      VERIFY(execution != NULL);
      if (mode == COMPOSED_HARNESS_CANCEL) {
        const uint64_t deadline = cmeta_monotonic_ms() + 15000u;
        while (!atomic_load_explicit(&gate.entered, memory_order_acquire) &&
               cmeta_monotonic_ms() < deadline)
          cmeta_thread_yield();
        VERIFY(atomic_load_explicit(&gate.entered, memory_order_acquire));
        VERIFY(turbo_agent_harness_execution_cancel(
            execution, TURBO_CANCEL_USER) == SALTS_OK);
        atomic_store_explicit(&gate.open, 1, memory_order_release);
      }
      VERIFY(turbo_agent_harness_execution_wait(execution, 20000u) == SALTS_OK);
      turbo_agent_execution_status_t status = TURBO_AGENT_EXECUTION_FAILED;
      VERIFY(turbo_agent_harness_execution_get_status(execution, &status) ==
             SALTS_OK);
      if (mode == COMPOSED_HARNESS_CANCEL ||
          mode == COMPOSED_HARNESS_TIMEOUT) {
        VERIFY(status == (mode == COMPOSED_HARNESS_CANCEL ?
                          TURBO_AGENT_EXECUTION_CANCELLED :
                          TURBO_AGENT_EXECUTION_TIMED_OUT));
        VERIFY(turbo_voice_admit_harness_completed_final(
            voice.voice, 42u, harness, execution, "not-admissible",
            1, "local", "en") == TURBO_VOICE_EBUSY);
        VERIFY(voice.provider.begins == 0u && voice.sink.writes == 0u);
        json_value_t *summary = NULL, *state = NULL;
        VERIFY(turbo_agent_harness_execution_take_result(
            execution, &summary, &state) == SALTS_OK);
        VERIFY(summary != NULL);
        turbo_runtime_json_destroy(summary);
        turbo_runtime_json_destroy(state);
        VERIFY(turbo_voice_interrupt(voice.voice, 42u) == TURBO_VOICE_OK);
      } else {
        VERIFY(status == TURBO_AGENT_EXECUTION_COMPLETED);
        const char *last = turbo_agent_app_last_run_id(
            turbo_agent_harness_app(harness));
        VERIFY(last && last[0] != '\0');
        char run_id[160];
        size_t id_bytes = strlen(last);
        VERIFY(id_bytes && id_bytes < sizeof(run_id));
        memcpy(run_id, last, id_bytes + 1u);
        VERIFY(turbo_voice_admit_harness_completed_final(
            voice.voice, 42u, harness, execution, run_id,
            0, "local", "en") == TURBO_VOICE_EBUSY);
        VERIFY(turbo_voice_admit_harness_completed_final(
            voice.voice, 42u, harness, execution, "stale",
            1, "local", "en") == TURBO_VOICE_ESTALE);
        VERIFY(voice.provider.begins == 0u && voice.sink.writes == 0u);
        VERIFY(turbo_voice_admit_harness_completed_final(
            voice.voice, 42u, harness, execution, run_id,
            1, "local", "en") == TURBO_VOICE_OK);
        VERIFY(voice.provider.begins == 1u);
        json_value_t *summary = NULL, *state = NULL;
        VERIFY(turbo_agent_harness_execution_take_result(
            execution, &summary, &state) == SALTS_EALREADY);
        VERIFY(summary == NULL && state == NULL);
        VERIFY(turbo_voice_begin_input(voice.voice, 43u) ==
               TURBO_VOICE_EBUSY);
        size_t written = 0u;
        for (unsigned tick = 0u; tick < 24u &&
             turbo_voice_get_state(voice.voice) !=
             TURBO_VOICE_DRAIN_PENDING; ++tick)
          VERIFY(turbo_voice_pump(voice.voice, 1u, &written) ==
                 TURBO_VOICE_OK);
        VERIFY(turbo_voice_get_state(voice.voice) ==
               TURBO_VOICE_DRAIN_PENDING);
        VERIFY(voice.sink.used == 960u && voice.provider.polls == 2u);
        VERIFY(turbo_voice_drain(voice.voice, 10u) == TURBO_VOICE_EBUSY);
        voice.sink.used = 0u; /* simulated native device consumption */
        VERIFY(turbo_voice_drain(voice.voice, 10u) == TURBO_VOICE_OK);
        VERIFY(turbo_voice_get_state(voice.voice) == TURBO_VOICE_COMPLETED);
      }
      turbo_agent_harness_execution_release(execution);
      turbo_agent_harness_release(harness);
      cmeta_threadpool_destroy(pool);
    }
  }
  if (mode != COMPOSED_FINAL_AND_HARNESS)
    VERIFY(voice.provider.begins == 0u && voice.sink.writes == 0u);
  VERIFY(voice_destroy(&voice) == 0);
  VERIFY(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  VERIFY(asr.begins == 1u &&
         asr.finishes == (mode == COMPOSED_INVALID_PCM ||
                         mode == COMPOSED_INPUT_CANCEL ? 0u : 1u));
  return 0;
}

static int test_all_composed_cases(void) {
  VERIFY(test_actual_input_harness_output(COMPOSED_FINAL_AND_HARNESS) == 0);
  VERIFY(test_actual_input_harness_output(COMPOSED_NO_SPEECH) == 0);
  VERIFY(test_actual_input_harness_output(COMPOSED_INVALID_PCM) == 0);
  VERIFY(test_actual_input_harness_output(COMPOSED_HARNESS_CANCEL) == 0);
  VERIFY(test_actual_input_harness_output(COMPOSED_HARNESS_TIMEOUT) == 0);
  VERIFY(test_actual_input_harness_output(COMPOSED_INPUT_CANCEL) == 0);
  puts("PASS same-process original M1/M2 16k Capture callback->checked FINAL");
  puts("PASS same-generation real Harness approved final->M4.2 24k PCM/drain");
  puts("PASS NO_SPEECH, invalid/canceled input, Harness cancel/timeout no TTS");
  return 0;
}

int main(void) {
  VERIFY(test_actual_completed_harness() == 0);
  VERIFY(test_actual_cancelled_harness() == 0);
  VERIFY(test_actual_timed_out_harness() == 0);
  VERIFY(test_all_composed_cases() == 0);
  puts("PASS real bounded executor/Agent/Session/Harness FINAL ownership gates");
  return 0;
}
