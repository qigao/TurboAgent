#include "turbo_agent_speech_capture.h"
#include "turbo_agent_voice_coordinator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* M4.2 test-only HOST COMPOSITION of EXACT unmodified pinned M1/M2 input
 * source and M4.0/M4.1/M4.2 output source. No capture hardware, Agent
 * invoke, real model or second PCM queue. Capture callback runs on a real
 * pthread; only the Host owner may touch SpeechInput Provider/Voice APIs.
 * Trusted Harness approval remains external to this test-only fixture. */
#define CHECK(c) do { if (!(c)) { \
  fprintf(stderr,"FAIL M4.2 HOST line %d: %s\n", __LINE__, #c); \
  return 1; } } while (0)

struct salts_playback {
  uint8_t ring[4800];
  size_t buffered;
  size_t max_write;
  unsigned writes;
  unsigned clears;
  unsigned drains;
};
int salts_playback_write(salts_playback_t *p, const void *pcm,
                         size_t bytes, size_t *out_written) {
  if (!out_written) return SALTS_PLAYBACK_ERR_FORMAT;
  *out_written = 0u;
  if (!p || !pcm || bytes % 2u) return SALTS_PLAYBACK_ERR_FORMAT;
  p->writes++;
  size_t space = sizeof(p->ring) - p->buffered;
  size_t n = bytes < space ? bytes : space;
  if (n > p->max_write) n = p->max_write;
  n &= ~(size_t)1u;
  memcpy(p->ring + p->buffered, pcm, n);
  p->buffered += n;
  *out_written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  p->clears++;
  memset(p->ring, 0, sizeof(p->ring));
  p->buffered = 0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || !timeout_ms) return SALTS_PLAYBACK_ERR_FORMAT;
  p->drains++;
  return p->buffered ? SALTS_PLAYBACK_ERR_TIMEOUT : SALTS_PLAYBACK_OK;
}

typedef struct asr_stub {
  int begins, accepts, finishes, cancels;
  int first_sample, second_sample;
  int no_speech;
} asr_stub_t;
static int asr_begin(void *ctx, uint64_t generation) {
  asr_stub_t *p = ctx;
  if (!generation) return -1;
  p->begins++;
  return 0;
}
static int asr_accept(void *ctx, const int16_t *pcm, size_t count,
                      uint64_t timestamp_us,
                      turbo_agent_speech_transcript_t *out) {
  asr_stub_t *p = ctx;
  if (!pcm || count != 320u || !timestamp_us || !out) return -1;
  p->accepts++;
  p->first_sample = pcm[0];
  p->second_sample = pcm[1];
  *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_PARTIAL, "speculative", 11u
  };
  return 0;
}
static int asr_finish(void *ctx, turbo_agent_speech_transcript_t *out) {
  asr_stub_t *p = ctx;
  p->finishes++;
  if (p->no_speech)
    *out = (turbo_agent_speech_transcript_t){
        TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH, NULL, 0u
    };
  else
    *out = (turbo_agent_speech_transcript_t){
        TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, "Hello host", 10u
    };
  return 0;
}
static void asr_cancel(void *ctx) {
  ((asr_stub_t *)ctx)->cancels++;
}

typedef struct tts_stub {
  uint64_t generation;
  int begins, polls, cancels;
  int step, canceled;
} tts_stub_t;
static int tts_begin(void *ctx, uint64_t generation, const char *approved,
                     size_t count, const char *voice, const char *language,
                     const turbo_tts_format_t *format) {
  tts_stub_t *p = ctx;
  if (!generation || !approved || count != strlen("Approved answer") ||
      memcmp(approved, "Approved answer", count) != 0 ||
      !voice || !language || !format ||
      format->sample_rate_hz != 24000u || format->frame_samples != 480u ||
      format->channels != 1u || format->bits_per_sample != 16u) return -1;
  p->generation = generation;
  p->begins++;
  p->step = 0;
  p->canceled = 0;
  return 0;
}
static int tts_poll(void *ctx, uint64_t generation,
                    int16_t *pcm, size_t capacity, size_t *out_samples,
                    turbo_tts_poll_t *out_kind) {
  tts_stub_t *p = ctx;
  if (p->generation != generation || !pcm || capacity != 480u)
    return -1;
  p->polls++;
  if (p->canceled) {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_POLL_CANCELED;
  } else if (p->step == 0) {
    for (size_t i = 0u; i < 480u; ++i) pcm[i] = (int16_t)(2000u + i);
    *out_samples = 480u;
    *out_kind = TURBO_TTS_PCM;
  } else {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_TERMINAL;
  }
  p->step++;
  return 0;
}
static void tts_cancel(void *ctx, uint64_t generation) {
  tts_stub_t *p = ctx;
  if (p->generation == generation) { p->cancels++; p->canceled = 1; }
}

