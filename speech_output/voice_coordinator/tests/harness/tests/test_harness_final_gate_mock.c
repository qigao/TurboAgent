#include "turbo_agent_voice_harness_final_gate.h"
#include <turbo_agent_state.h>
#include <turbo_runtime_json.h>
#include <json_parser.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ONLY this conformance executable supplies these Harness ABI test doubles.
 * They match the unchanged exact pinned real public Harness/App headers.
 * This test is NOT itself evidence of running a real Harness/Agent execution. */
struct turbo_agent_app_s {
  const char *last_run_id;
};
struct turbo_agent_harness_s {
  struct turbo_agent_app_s app;
};
struct turbo_agent_harness_execution_s {
  const char *execution_id;
  turbo_agent_execution_status_t status;
  int code;
  int get_status_error;
  int get_code_error;
  int result_error;
  int already_taken;
  unsigned take_calls;
  const char *summary_status;
  const char *summary_run_id;
  const char *final_text;
  struct turbo_agent_harness_s *mutate_harness;
  const char *new_app_run_id;
};
static unsigned final_reads = 0u;
static unsigned result_destroys = 0u;

const char *turbo_agent_harness_execution_id(
    const turbo_agent_harness_execution_t *e) {
  return e ? e->execution_id : NULL;
}
int turbo_agent_harness_execution_get_status(
    const turbo_agent_harness_execution_t *e,
    turbo_agent_execution_status_t *out) {
  if (!e || !out || e->get_status_error) return SALTS_EINVAL;
  *out = e->status;
  return SALTS_OK;
}
int turbo_agent_harness_execution_result_code(
    const turbo_agent_harness_execution_t *e, int *out) {
  if (!e || !out || e->get_code_error) return SALTS_EINVAL;
  *out = e->code;
  return SALTS_OK;
}
int turbo_agent_harness_execution_take_result(
    turbo_agent_harness_execution_t *e,
    json_value_t **out_summary, json_value_t **out_state) {
  if (!e || !out_summary || !out_state || e->result_error)
    return SALTS_EINVAL;
  *out_summary = NULL;
  *out_state = NULL;
  e->take_calls++;
  if (e->already_taken) return SALTS_EALREADY;
  e->already_taken = 1;
  if (e->mutate_harness) e->mutate_harness->app.last_run_id = e->new_app_run_id;
  json_value_t *summary = json_create_object();
  json_value_t *state = json_create_object();
  if (!summary || !state) {
    json_free(summary);
    json_free(state);
    return SALTS_ENOMEM;
  }
  if (e->summary_status)
    json_object_set_string(summary, "status", e->summary_status);
  if (e->summary_run_id)
    json_object_set_string(summary, "run_id", e->summary_run_id);
  if (e->final_text)
    json_object_set_string(state, "final", e->final_text);
  *out_summary = summary;
  *out_state = state;
  return SALTS_OK;
}
turbo_agent_app_t *turbo_agent_harness_app(
    const turbo_agent_harness_t *h) {
  return h ? (turbo_agent_app_t *)&h->app : NULL;
}
const char *turbo_agent_app_last_run_id(const turbo_agent_app_t *app) {
  return app ? ((const struct turbo_agent_app_s *)app)->last_run_id : NULL;
}
const char *turbo_agent_state_final_answer_text(const json_value_t *state) {
  return state ? json_get_string(state, "final") : NULL;
}
char *turbo_agent_app_result_text(const json_value_t *state) {
  final_reads++;
  const char *s = state ? json_get_string(state, "final") : NULL;
  if (!s || !s[0]) return NULL;
  size_t n = strlen(s);
  char *copy = (char *)malloc(n + 1u);
  if (copy) memcpy(copy, s, n + 1u);
  return copy;
}
void turbo_runtime_json_destroy(json_value_t *value) {
  if (value) result_destroys++;
  json_free(value);
}

/* TEST ONLY: no native device, miniaudio, network, second PCM queue or
 * synthesis model. Playback signature matches pinned installed header. */
