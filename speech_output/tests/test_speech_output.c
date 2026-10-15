#include "turbo_agent_speech_output.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { \
  if (!(c)) { fprintf(stderr, "FAIL C11 TTS line %d\n", __LINE__); return 1; } \
} while (0)

enum fake_mode {
  FAKE_NORMAL = 0,
  FAKE_CANCEL_LATE_PCM,
  FAKE_CANCEL_SLOW,
  FAKE_BEGIN_FAIL,
  FAKE_BAD_COUNT,
  FAKE_BAD_KIND,
  FAKE_BAD_ZERO_PCM,
  FAKE_BAD_TERMINAL_PCM,
  FAKE_POLL_FAIL,
  FAKE_UNEXPECTED_ABORT,
  FAKE_ALWAYS_PCM,
  FAKE_WOULD_BLOCK
};

typedef struct fake_provider_s {
  turbo_tts_output_t *output;
  enum fake_mode mode;
  uint64_t generation;
  const char *borrowed_text;
  const char *borrowed_voice;
  const char *borrowed_language;
  unsigned begin_calls, poll_calls, cancel_calls;
  unsigned step, cancel_poll_calls;
  int cancel_requested;
  int allow_settle;
  int reenter_rc;
  turbo_tts_format_t captured_format;
} fake_provider_t;

static int fake_begin(void *context, uint64_t generation,
                      const char *text, size_t text_bytes,
                      const char *voice, const char *language,
                      const turbo_tts_format_t *format) {
  fake_provider_t *fake = (fake_provider_t *)context;
  if (!generation || !text || !text_bytes || !voice || !language ||
      !format) return -1;
  fake->begin_calls++;
  fake->generation = generation;
  fake->borrowed_text = text;
  fake->borrowed_voice = voice;
  fake->borrowed_language = language;
  fake->captured_format = *format;
  fake->step = 0u;
  fake->cancel_poll_calls = 0u;
  fake->cancel_requested = 0;
  /* Even inside begin, reentering control operations must fail before
   * any another owner/Provider callback is touched. */
  if (fake->output)
    fake->reenter_rc = turbo_tts_cancel(fake->output);
  return fake->mode == FAKE_BEGIN_FAIL ? -1 : 0;
}

static int fake_poll(void *context, uint64_t generation, int16_t *pcm,
                     size_t capacity, size_t *out_samples,
                     turbo_tts_poll_t *out_kind) {
  fake_provider_t *fake = (fake_provider_t *)context;
  if (generation != fake->generation ||
      capacity != TURBO_TTS_SAMPLES_PER_FRAME ||
      !pcm || !out_samples || !out_kind) return -1;
  fake->poll_calls++;

  if (fake->cancel_requested) {
    if (fake->mode == FAKE_CANCEL_LATE_PCM &&
        fake->cancel_poll_calls++ == 0u) {
      for (size_t i = 0u; i < capacity; i++) pcm[i] = 2345;
      *out_samples = capacity;
      *out_kind = TURBO_TTS_PCM; /* MUST be discarded by SpeechOutput */
    } else if (fake->mode == FAKE_CANCEL_SLOW && !fake->allow_settle) {
      *out_samples = 0u;
      *out_kind = TURBO_TTS_WOULD_BLOCK;
    } else {
      *out_samples = 0u;
      *out_kind = TURBO_TTS_POLL_CANCELED;
    }
    return 0;
  }

  if (fake->mode == FAKE_BAD_COUNT) {
    *out_samples = capacity + 1u;  /* claim, no out-of-bounds write */
    *out_kind = TURBO_TTS_PCM;
    return 0;
  }
  if (fake->mode == FAKE_BAD_KIND) {
    *out_samples = 0u;
    *out_kind = (turbo_tts_poll_t)88;
    return 0;
  }
  if (fake->mode == FAKE_BAD_ZERO_PCM) {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_PCM;
    return 0;
  }
  if (fake->mode == FAKE_BAD_TERMINAL_PCM) {
    pcm[0] = 444;
    *out_samples = 1u;
    *out_kind = TURBO_TTS_TERMINAL;
    return 0;
  }
  if (fake->mode == FAKE_POLL_FAIL) return -1;
  if (fake->mode == FAKE_UNEXPECTED_ABORT) {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_POLL_CANCELED;
    return 0;
  }
  if (fake->mode == FAKE_WOULD_BLOCK && fake->step == 0u) {
    fake->step++;
    pcm[0] = 1234; /* discarded since out_samples=0 */
    *out_samples = 0u;
    *out_kind = TURBO_TTS_WOULD_BLOCK;
    return 0;
  }
  if (fake->mode == FAKE_ALWAYS_PCM) {
    for (size_t i = 0u; i < capacity; i++) pcm[i] = 1000;
    *out_samples = capacity;
    *out_kind = TURBO_TTS_PCM;
    return 0;
  }
  if (fake->step == 0u || (fake->mode == FAKE_WOULD_BLOCK &&
      fake->step == 1u)) {
    for (size_t i = 0u; i < capacity; i++)
      pcm[i] = (int16_t)((i & 1u) ? 1000 : -1000);
    *out_samples = capacity;
    *out_kind = TURBO_TTS_PCM;
  } else if (fake->step == 1u || (fake->mode == FAKE_WOULD_BLOCK &&
             fake->step == 2u)) {
    for (size_t i = 0u; i < 80u; i++) pcm[i] = (int16_t)i;
    *out_samples = 80u;
    *out_kind = TURBO_TTS_PCM;
  } else {
    *out_samples = 0u;
    *out_kind = TURBO_TTS_TERMINAL;
  }
  fake->step++;
  return 0;
}