typedef struct capture_thread {
  pthread_mutex_t mutex;
  pthread_cond_t event;
  turbo_agent_speech_capture_bridge_t *bridge;
  int go, callback_done, allow_return;
  int callback_rc;
  size_t callback_bytes;
} capture_thread_t;
static void *borrowed_capture_callback(void *p) {
  capture_thread_t *w = p;
  pthread_mutex_lock(&w->mutex);
  while (!w->go) (void)pthread_cond_wait(&w->event, &w->mutex);
  pthread_mutex_unlock(&w->mutex);

  uint8_t pcm[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES] = {0};
  pcm[0] = 0x34u; pcm[1] = 0x12u; /* 0x1234 signed LE */
  pcm[2] = 0xffu; pcm[3] = 0xffu; /* -1 */
  w->callback_rc = turbo_agent_speech_capture_bridge_offer(
      w->bridge, pcm, w->callback_bytes, 1000u);
  memset(pcm, 0xcd, sizeof(pcm)); /* borrowed buffer immediately reused */

  pthread_mutex_lock(&w->mutex);
  w->callback_done = 1;
  pthread_cond_broadcast(&w->event);
  while (!w->allow_return)
    (void)pthread_cond_wait(&w->event, &w->mutex);
  pthread_mutex_unlock(&w->mutex);
  return NULL;
}
static int capture_thread_init(capture_thread_t *w,
                               turbo_agent_speech_capture_bridge_t *bridge,
                               size_t bytes) {
  memset(w, 0, sizeof(*w));
  w->bridge = bridge;
  w->callback_bytes = bytes;
  if (pthread_mutex_init(&w->mutex, NULL) != 0) return -1;
  if (pthread_cond_init(&w->event, NULL) != 0) {
    pthread_mutex_destroy(&w->mutex);
    return -1;
  }
  return 0;
}
static void capture_thread_allow(capture_thread_t *w, int release) {
  pthread_mutex_lock(&w->mutex);
  if (release) w->allow_return = 1;
  else w->go = 1;
  pthread_cond_broadcast(&w->event);
  pthread_mutex_unlock(&w->mutex);
}
static int capture_thread_wait_callback(capture_thread_t *w) {
  pthread_mutex_lock(&w->mutex);
  struct timespec deadline;
  if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
    pthread_mutex_unlock(&w->mutex);
    return -1;
  }
  deadline.tv_sec += 8;
  while (!w->callback_done) {
    if (pthread_cond_timedwait(&w->event, &w->mutex, &deadline) != 0) {
      pthread_mutex_unlock(&w->mutex);
      return -1;
    }
  }
  pthread_mutex_unlock(&w->mutex);
  return 0;
}
static void capture_thread_destroy(capture_thread_t *w) {
  pthread_cond_destroy(&w->event);
  pthread_mutex_destroy(&w->mutex);
}

static int create_input(asr_stub_t *asr, turbo_agent_speech_input_t **input) {
  turbo_agent_speech_provider_v1_t p;
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_init(&p);
  turbo_agent_speech_config_init(&cfg);
  cfg.queue_frames = 4u;
  p.context = asr;
  p.begin = asr_begin;
  p.accept_frame = asr_accept;
  p.finish = asr_finish;
  p.cancel = asr_cancel;
  return turbo_agent_speech_create(&cfg, &p, input);
}
static int create_voice(tts_stub_t *tts, salts_playback_t *sink,
                        turbo_tts_output_t **source,
                        turbo_voice_coordinator_t **voice) {
  turbo_tts_config_t cfg;
  turbo_tts_provider_t p;
  turbo_tts_config_init(&cfg);
  turbo_tts_provider_init(&p);
  p.context = tts;
  p.begin = tts_begin;
  p.poll_pcm = tts_poll;
  p.request_cancel = tts_cancel;
  if (turbo_tts_create(&cfg, &p, source) != TURBO_TTS_OK)
    return -1;
  return turbo_voice_create(*source, sink, voice);
}

