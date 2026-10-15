/* Unix source/TSan gate: real producer thread never invokes ASR callbacks. */
#include "turbo_agent_speech_input.h"

#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: failed %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

enum { COUNT = 500 };

typedef struct test_s {
  turbo_agent_speech_input_t *handle;
  atomic_int producer_done;
  atomic_int producer_error;
  int accepted; /* owned exclusively by the consumer/control thread */
  uint64_t last_ts;
} test_t;

static int begin_cb(void *v, uint64_t id) {
  (void)v;
  return id == 1001u ? 0 : -1;
}

static int accept_cb(void *v, const int16_t *pcm, size_t n, uint64_t ts,
                     turbo_agent_speech_transcript_t *partial) {
  test_t *t = (test_t *)v;
  int want = t->accepted + 1;
  if (n != 320u || want > COUNT || pcm[0] != want ||
      (t->accepted && ts <= t->last_ts)) return -1;
  t->accepted = want;
  t->last_ts = ts;
  *partial = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_NONE, NULL, 0
  };
  return 0;
}

static int finish_cb(void *v, turbo_agent_speech_transcript_t *result) {
  test_t *t = (test_t *)v;
  if (t->accepted != COUNT) return -1;
  *result = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, "accepted", 8
  };
  return 0;
}

static void cancel_cb(void *v) {
  atomic_store(&((test_t *)v)->producer_error, 1);
}

static void *producer(void *v) {
  test_t *t = (test_t *)v;
  int16_t pcm[320] = {0};
  for (int i = 1; i <= COUNT; i++) {
    int rc;
    pcm[0] = (int16_t)i;
    do {
      rc = turbo_agent_speech_offer_frame(
        t->handle, 1001u, pcm, 320u, (uint64_t)i * 20000u);
      if (rc == TURBO_AGENT_SPEECH_EBUSY ||
          rc == TURBO_AGENT_SPEECH_ECAPACITY) sched_yield();
    } while (rc == TURBO_AGENT_SPEECH_EBUSY ||
             rc == TURBO_AGENT_SPEECH_ECAPACITY);
    if (rc != TURBO_AGENT_SPEECH_OK) {
      atomic_store(&t->producer_error, rc);
      break;
    }
  }
  atomic_store(&t->producer_done, 1);
  return NULL;
}

int main(void) {
  test_t t = {0};
  pthread_t worker;
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t provider;
  size_t pumped;
  char text[16];
  size_t bytes;
  turbo_agent_speech_transcript_kind_t kind;
  atomic_init(&t.producer_done, 0);
  atomic_init(&t.producer_error, 0);
  turbo_agent_speech_config_init(&cfg);
  cfg.queue_frames = 8;
  cfg.max_utterance_frames = COUNT;
  turbo_agent_speech_provider_v1_init(&provider);
  provider.context = &t;
  provider.begin = begin_cb;
  provider.accept_frame = accept_cb;
  provider.finish = finish_cb;
  provider.cancel = cancel_cb;
  CHECK(turbo_agent_speech_create(&cfg, &provider, &t.handle) == 0);
  CHECK(turbo_agent_speech_begin(t.handle, 1001u) == 0);
  CHECK(pthread_create(&worker, NULL, producer, &t) == 0);
  for (int retry = 0; retry < 1000000; retry++) {
    CHECK(turbo_agent_speech_pump(t.handle, 3u, &pumped) == 0);
    if (atomic_load(&t.producer_done) && t.accepted == COUNT) break;
    sched_yield();
  }
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(atomic_load(&t.producer_error) == 0);
  CHECK(t.accepted == COUNT);
  CHECK(turbo_agent_speech_request_finish(t.handle) == 0);
  CHECK(turbo_agent_speech_pump(t.handle, 1u, &pumped) == 0);
  CHECK(turbo_agent_speech_get_state(t.handle) == TURBO_AGENT_SPEECH_FINAL);
  CHECK(turbo_agent_speech_take_final(t.handle, text, sizeof(text),
                                      &bytes, &kind) == 0);
  CHECK(bytes == 8 && strcmp(text, "accepted") == 0);
  CHECK(turbo_agent_speech_destroy(&t.handle) == 0);
  puts("SpeechInput concurrent producer/consumer PASS");
  return 0;
}