static void fake_cancel(void *context, uint64_t generation) {
  fake_provider_t *fake = (fake_provider_t *)context;
  if (generation == fake->generation) {
    fake->cancel_calls++;
    fake->cancel_requested = 1;
    if (fake->output)
      fake->reenter_rc = turbo_tts_cancel(fake->output);
  }
}

static int make_output(fake_provider_t *fake, turbo_tts_output_t **out,
                       uint32_t max_frames, uint32_t max_text_bytes) {
  turbo_tts_provider_t provider;
  turbo_tts_config_t config;
  turbo_tts_provider_init(&provider);
  turbo_tts_config_init(&config);
  provider.context = fake;
  provider.begin = fake_begin;
  provider.poll_pcm = fake_poll;
  provider.request_cancel = fake_cancel;
  config.max_frames = max_frames;
  config.max_text_bytes = max_text_bytes;
  int rc = turbo_tts_create(&config, &provider, out);
  if (rc == TURBO_TTS_OK) fake->output = *out;
  return rc;
}

static int test_pcm_and_terminal(void) {
  fake_provider_t f = {0};
  turbo_tts_output_t *o = NULL;
  turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
  int16_t pcm[480] = {0};
  size_t n = 55u;
  char text[] = "Hello";
  CHECK(make_output(&f, &o, 1500u, 4096u) == TURBO_TTS_OK);
  CHECK(turbo_tts_begin(o, 1u, text, 5u, "test_voice.1", "en") == TURBO_TTS_OK);
  CHECK(f.reenter_rc == TURBO_TTS_EBUSY);
  CHECK(f.borrowed_text != text);
  CHECK(f.borrowed_voice != NULL && !strcmp(f.borrowed_voice, "test_voice.1"));
  CHECK(!strcmp(f.borrowed_language, "en"));
  CHECK(f.captured_format.sample_rate_hz == 24000u &&
        f.captured_format.channels == 1u &&
        f.captured_format.bits_per_sample == 16u &&
        f.captured_format.frame_samples == 480u);
  text[0] = 'X';
  CHECK(f.borrowed_text[0] == 'H');
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY && o != NULL);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_PCM && n == 480u && pcm[0] == -1000);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_PCM && n == 80u && pcm[79] == 79);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_TERMINAL && n == 0u);
  CHECK(turbo_tts_state(o) == TURBO_TTS_SOURCE_DONE);
  CHECK(f.borrowed_text[0] == '\0'); /* text wiped only at settlement */
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_ESTATE);
  CHECK(turbo_tts_begin(o, 1u, "old", 3u, "test-voice", "en") == TURBO_TTS_ESTALE);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_ESTATE);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK && o == NULL);
  return 0;
}