/* The Host's actual M1 one-shot FINAL is the only source of an input
 * CHECKED_FINAL. It never invokes Agent; returned text remains untrusted,
 * and Harness must separately approve a committed user-visible reply. */
static int host_take_final(turbo_voice_coordinator_t *voice,
                           turbo_agent_speech_input_t *input,
                           uint64_t generation, char *out, size_t capacity) {
  size_t count = 0u;
  turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
  if (turbo_agent_speech_get_state(input) != TURBO_AGENT_SPEECH_FINAL)
    return TURBO_VOICE_EBUSY;
  if (turbo_agent_speech_take_final(input, out, capacity, &count,
                                   &kind) != TURBO_AGENT_SPEECH_OK ||
      kind != TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL || count == 0u)
    return TURBO_VOICE_EBUSY;
  if (turbo_voice_asr_event(voice, generation,
                           TURBO_VOICE_ASR_CHECKED_FINAL) != TURBO_VOICE_OK)
    return TURBO_VOICE_ESTALE;
  if (turbo_voice_input_quiesced(voice, generation) != TURBO_VOICE_OK)
    return TURBO_VOICE_EBUSY;
  return TURBO_VOICE_OK;
}

static int run_host_handoff(int mode) {
  const uint64_t generation = (uint64_t)(10 + mode);
  asr_stub_t asr = {0};
  tts_stub_t tts = {0};
  struct salts_playback sink = {0};
  sink.max_write = 160u;
  asr.no_speech = mode == 1;
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_capture_bridge_t *capture = NULL;
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  CHECK(create_input(&asr, &input) == TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_capture_bridge_create(
      input, generation, &capture) == TURBO_AGENT_SPEECH_OK);
  CHECK(create_voice(&tts, &sink, &source, &voice) == TURBO_VOICE_OK);
  CHECK(turbo_voice_begin_input(voice, generation) == TURBO_VOICE_OK);
  CHECK(turbo_agent_speech_capture_bridge_begin(capture) ==
        TURBO_AGENT_SPEECH_OK);
  capture_thread_t worker;
  CHECK(capture_thread_init(&worker, capture,
      mode == 2 ? TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES - 1u :
                  TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES) == 0);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, borrowed_capture_callback, &worker) == 0);
  capture_thread_allow(&worker, 0);
  CHECK(capture_thread_wait_callback(&worker) == 0);
  CHECK(worker.callback_rc == (mode == 2 ? TURBO_AGENT_SPEECH_EINVAL :
                                           TURBO_AGENT_SPEECH_OK));
  CHECK(turbo_voice_approved_final(voice, generation, "Approved answer",
             strlen("Approved answer"), "fake", "en") == TURBO_VOICE_EBUSY);
  CHECK(tts.begins == 0 && sink.writes == 0);
  char partial[32] = {0};
  size_t partial_bytes = 0u, done = 0u;
  uint64_t revision = 0u;
  if (mode != 2) {
    CHECK(turbo_agent_speech_pump(input, 1u, &done) ==
          TURBO_AGENT_SPEECH_OK && done == 1u);
    CHECK(turbo_agent_speech_read_partial(input, partial, sizeof(partial),
          &partial_bytes, &revision) == TURBO_AGENT_SPEECH_OK);
    CHECK(strcmp(partial, "speculative") == 0 && revision == 1u);
    char forbidden[32];
    turbo_agent_speech_transcript_kind_t kind;
    CHECK(turbo_agent_speech_take_final(input, forbidden,
          sizeof(forbidden), &done, &kind) == TURBO_AGENT_SPEECH_ESTATE);
    CHECK(asr.first_sample == 0x1234 && asr.second_sample == -1);
  }
  /* Actual Host sequencing: close admission, native stop/join callback,
   * then M2 finish. The Coordinator is NOT granted quiescence before join. */
  CHECK(turbo_agent_speech_capture_bridge_stop_admission(capture) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_voice_approved_final(voice, generation, "Approved answer",
             strlen("Approved answer"), "fake", "en") == TURBO_VOICE_EBUSY);
  capture_thread_allow(&worker, 1);
  CHECK(pthread_join(thread, NULL) == 0);
  capture_thread_destroy(&worker);
  CHECK(turbo_agent_speech_capture_bridge_offer(capture,
        (const uint8_t *)partial, TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES,
        2000u) == TURBO_AGENT_SPEECH_ESTATE);
  const int capture_rc = turbo_agent_speech_capture_bridge_finish(capture);
  CHECK(capture_rc == (mode == 2 ? TURBO_AGENT_SPEECH_EINVAL :
                                   TURBO_AGENT_SPEECH_OK));
  CHECK(turbo_agent_speech_capture_bridge_destroy(&capture) ==
        TURBO_AGENT_SPEECH_OK);
  if (mode == 2) {
    CHECK(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_CANCELED);
    CHECK(asr.cancels == 1 && asr.finishes == 0);
    CHECK(turbo_voice_interrupt(voice, generation) == TURBO_VOICE_OK);
    CHECK(turbo_voice_input_quiesced(voice, generation) == TURBO_VOICE_OK);
  } else {
    /* True M1 draining/finish and one-shot FINAL, not a fabricated flag. */
    CHECK(turbo_agent_speech_pump(input, 1u, &done) == TURBO_AGENT_SPEECH_OK);
    char transcript[64] = {0};
    if (mode == 1) {
      CHECK(turbo_agent_speech_get_state(input) ==
            TURBO_AGENT_SPEECH_NO_SPEECH);
      CHECK(host_take_final(voice, input, generation, transcript,
                            sizeof(transcript)) == TURBO_VOICE_EBUSY);
      CHECK(turbo_voice_interrupt(voice, generation) == TURBO_VOICE_OK);
      CHECK(turbo_voice_input_quiesced(voice, generation) == TURBO_VOICE_OK);
    } else {
      CHECK(host_take_final(voice, input, generation, transcript,
                            sizeof(transcript)) == TURBO_VOICE_OK);
      CHECK(strcmp(transcript, "Hello host") == 0);
      CHECK(host_take_final(voice, input, generation, transcript,
                            sizeof(transcript)) == TURBO_VOICE_EBUSY);
      CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_WAIT_APPROVAL);
      CHECK(turbo_voice_asr_event(voice, generation + 1u,
                                 TURBO_VOICE_ASR_CHECKED_FINAL) ==
            TURBO_VOICE_ESTALE);
      /* Simulate separate trusted Harness committed user-visible FINAL.
       * SpeechInput's transcript is NOT directly used as TTS text. */
      CHECK(turbo_voice_approved_final(voice, generation, "Approved answer",
             strlen("Approved answer"), "fake", "en") == TURBO_VOICE_OK);
      CHECK(tts.begins == 1);
      CHECK(turbo_voice_begin_input(voice, generation + 1u) ==
            TURBO_VOICE_EBUSY);
      size_t written = 0u;
      for (unsigned tick = 0u; tick < 12u &&
           turbo_voice_get_state(voice) != TURBO_VOICE_DRAIN_PENDING; ++tick)
        CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
      CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_DRAIN_PENDING);
      CHECK(sink.buffered == 960u && tts.polls == 2);
      CHECK(turbo_voice_drain(voice, 10u) == TURBO_VOICE_EBUSY);
      sink.buffered = 0u; /* simulated device callback consumption */
      CHECK(turbo_voice_drain(voice, 10u) == TURBO_VOICE_OK);
      CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
    }
  }
  if (mode != 0) CHECK(tts.begins == 0 && sink.writes == 0);
  CHECK(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  CHECK(asr.begins == 1 && asr.finishes == (mode == 2 ? 0 : 1));
  return 0;
}

int main(void) {
  CHECK(run_host_handoff(0) == 0); /* checked FINAL -> host approved -> PCM */
  CHECK(run_host_handoff(1) == 0); /* NO_SPEECH cannot start Agent/TTS */
  CHECK(run_host_handoff(2) == 0); /* corrupt capture cancels without FINAL */
  puts("PASS: real M1/M2 Capture callback join and one-shot FINAL to M4.2");
  puts("PASS: PARTIAL, NO_SPEECH, malformed PCM, stale/duplicate blocked");
  puts("PASS: Host-approved committed response remains distinct from ASR");
  return 0;
}
