#include "turbo_agent_speech_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d REQUIRE(%s) failed\n", __FILE__, __LINE__, #x); \
  return 1; \
} } while (0)

typedef struct capture_mock_s {
  int starts, frames, finishes, cancels, first_sample, second_sample;
  const char *result;
} capture_mock_t;

static int mock_begin(void *ctx, uint64_t id) {
  capture_mock_t *m = (capture_mock_t *)ctx;
  m->starts++;
  return id != 0u ? 0 : -1;
}

static int mock_accept(void *ctx, const int16_t *pcm, size_t n,
                       uint64_t timestamp_us,
                       turbo_agent_speech_transcript_t *out) {
  capture_mock_t *m = (capture_mock_t *)ctx;
  if (n != TURBO_AGENT_SPEECH_SAMPLES_PER_FRAME ||
      timestamp_us == 0u) return -1;
  m->first_sample = pcm[0];
  m->second_sample = pcm[1];
  m->frames++;
  *out = (turbo_agent_speech_transcript_t) {
    TURBO_AGENT_SPEECH_PARTIAL, "incomplete", 10
  };
  return 0;
}

static int mock_finish(void *ctx, turbo_agent_speech_transcript_t *out) {
  capture_mock_t *m = (capture_mock_t *)ctx;
  m->finishes++;
  *out = (turbo_agent_speech_transcript_t) {
    TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL, m->result,
    m->result ? strlen(m->result) : 0
  };
  return 0;
}

static void mock_cancel(void *ctx) {
  ((capture_mock_t *)ctx)->cancels++;
}

static int create_test(capture_mock_t *mock,
                       uint32_t queued_frames,
                       uint64_t utterance,
                       turbo_agent_speech_input_t **out_input,
                       turbo_agent_speech_capture_bridge_t **out_bridge) {
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t p;
  int rc;
  turbo_agent_speech_config_init(&cfg);
  turbo_agent_speech_provider_v1_init(&p);
  cfg.queue_frames = queued_frames;
  p.context = mock;
  p.begin = mock_begin;
  p.accept_frame = mock_accept;
  p.finish = mock_finish;
  p.cancel = mock_cancel;
  rc = turbo_agent_speech_create(&cfg, &p, out_input);
  if (rc != TURBO_AGENT_SPEECH_OK) return rc;
  rc = turbo_agent_speech_capture_bridge_create(
      *out_input, utterance, out_bridge);
  if (rc != TURBO_AGENT_SPEECH_OK)
    (void)turbo_agent_speech_destroy(out_input);
  return rc;
}

