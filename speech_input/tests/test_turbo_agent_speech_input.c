#include "turbo_agent_speech_input.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

typedef struct mock_s {
  int begin_calls, feed_calls, finish_calls, cancel_calls;
  int begin_error, feed_error, finish_error, invalid_utf8;
  int16_t first_sample;
  uint64_t last_timestamp;
  const char *final;
} mock_t;

static int mock_begin(void *ctx, uint64_t id) {
  mock_t *m = (mock_t *)ctx;
  m->begin_calls++;
  return id != 0 && !m->begin_error ? 0 : -1;
}

static int mock_accept(void *ctx, const int16_t *frame, size_t n,
                       uint64_t ts, turbo_agent_speech_transcript_t *out) {
  mock_t *m = (mock_t *)ctx;
  if (n != TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME) return -1;
  m->first_sample = frame[0];
  m->last_timestamp = ts;
  m->feed_calls++;
  if (m->feed_error) return -1;
  if (m->invalid_utf8) {
    static const char bad[] = "\xc0\xaf";
    *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_PARTIAL, bad, 2
    };
  } else {
    *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_PARTIAL, "draft", 5
    };
  }
  return 0;
}

static int mock_finish(void *ctx, turbo_agent_speech_transcript_t *out) {
  mock_t *m = (mock_t *)ctx;
  m->finish_calls++;
  if (m->finish_error) return -1;
  if (m->final) {
    *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, m->final, strlen(m->final)
    };
  } else {
    *out = (turbo_agent_speech_transcript_t){
      TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH, NULL, 0
    };
  }
  return 0;
}

static void mock_cancel(void *ctx) {
  ((mock_t *)ctx)->cancel_calls++;
}

static int create(mock_t *mock, uint32_t q, uint32_t total,
                  turbo_agent_speech_input_t **out) {
  turbo_agent_speech_config_t c;
  turbo_agent_speech_provider_v1_t p;
  turbo_agent_speech_config_init(&c);
  turbo_agent_speech_provider_v1_init(&p);
  c.queue_frames = q;
  c.max_utterance_frames = total;
  p.context = mock;
  p.begin = mock_begin;
  p.accept_frame = mock_accept;
  p.finish = mock_finish;
  p.cancel = mock_cancel;
  return turbo_agent_speech_create(&c, &p, out);
}

static int test_lifecycle_and_borrowed_copy(void) {
  mock_t m = {0};
  turbo_agent_speech_input_t *h = NULL;
  int16_t data[TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME] = {0};
  size_t count = 0, n = 0;
  uint64_t revision = 0;
  char text[256];
  turbo_agent_speech_transcript_kind_t kind;
  m.final = "hello";
  CHECK(create(&m, 2, 3, &h) == 0);
  CHECK(turbo_agent_speech_begin(h, 71) == 0);
  data[0] = 900;
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 320, 0) == 0);
  data[0] = -1; /* Capture borrowed buffer reused after callback returns. */
  CHECK(turbo_agent_speech_offer_frame(h, 70, data, 320, 20000) ==
        TURBO_AGENT_SPEECH_ESTALE);
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 319, 20000) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 320, 0) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 320, 20000) == 0);
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 320, 40000) ==
        TURBO_AGENT_SPEECH_ECAPACITY);
  CHECK(turbo_agent_speech_destroy(&h) == TURBO_AGENT_SPEECH_EBUSY);
  CHECK(turbo_agent_speech_pump(h, 1, &count) == 0 && count == 1);
  CHECK(m.first_sample == 900 && m.feed_calls == 1);
  CHECK(turbo_agent_speech_read_partial(h, text, sizeof(text), &n, &revision) == 0);
  CHECK(n == 5 && revision == 1 && strcmp(text, "draft") == 0);
  CHECK(turbo_agent_speech_take_final(h, text, sizeof(text), &n, &kind) ==
        TURBO_AGENT_SPEECH_ESTATE); /* PARTIAL cannot enter Agent. */
  CHECK(turbo_agent_speech_request_finish(h) == 0);
  CHECK(turbo_agent_speech_offer_frame(h, 71, data, 320, 60000) ==
        TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_pump(h, 1, &count) == 0 && count == 1);
  CHECK(turbo_agent_speech_get_state(h) == TURBO_AGENT_SPEECH_DRAINING);
  CHECK(turbo_agent_speech_pump(h, 1, &count) == 0 && count == 0);
  CHECK(turbo_agent_speech_get_state(h) == TURBO_AGENT_SPEECH_FINAL);
  CHECK(m.finish_calls == 1 && m.cancel_calls == 0);
  CHECK(turbo_agent_speech_take_final(h, text, 5, &n, &kind) ==
        TURBO_AGENT_SPEECH_EBUFFER);
  CHECK(n == 5 && kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL);
  CHECK(turbo_agent_speech_take_final(h, text, sizeof(text), &n, &kind) == 0);
  CHECK(n == 5 && strcmp(text, "hello") == 0);
  CHECK(turbo_agent_speech_take_final(h, text, sizeof(text), &n, &kind) ==
        TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_destroy(&h) == 0 && h == NULL);
  return 0;
}