static int test_cancel_pending_no_early_unload(void) {
  fake_provider_t f = {0};
  turbo_tts_output_t *o = NULL;
  turbo_tts_poll_t kind = TURBO_TTS_WOULD_BLOCK;
  int16_t pcm[480];
  size_t n;
  CHECK(make_output(&f, &o, 5u, 128u) == TURBO_TTS_OK);
  f.mode = FAKE_CANCEL_LATE_PCM;
  CHECK(turbo_tts_begin(o, 1u, "Stop", 4u, "voice", "en") == TURBO_TTS_OK);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_OK);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_OK);
  CHECK(f.cancel_calls == 1u);
  CHECK(f.reenter_rc == TURBO_TTS_EBUSY);
  CHECK(turbo_tts_state(o) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY && o);
  CHECK(turbo_tts_begin(o, 2u, "new", 3u, "voice", "en") == TURBO_TTS_EBUSY);
  for (size_t i = 0u; i < 480u; i++) pcm[i] = 456;
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        n == 0u && kind == TURBO_TTS_WOULD_BLOCK);
  for (size_t i = 0u; i < 480u; i++) CHECK(pcm[i] == 0);
  CHECK(turbo_tts_state(o) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        n == 0u && kind == TURBO_TTS_POLL_CANCELED);
  CHECK(turbo_tts_state(o) == TURBO_TTS_CANCELED);
  CHECK(f.borrowed_text[0] == '\0');
  f.mode = FAKE_CANCEL_SLOW;
  f.allow_settle = 0;
  CHECK(turbo_tts_begin(o, 2u, "Long", 4u, "voice", "ja") == TURBO_TTS_OK);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_OK);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_WOULD_BLOCK && n == 0u);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY);
  f.allow_settle = 1;
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_POLL_CANCELED);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK);
  CHECK(f.cancel_calls == 2u);
  return 0;
}

static int test_provider_errors_need_settlement(void) {
  const enum fake_mode bad_modes[] = {
    FAKE_BAD_COUNT, FAKE_BAD_KIND, FAKE_BAD_ZERO_PCM,
    FAKE_BAD_TERMINAL_PCM, FAKE_POLL_FAIL
  };
  for (size_t i = 0u; i < sizeof(bad_modes)/sizeof(bad_modes[0]); i++) {
    fake_provider_t f = {0};
    turbo_tts_output_t *o = NULL;
    int16_t pcm[480];
    turbo_tts_poll_t kind = TURBO_TTS_PCM;
    size_t n = 123u;
    CHECK(make_output(&f, &o, 2u, 256u) == TURBO_TTS_OK);
    f.mode = bad_modes[i];
    CHECK(turbo_tts_begin(o, 1u, "fail", 4u, "test", "zh-CN") == TURBO_TTS_OK);
    memset(pcm, 0x7f, sizeof(pcm));
    CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_EPROVIDER);
    CHECK(n == 0u && kind == TURBO_TTS_WOULD_BLOCK);
    for (size_t j = 0u; j < 480u; j++) CHECK(pcm[j] == 0);
    CHECK(f.cancel_calls == 1u);
    CHECK(turbo_tts_state(o) == TURBO_TTS_CANCEL_PENDING);
    CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY);
    CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
          kind == TURBO_TTS_POLL_CANCELED && n == 0u);
    CHECK(turbo_tts_state(o) == TURBO_TTS_FAILED);
    CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK);
  }

  fake_provider_t begin_f = {0};
  turbo_tts_output_t *o = NULL;
  int16_t pcm[480] = {0};
  size_t n = 0u;
  turbo_tts_poll_t k;
  CHECK(make_output(&begin_f, &o, 2u, 100u) == TURBO_TTS_OK);
  begin_f.mode = FAKE_BEGIN_FAIL;
  CHECK(turbo_tts_begin(o, 1u, "start", 5u, "v", "en") ==
        TURBO_TTS_EPROVIDER);
  CHECK(turbo_tts_state(o) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_OK &&
        k == TURBO_TTS_POLL_CANCELED);
  CHECK(turbo_tts_state(o) == TURBO_TTS_FAILED);
  begin_f.mode = FAKE_ALWAYS_PCM;
  CHECK(turbo_tts_begin(o, 2u, "second", 6u, "v", "en") == TURBO_TTS_OK);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_OK &&
        n == 480u && k == TURBO_TTS_PCM);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_OK &&
        n == 480u && k == TURBO_TTS_PCM);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_EPROVIDER);
  CHECK(turbo_tts_state(o) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_OK);
  CHECK(turbo_tts_state(o) == TURBO_TTS_FAILED);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK);
  return 0;
}