struct salts_playback {
  uint8_t ring[4800];
  size_t bytes;
  unsigned writes;
  unsigned drains;
  unsigned clears;
};
int salts_playback_write(salts_playback_t *p, const void *pcm,
                         size_t bytes, size_t *written) {
  if (!written) return SALTS_PLAYBACK_ERR_FORMAT;
  *written = 0u;
  if (!p || !pcm || bytes % 2u) return SALTS_PLAYBACK_ERR_FORMAT;
  size_t capacity = sizeof(p->ring) - p->bytes;
  size_t n = bytes < capacity ? bytes : capacity;
  n &= ~(size_t)1u;
  memcpy(p->ring + p->bytes, pcm, n);
  p->bytes += n;
  p->writes++;
  *written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  memset(p->ring, 0, sizeof(p->ring));
  p->bytes = 0u;
  p->clears++;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || !timeout_ms) return SALTS_PLAYBACK_ERR_FORMAT;
  p->drains++;
  return p->bytes == 0u ? SALTS_PLAYBACK_OK : SALTS_PLAYBACK_ERR_TIMEOUT;
}

typedef struct fake_tts {
  uint64_t generation;
  unsigned began;
  unsigned polled;
  unsigned canceled;
  int done;
} fake_tts_t;
static int tts_begin(void *ptr, uint64_t generation,
                     const char *approved, size_t count,
                     const char *voice, const char *language,
                     const turbo_tts_format_t *format) {
  fake_tts_t *t = (fake_tts_t *)ptr;
  if (!t || generation != 42u || !approved || !count ||
      !voice || !language || !format || format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_samples != 480u) return -1;
  t->generation = generation;
  t->began++;
  t->done = 0;
  return 0;
}
static int tts_poll(void *ptr, uint64_t generation, int16_t *pcm,
                    size_t capacity, size_t *count, turbo_tts_poll_t *kind) {
  fake_tts_t *t = (fake_tts_t *)ptr;
  if (!t || t->generation != generation || !pcm || capacity != 480u)
    return -1;
  t->polled++;
  if (t->canceled) {
    *count = 0u;
    *kind = TURBO_TTS_POLL_CANCELED;
    return 0;
  }
  if (t->done) {
    *count = 0u;
    *kind = TURBO_TTS_TERMINAL;
  } else {
    for (size_t i = 0u; i < 480u; ++i) pcm[i] = (int16_t)(100u + i);
    *count = 480u;
    *kind = TURBO_TTS_PCM;
    t->done = 1;
  }
  return 0;
}
static void tts_cancel(void *ptr, uint64_t generation) {
  fake_tts_t *t = (fake_tts_t *)ptr;
  if (t && t->generation == generation) t->canceled++;
}

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL HARNESS FINAL line %d: %s\n", __LINE__, #x); \
  return 1; } } while (0)

