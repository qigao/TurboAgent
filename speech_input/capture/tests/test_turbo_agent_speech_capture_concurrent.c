/* Callback admission race is tested with a real thread but no microphone.
 * Salts::Capture itself must be stopped/drained before the bridge is freed. */
#include "turbo_agent_speech_capture.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { \
  fprintf(stderr, "%s:%d failed %s\n", __FILE__, __LINE__, #cond); \
  return 1; \
} } while (0)

typedef struct fixture_s {
  turbo_agent_speech_input_t *input;
  turbo_agent_speech_capture_bridge_t *bridge;
  atomic_int go;
  atomic_int returned;
  atomic_int unexpected;
  atomic_int accepted;
  int processed; /* control thread only */
  int canceled;  /* control thread only */
} fixture_t;

static int provider_begin(void *ctx, uint64_t utterance) {
  (void)ctx;
  return utterance == 100u ? 0 : -1;
}

static int provider_accept(void *ctx, const int16_t *samples, size_t n,
                           uint64_t ts, turbo_agent_speech_transcript_t *partial) {
  fixture_t *fixture = (fixture_t *)ctx;
  if (n != 320u || ts == 0u || samples[0] < 0) return -1;
  fixture->processed++;
  *partial = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_NONE, NULL, 0
  };
  return 0;
}

static int provider_finish(void *ctx, turbo_agent_speech_transcript_t *final) {
  (void)ctx;
  *final = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, "done", 4
  };
  return 0;
}

static void provider_cancel(void *ctx) {
  ((fixture_t *)ctx)->canceled++;
}

static void *capture_callback_thread(void *arg) {
  fixture_t *fixture = (fixture_t *)arg;
  uint8_t pcm[640] = {0};
  while (!atomic_load_explicit(&fixture->go, memory_order_acquire))
    sched_yield();
  for (int i = 1; i < 400; ++i) {
    const int rc = turbo_agent_speech_capture_bridge_offer(
        fixture->bridge, pcm, sizeof(pcm), (uint64_t)i * 20000u);
    if (rc == TURBO_AGENT_SPEECH_OK) {
      atomic_fetch_add_explicit(&fixture->accepted, 1, memory_order_relaxed);
    } else if (rc == TURBO_AGENT_SPEECH_ECAPACITY ||
               rc == TURBO_AGENT_SPEECH_EBUSY ||
               rc == TURBO_AGENT_SPEECH_ESTATE) {
      break; /* fail-closed or control-plane stop won */
    } else {
      atomic_store_explicit(&fixture->unexpected, rc, memory_order_relaxed);
      break;
    }
    sched_yield();
  }
  atomic_store_explicit(&fixture->returned, 1, memory_order_release);
  return NULL;
}

static int stress_once(void) {
  fixture_t fixture = {0};
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t p;
  pthread_t worker;
  turbo_agent_speech_transcript_kind_t kind;
  char text[16];
  size_t bytes, processed = 0;
  int first_error;
  atomic_init(&fixture.go, 0);
  atomic_init(&fixture.returned, 0);
  atomic_init(&fixture.unexpected, 0);
  atomic_init(&fixture.accepted, 0);
  turbo_agent_speech_config_init(&cfg);
  cfg.queue_frames = 32;
  cfg.max_utterance_frames = 400;
  turbo_agent_speech_provider_v1_init(&p);
  p.context = &fixture;
  p.begin = provider_begin;
  p.accept_frame = provider_accept;
  p.finish = provider_finish;
  p.cancel = provider_cancel;
  CHECK(turbo_agent_speech_create(&cfg, &p, &fixture.input) == 0);
  CHECK(turbo_agent_speech_capture_bridge_create(
            fixture.input, 100u, &fixture.bridge) == 0);
  CHECK(turbo_agent_speech_capture_bridge_begin(fixture.bridge) == 0);
  CHECK(pthread_create(&worker, NULL, capture_callback_thread, &fixture) == 0);
  atomic_store_explicit(&fixture.go, 1, memory_order_release);
  /* Control thread pumps while the producer tries short, nonblocking offers.
   * Stop may win before the callback, during a copy or afterward. */
  for (int i = 0; i < 80; i++) {
    CHECK(turbo_agent_speech_pump(fixture.input, 4u, &processed) == 0);
    sched_yield();
  }
  CHECK(turbo_agent_speech_capture_bridge_stop_admission(fixture.bridge) == 0);
  CHECK(pthread_join(worker, NULL) == 0);
  CHECK(atomic_load(&fixture.returned) == 1);
  CHECK(atomic_load(&fixture.unexpected) == 0);
  first_error = turbo_agent_speech_capture_bridge_first_error(fixture.bridge);
  if (first_error == TURBO_AGENT_SPEECH_OK) {
    CHECK(turbo_agent_speech_capture_bridge_finish(fixture.bridge) == 0);
    for (int i = 0; i < 256 &&
                    turbo_agent_speech_get_state(fixture.input) ==
                      TURBO_AGENT_SPEECH_DRAINING; i++)
      CHECK(turbo_agent_speech_pump(fixture.input, 8u, &processed) == 0);
    CHECK(turbo_agent_speech_get_state(fixture.input) ==
          TURBO_AGENT_SPEECH_FINAL);
    CHECK(turbo_agent_speech_take_final(fixture.input, text, sizeof(text),
                                         &bytes, &kind) == 0);
    CHECK(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
          bytes == 4 && strcmp(text, "done") == 0);
    CHECK(fixture.canceled == 0);
  } else {
    CHECK(first_error == TURBO_AGENT_SPEECH_ECAPACITY ||
          first_error == TURBO_AGENT_SPEECH_EBUSY);
    CHECK(turbo_agent_speech_capture_bridge_finish(fixture.bridge) ==
          first_error);
    CHECK(turbo_agent_speech_get_state(fixture.input) ==
          TURBO_AGENT_SPEECH_CANCELED);
    CHECK(fixture.canceled == 1);
  }
  CHECK(turbo_agent_speech_capture_bridge_destroy(&fixture.bridge) == 0);
  CHECK(turbo_agent_speech_destroy(&fixture.input) == 0);
  return 0;
}

int main(void) {
  for (int i = 0; i < 100; i++) CHECK(stress_once() == 0);
  puts("Capture stop/interleaving 100 x callback producer PASS");
  return 0;
}