static int test_silence_and_cancel(void) {
  mock_t m = {0};
  turbo_agent_speech_input_t *h = NULL;
  size_t n = 999, pumped = 42;
  char text[8] = "stale";
  turbo_agent_speech_transcript_kind_t kind;
  CHECK(create(&m, 2, 2, &h) == 0);
  CHECK(turbo_agent_speech_begin(h, 111) == 0);
  CHECK(turbo_agent_speech_request_finish(h) == 0);
  CHECK(turbo_agent_speech_pump(h, 2, &pumped) == 0 && pumped == 0);
  CHECK(turbo_agent_speech_get_state(h) == TURBO_AGENT_SPEECH_NO_SPEECH);
  CHECK(turbo_agent_speech_take_final(h, text, sizeof(text), &n, &kind) == 0);
  CHECK(n == 0 && text[0] == 0 &&
        kind == TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH);
  CHECK(turbo_agent_speech_begin(h, 111) == TURBO_AGENT_SPEECH_ESTALE);
  CHECK(turbo_agent_speech_begin(h, 112) == 0);
  CHECK(turbo_agent_speech_cancel(h) == 0);
  CHECK(turbo_agent_speech_cancel(h) == TURBO_AGENT_SPEECH_ESTATE);
  CHECK(m.cancel_calls == 1);
  CHECK(turbo_agent_speech_take_final(h, text, sizeof(text), &n, &kind) ==
        TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_destroy(&h) == 0);
  return 0;
}

static int test_provider_fail_and_utf8(void) {
  mock_t m = {0};
  turbo_agent_speech_input_t *h = NULL;
  size_t pumped = 0;
  int16_t pcm[320] = {0};
  CHECK(create(&m, 2, 2, &h) == 0);
  CHECK(turbo_agent_speech_begin(h, 1) == 0);
  m.invalid_utf8 = 1;
  CHECK(turbo_agent_speech_offer_frame(h, 1, pcm, 320, 0) == 0);
  CHECK(turbo_agent_speech_pump(h, 1, &pumped) == TURBO_AGENT_SPEECH_ETEXT);
  CHECK(m.cancel_calls == 1 &&
        turbo_agent_speech_get_state(h) == TURBO_AGENT_SPEECH_FAILED);
  CHECK(turbo_agent_speech_begin(h, 2) == 0);
  m.invalid_utf8 = 0;
  m.feed_error = 1;
  CHECK(turbo_agent_speech_offer_frame(h, 2, pcm, 320, 0) == 0);
  CHECK(turbo_agent_speech_pump(h, 1, &pumped) == TURBO_AGENT_SPEECH_EPROVIDER);
  CHECK(m.cancel_calls == 2);
  m.feed_error = 0;
  m.finish_error = 1;
  CHECK(turbo_agent_speech_begin(h, 3) == 0);
  CHECK(turbo_agent_speech_request_finish(h) == 0);
  CHECK(turbo_agent_speech_pump(h, 1, &pumped) == TURBO_AGENT_SPEECH_EPROVIDER);
  CHECK(m.cancel_calls == 3);
  CHECK(turbo_agent_speech_destroy(&h) == 0);
  return 0;
}

static int test_invalid_config_and_max_frames(void) {
  turbo_agent_speech_input_t *h = NULL;
  turbo_agent_speech_config_t c;
  turbo_agent_speech_provider_v1_t p;
  mock_t m = {0};
  int16_t data[320] = {0};
  size_t pumped;
  turbo_agent_speech_config_init(&c);
  turbo_agent_speech_provider_v1_init(&p);
  p.context = &m;
  p.begin = mock_begin;
  p.accept_frame = mock_accept;
  p.finish = mock_finish;
  p.cancel = mock_cancel;
  c.sample_rate_hz = 24000;
  CHECK(turbo_agent_speech_create(&c, &p, &h) == TURBO_AGENT_SPEECH_EINVAL &&
        h == NULL);
  c.sample_rate_hz = 16000;
  c.abi_version = 123;
  CHECK(turbo_agent_speech_create(&c, &p, &h) == TURBO_AGENT_SPEECH_EINVAL);
  c.abi_version = TURBO_AGENT_SPEECH_INPUT_ABI_VERSION;
  c.max_utterance_frames = 1;
  CHECK(turbo_agent_speech_create(&c, &p, &h) == 0);
  CHECK(turbo_agent_speech_begin(h, 50) == 0);
  CHECK(turbo_agent_speech_offer_frame(h, 50, data, 320, 3) == 0);
  CHECK(turbo_agent_speech_pump(h, 1, &pumped) == 0);
  CHECK(turbo_agent_speech_offer_frame(h, 50, data, 320, 4) ==
        TURBO_AGENT_SPEECH_ECAPACITY);
  CHECK(turbo_agent_speech_cancel(h) == 0);
  CHECK(turbo_agent_speech_destroy(&h) == 0);
  return 0;
}

int main(void) {
  CHECK(test_lifecycle_and_borrowed_copy() == 0);
  CHECK(test_silence_and_cancel() == 0);
  CHECK(test_provider_fail_and_utf8() == 0);
  CHECK(test_invalid_config_and_max_frames() == 0);
  puts("SpeechInput M1 C11 conformance PASS");
  return 0;
}