static int run_case(int variant) {
  fake_tts_t provider = {0};
  struct salts_playback sink = {0};
  turbo_tts_provider_t callbacks;
  turbo_tts_config_t config;
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  turbo_tts_provider_init(&callbacks);
  turbo_tts_config_init(&config);
  callbacks.context = &provider;
  callbacks.begin = tts_begin;
  callbacks.poll_pcm = tts_poll;
  callbacks.request_cancel = tts_cancel;
  CHECK(turbo_tts_create(&config, &callbacks, &source) == TURBO_TTS_OK);
  CHECK(turbo_voice_create(source, &sink, &voice) == TURBO_VOICE_OK);
  CHECK(turbo_voice_begin_input(voice, 42u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_asr_event(voice, 42u,
          TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_OK);

  struct turbo_agent_harness_s harness = {.app = {"run-42"}};
  struct turbo_agent_harness_execution_s exec = {
    .execution_id = "exec-42",
    .status = TURBO_AGENT_EXECUTION_COMPLETED,
    .code = SALTS_OK,
    .summary_status = "completed",
    .summary_run_id = "run-42",
    .final_text = "Approved visible answer"
  };
  int approved = 1;
  const char *expected_run_id = "run-42";
  unsigned previous_reads = final_reads;
  unsigned previous_destroys = result_destroys;
  char oversized[TURBO_TTS_MAX_TEXT_BYTES + 2u];

  if (variant == 0) { /* raw FINAL is not enough: Capture must join */
    CHECK(turbo_voice_admit_harness_completed_final(voice, 42u,
        &harness, &exec, expected_run_id, approved, "fake", "en")
        == TURBO_VOICE_EBUSY);
    CHECK(exec.take_calls == 0u && provider.began == 0u);
  }
  CHECK(turbo_voice_input_quiesced(voice, 42u) == TURBO_VOICE_OK);
  switch (variant) {
    case 1: approved = 0; break;
    case 2: exec.status = TURBO_AGENT_EXECUTION_RUNNING; break;
    case 3: exec.status = TURBO_AGENT_EXECUTION_CANCELLED; break;
    case 4: exec.status = TURBO_AGENT_EXECUTION_INTERRUPTED; break;
    case 5: exec.status = TURBO_AGENT_EXECUTION_TIMED_OUT; break;
    case 6: exec.status = TURBO_AGENT_EXECUTION_FAILED; break;
    case 7: exec.code = SALTS_EIO; break;
    case 8: exec.summary_status = "interrupted"; break;
    case 9: exec.summary_run_id = "run-previous"; break;
    case 10: exec.final_text = NULL; break;
    case 11: expected_run_id = "run-other"; break;
    case 12: harness.app.last_run_id = "run-new"; break;
    case 13: exec.final_text = "<tool call>"; break;
    case 14: exec.final_text = ""; break;
    case 15: exec.execution_id = ""; break;
    case 16: exec.get_status_error = 1; break;
    case 17: exec.get_code_error = 1; break;
    case 18: exec.result_error = 1; break;
    case 19: exec.already_taken = 1; break;
    case 21: exec.mutate_harness = &harness;
             exec.new_app_run_id = "run-new"; break;
    case 22: memset(oversized, 'x', sizeof(oversized) - 1u);
             oversized[sizeof(oversized) - 1u] = '\0';
             exec.final_text = oversized; break;
    default: break;
  }

  int rc = turbo_voice_admit_harness_completed_final(voice, 42u,
      &harness, &exec, expected_run_id, approved, "fake", "en");
  if (variant == 0 || variant == 20) {
    CHECK(rc == TURBO_VOICE_OK && provider.began == 1u);
    CHECK(exec.take_calls == 1u && final_reads == previous_reads + 1u);
    CHECK(result_destroys == previous_destroys + 2u);
    CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_SPEAKING);
    CHECK(turbo_voice_admit_harness_completed_final(voice, 42u,
        &harness, &exec, expected_run_id, approved, "fake", "en")
        == TURBO_VOICE_EBUSY && exec.take_calls == 1u);
    size_t written = 0u;
    for (unsigned i = 0u; i < 4u &&
         turbo_voice_get_state(voice) != TURBO_VOICE_DRAIN_PENDING; ++i)
      CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_DRAIN_PENDING);
    CHECK(sink.bytes == 960u);
    CHECK(turbo_voice_drain(voice, 10u) == TURBO_VOICE_EBUSY);
    sink.bytes = 0u;
    CHECK(turbo_voice_drain(voice, 10u) == TURBO_VOICE_OK);
    CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
  } else {
    CHECK(rc != TURBO_VOICE_OK && provider.began == 0u &&
          sink.writes == 0u);
    CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_WAIT_APPROVAL);
    if (variant == 10 || variant == 14 || variant == 21 || variant == 22)
      CHECK(final_reads == previous_reads);
    CHECK(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
    CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_INTERRUPTED);
  }
  CHECK(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  return 0;
}

int main(void) {
  CHECK(run_case(0) == 0);
  for (int i = 1; i <= 22; ++i) CHECK(run_case(i) == 0);
  puts("PASS: original public Harness status/result/take/app interfaces");
  puts("PASS: terminal, replay, mid-take run switch and oversized-final negative gates");
  puts("PASS: only Host-approved committed final may start bounded TTS/drain");
  return 0;
}