static int test_borrowed_frame_and_exact_no_fallback(void) {
  capture_mock_t m = {0};
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_capture_bridge_t *bridge = NULL;
  uint8_t borrowed[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES] = {0};
  size_t done = 0, len = 0;
  uint64_t revision = 0;
  turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
  char text[32];
  m.result = "safe";
  REQUIRE(create_test(&m, 2, 44, &input, &bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_begin(bridge) == 0);
  /* 0x1234 / 0xFFFF explicitly test endianness and signed conversion. */
  borrowed[0] = 0x34u;
  borrowed[1] = 0x12u;
  borrowed[2] = 0xffu;
  borrowed[3] = 0xffu;
  REQUIRE(turbo_agent_speech_capture_bridge_offer(bridge, borrowed,
          sizeof(borrowed), 1000u) == 0);
  memset(borrowed, 0xee, sizeof(borrowed));
  REQUIRE(turbo_agent_speech_pump(input, 2u, &done) == 0 && done == 1u);
  REQUIRE(m.frames == 1 && m.first_sample == 0x1234 &&
          m.second_sample == -1);
  REQUIRE(turbo_agent_speech_read_partial(input, text, sizeof(text),
                                          &len, &revision) == 0);
  REQUIRE(len == 10u && revision == 1u &&
          strcmp(text, "incomplete") == 0);
  REQUIRE(turbo_agent_speech_take_final(input, text, sizeof(text),
                                         &len, &kind) == TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_capture_bridge_destroy(&bridge) ==
          TURBO_AGENT_SPEECH_EBUSY);
  REQUIRE(turbo_agent_speech_capture_bridge_stop_admission(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_offer(bridge, borrowed,
          sizeof(borrowed), 2000u) == TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_capture_bridge_first_error(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_finish(bridge) == 0);
  REQUIRE(turbo_agent_speech_pump(input, 1u, &done) == 0 && done == 0u);
  REQUIRE(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
  REQUIRE(turbo_agent_speech_take_final(input, text, sizeof(text),
                                         &len, &kind) == 0);
  REQUIRE(len == 4u && strcmp(text, "safe") == 0 &&
          kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL);
  REQUIRE(m.starts == 1 && m.finishes == 1 && m.cancels == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_destroy(&bridge) == 0);
  REQUIRE(turbo_agent_speech_destroy(&input) == 0);
  return 0;
}

static int test_invalid_pcm_fails_closed(void) {
  capture_mock_t m = {0};
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_capture_bridge_t *bridge = NULL;
  uint8_t frame[TURBO_AGENT_SPEECH_CAPTURE_PCM_BYTES] = {0};
  size_t pumped = 0, bytes = 0;
  char out[16];
  turbo_agent_speech_transcript_kind_t kind;
  m.result = "never accepted";
  REQUIRE(create_test(&m, 1, 51, &input, &bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_begin(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_offer(bridge, frame,
          sizeof(frame) - 1u, 1000u) == TURBO_AGENT_SPEECH_EINVAL);
  REQUIRE(turbo_agent_speech_capture_bridge_first_error(bridge) ==
          TURBO_AGENT_SPEECH_EINVAL);
  REQUIRE(turbo_agent_speech_capture_bridge_offer(bridge, frame,
          sizeof(frame), 2000u) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_offer(bridge, frame,
          sizeof(frame), 3000u) == TURBO_AGENT_SPEECH_ECAPACITY);
  /* The first error wins; overflow cannot silently turn into a text FINAL. */
  REQUIRE(turbo_agent_speech_capture_bridge_first_error(bridge) ==
          TURBO_AGENT_SPEECH_EINVAL);
  REQUIRE(turbo_agent_speech_capture_bridge_stop_admission(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_finish(bridge) ==
          TURBO_AGENT_SPEECH_EINVAL);
  REQUIRE(m.cancels == 1 && m.finishes == 0);
  REQUIRE(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_CANCELED);
  REQUIRE(turbo_agent_speech_pump(input, 1, &pumped) ==
          TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_take_final(input, out, sizeof(out), &bytes,
                                         &kind) == TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_capture_bridge_destroy(&bridge) == 0);
  REQUIRE(turbo_agent_speech_destroy(&input) == 0);
  return 0;
}

static int test_explicit_cancel_before_provider(void) {
  capture_mock_t m = {0};
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_capture_bridge_t *bridge = NULL;
  REQUIRE(create_test(&m, 1, 99, &input, &bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_begin(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_cancel(bridge) ==
          TURBO_AGENT_SPEECH_EBUSY);
  REQUIRE(turbo_agent_speech_capture_bridge_stop_admission(bridge) == 0);
  REQUIRE(turbo_agent_speech_capture_bridge_cancel(bridge) == 0);
  REQUIRE(m.cancels == 1);
  REQUIRE(turbo_agent_speech_capture_bridge_cancel(bridge) ==
          TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_capture_bridge_destroy(&bridge) == 0);
  REQUIRE(turbo_agent_speech_destroy(&input) == 0);
  return 0;
}

int main(void) {
  REQUIRE(test_borrowed_frame_and_exact_no_fallback() == 0);
  REQUIRE(test_invalid_pcm_fails_closed() == 0);
  REQUIRE(test_explicit_cancel_before_provider() == 0);
  puts("Speech Capture M2 bridge mock PCM conformance PASS");
  return 0;
}