static int test_utf8_abi_format_and_stale(void) {
  fake_provider_t f = {0};
  turbo_tts_output_t *o = NULL;
  turbo_tts_provider_t p;
  turbo_tts_config_t c;
  turbo_tts_provider_init(&p);
  turbo_tts_config_init(&c);
  p.context = &f; p.begin = fake_begin; p.poll_pcm = fake_poll;
  p.request_cancel = fake_cancel;

  CHECK(turbo_tts_create(&c, &p, NULL) == TURBO_TTS_EINVAL);
  c.format.sample_rate_hz = 48000u;
  CHECK(turbo_tts_create(&c, &p, &o) == TURBO_TTS_EINVAL && !o);
  c.format.sample_rate_hz = 24000u;
  c.max_text_bytes = 0u;
  CHECK(turbo_tts_create(&c, &p, &o) == TURBO_TTS_EINVAL);
  c.max_text_bytes = 6u;
  CHECK(turbo_tts_create(&c, &p, &o) == TURBO_TTS_OK);
  f.output = o;
  CHECK(turbo_tts_begin(o, 1u, "<s>", 3u, "v", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "\x1b", 1u, "v", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "\xc0\xaf", 2u, "v", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "newline\n", 8u, "v", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "too-long", 8u, "v", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "valid", 5u, "", "en") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "valid", 5u, "good", "e n") == TURBO_TTS_EINVAL);
  CHECK(turbo_tts_begin(o, 1u, "valid", 5u, "good", "en") == TURBO_TTS_OK);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_OK);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_EBUSY);
  int16_t pcm[480];
  size_t n;
  turbo_tts_poll_t k;
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &k) == TURBO_TTS_OK);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK);
  p.abi_version = 5u;
  CHECK(turbo_tts_create(&c, &p, &o) == TURBO_TTS_EINVAL);
  return 0;
}

static int test_would_block_never_admits_pcm(void) {
  fake_provider_t f = {0};
  turbo_tts_output_t *o = NULL;
  int16_t pcm[480];
  size_t n = 321u;
  turbo_tts_poll_t kind = TURBO_TTS_PCM;
  CHECK(make_output(&f, &o, 5u, 64u) == TURBO_TTS_OK);
  f.mode = FAKE_WOULD_BLOCK;
  CHECK(turbo_tts_begin(o, 1u, "hi", 2u, "v", "en") == TURBO_TTS_OK);
  memset(pcm, 0x3f, sizeof(pcm));
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK);
  CHECK(n == 0u && kind == TURBO_TTS_WOULD_BLOCK);
  for (size_t i = 0u; i < 480u; i++) CHECK(pcm[i] == 0);
  CHECK(turbo_tts_state(o) == TURBO_TTS_ACTIVE);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_PCM && n == 480u);
  CHECK(turbo_tts_cancel(o) == TURBO_TTS_OK);
  CHECK(turbo_tts_poll(o, pcm, 480u, &n, &kind) == TURBO_TTS_OK &&
        kind == TURBO_TTS_POLL_CANCELED);
  CHECK(turbo_tts_destroy(&o) == TURBO_TTS_OK);
  return 0;
}

int main(void) {
  CHECK(test_pcm_and_terminal() == 0);
  CHECK(test_would_block_never_admits_pcm() == 0);
  CHECK(test_cancel_pending_no_early_unload() == 0);
  CHECK(test_provider_errors_need_settlement() == 0);
  CHECK(test_utf8_abi_format_and_stale() == 0);
  puts("PASS: C11 TTS ABI1, copied approved text, format, 20ms PCM + tail");
  puts("PASS: pending cancellation, settled DSO barrier, late PCM discard");
  puts("PASS: provider error, frame capacity, malformed text and reentry");
  return 0;
}
