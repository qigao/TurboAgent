#include "turbo_agent_speech_output.h"
#include "fake_tts_provider.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

static int setup(fake_tts_t *fake, uint32_t max_audio_ms,
                 uint32_t max_text_bytes,
                 turbo_agent_speech_output_t **out) {
  turbo_agent_tts_config_t config;
  turbo_agent_tts_provider_v1_t provider;
  turbo_agent_tts_config_init(&config);
  fake_tts_provider_init(fake, &provider);
  config.max_audio_ms = max_audio_ms;
  config.max_text_bytes = max_text_bytes;
  return turbo_agent_speech_output_create(&config, &provider, out);
}

static uint64_t feed_digest(uint64_t hash, const int16_t *data, size_t n) {
  for (size_t i = 0u; i < n; i++) {
    hash ^= (uint16_t)data[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static int test_finite_synthesis_and_borrowed_text(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t fake;
  const int16_t *pcm;
  size_t n, total = 0u;
  uint64_t digest = UINT64_C(14695981039346656037);
  char user_text[] = "Hello world";
  char voice[] = "voice_a";
  CHECK(setup(&fake, 30000u, 4096u, &h) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_IDLE);
  CHECK(turbo_agent_speech_output_begin(
      h, 41u, user_text, strlen(user_text), voice, "en") == TURBO_TTS_OK);
  CHECK(fake.begin_calls == 1 && fake.borrowed_text != user_text);
  CHECK(fake.borrowed_voice != voice);
  user_text[0] = 'X'; /* copied user-visible text must not change */
  voice[0] = 'X';
  CHECK(strcmp(fake.borrowed_text, "Hello world") == 0);
  CHECK(strcmp(fake.borrowed_voice, "voice_a") == 0);
  CHECK(strcmp(fake.borrowed_language, "en") == 0);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_EBUSY && h);
  CHECK(turbo_agent_speech_output_begin(
      h, 42u, "not now", 7u, "voice_b", "ja") == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_EAGAIN);
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_OK);
  CHECK(n == 480u && pcm[0] == 1779 && pcm[1] == 1810);
  digest = feed_digest(digest, pcm, 31u);
  total += 31u;
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, 31u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_EBUSY);
  CHECK(fake.poll_calls == 1);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_OK);
  CHECK(n == 449u);
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, 450u) == TURBO_TTS_EBUFFER);
  digest = feed_digest(digest, pcm, n);
  total += n;
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, n) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_OK);
  CHECK(n == 480u);
  digest = feed_digest(digest, pcm, n);
  total += n;
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, n) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_STAGING_DRAINING);
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_OK);
  CHECK(n == 137u);
  digest = feed_digest(digest, pcm, n);
  total += n;
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, 1u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 41u, &pcm, &n) == TURBO_TTS_OK);
  CHECK(n == 136u);
  CHECK(turbo_agent_speech_output_consume_pcm(h, 41u, n) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_SETTLED);
  CHECK(total == 1097u);
  CHECK(digest == UINT64_C(0xd1fb658cf793ece7));
  CHECK(fake.poll_calls == 3 && fake.cancel_calls == 0);
  CHECK(fake.borrowed_text[0] == '\0'); /* scoped content wiped at settle */
  CHECK(turbo_agent_speech_output_pump(h, 41u) == TURBO_TTS_ESTATE);
  CHECK(turbo_agent_speech_output_begin(
      h, 41u, "replay", 6u, "voice_a", "en") == TURBO_TTS_ESTALE);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_OK && h == NULL);
  return 0;
}

static int test_would_block_and_source_done(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  CHECK(setup(&f, 30000u, 4096u, &h) == TURBO_TTS_OK);
  f.would_block_count = 2;
  f.force_source_done = 1;
  CHECK(turbo_agent_speech_output_begin(h, 1u, "hello", 5u, "v1", "en") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == TURBO_TTS_EAGAIN);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == TURBO_TTS_EAGAIN);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_SYNTHESIZING);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_SETTLED);
  CHECK(f.poll_calls == 3);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_OK);
  return 0;
}

static int test_cancel_then_wait_for_settle_and_generation_fence(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  const int16_t *pcm = NULL;
  size_t n = 0u;
  CHECK(setup(&f, 30000u, 4096u, &h) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_begin(h, 10u, "hello", 5u, "v", "en") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 10u) == 0);
  f.delayed_cancel_polls = 2;
  f.emit_late_pcm_after_cancel = 1;
  CHECK(turbo_agent_speech_output_cancel(h, 10u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_cancel(h, 10u) == TURBO_TTS_OK);
  CHECK(f.cancel_calls == 1);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_peek_pcm(h, 10u, &pcm, &n) == TURBO_TTS_ESTATE);
  CHECK(turbo_agent_speech_output_pump(h, 10u) == TURBO_TTS_EAGAIN); /* late PCM dropped */
  CHECK(turbo_agent_speech_output_pump(h, 10u) == TURBO_TTS_EAGAIN);
  CHECK(turbo_agent_speech_output_pump(h, 10u) == TURBO_TTS_EAGAIN);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_pump(h, 10u) == TURBO_TTS_OK);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCELED);
  CHECK(turbo_agent_speech_output_cancel(h, 10u) == TURBO_TTS_OK);
  CHECK(f.cancel_calls == 1);
  CHECK(turbo_agent_speech_output_begin(h, 10u, "again", 5u, "v", "en") ==
        TURBO_TTS_ESTALE);
  f.delayed_cancel_polls = 0;
  CHECK(turbo_agent_speech_output_begin(h, 11u, "again", 5u, "v", "ja") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 10u) == TURBO_TTS_ESTALE);
  CHECK(turbo_agent_speech_output_cancel(h, 11u) == 0);
  CHECK(turbo_agent_speech_output_pump(h, 11u) == 0);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCELED);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  return 0;
}

static int test_provider_fault_waits_for_abort_settlement(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  CHECK(setup(&f, 30000u, 4096u, &h) == 0);
  f.fail_poll_on_call = 1;
  CHECK(turbo_agent_speech_output_begin(h, 100u, "abc", 3u, "v", "zh") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 100u) == TURBO_TTS_EPROVIDER);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCEL_PENDING);
  CHECK(f.cancel_calls == 1);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_pump(h, 100u) == 0);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_FAILED);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  return 0;
}

static int test_invalid_provider_pcm_and_capacity(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  CHECK(setup(&f, 30000u, 4096u, &h) == 0);
  f.invalid_pcm_on_call = 1;
  CHECK(turbo_agent_speech_output_begin(h, 1u, "abc", 3u, "v", "en") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == TURBO_TTS_EPROVIDER);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == 0);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_FAILED);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);

  CHECK(setup(&f, 20u, 4096u, &h) == 0);
  CHECK(turbo_agent_speech_output_begin(h, 2u, "abc", 3u, "v", "en") == 0);
  CHECK(turbo_agent_speech_output_pump(h, 2u) == 0);
  CHECK(turbo_agent_speech_output_consume_pcm(h, 2u, 480u) == 0);
  CHECK(turbo_agent_speech_output_pump(h, 2u) == TURBO_TTS_ECAPACITY);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_CANCEL_PENDING);
  CHECK(turbo_agent_speech_output_destroy(&h) == TURBO_TTS_EBUSY);
  CHECK(turbo_agent_speech_output_pump(h, 2u) == 0);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_FAILED);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  return 0;
}

static int test_plain_utf8_refusal_and_valid_unicode(void) {
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  static const char nul[] = {'a', '\0', 'b'};
  static const char malformed[] = "\xc0\xaf";
  static const char surrogate[] = "\xed\xa0\x80";
  static const char bidi[] = "\xe2\x80\xae";
  static const char c1[] = "\xc2\x80";
  static const char sep[] = "\xe2\x80\xa8";
  static const char japanese[] = "\xe3\x81\x93\xe3\x82\x93\xe3\x81\xab"
                                 "\xe3\x81\xa1\xe3\x81\xaf";
  CHECK(setup(&f, 30000u, 4096u, &h) == 0);
  CHECK(turbo_agent_speech_output_begin(h, 1u, nul, sizeof(nul), "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, malformed,
                                        sizeof(malformed)-1u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, surrogate,
                                        sizeof(surrogate)-1u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, bidi,
                                        sizeof(bidi)-1u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, c1, sizeof(c1)-1u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, sep, sizeof(sep)-1u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, "<speak>Hello</speak>",
                                        20u, "v", "en") == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, "a\nb", 3u, "v", "en")
        == TURBO_TTS_ETEXT);
  CHECK(turbo_agent_speech_output_begin(h, 1u, "abc", 3u, "path/voice", "en")
        == TURBO_TTS_EINVAL);
  CHECK(turbo_agent_speech_output_begin(h, 1u, "abc", 3u, "v", "auto")
        == TURBO_TTS_EINVAL);
  CHECK(f.begin_calls == 0);
  CHECK(turbo_agent_speech_output_begin(h, 1u, japanese,
                                        sizeof(japanese)-1u, "local_v1", "ja")
        == TURBO_TTS_OK);
  CHECK(f.begin_calls == 1);
  CHECK(turbo_agent_speech_output_cancel(h, 1u) == 0);
  CHECK(turbo_agent_speech_output_pump(h, 1u) == 0);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  return 0;
}

static int test_bad_config_and_begin_failure(void) {
  turbo_agent_tts_config_t cfg;
  turbo_agent_tts_provider_v1_t provider;
  turbo_agent_speech_output_t *h = NULL;
  fake_tts_t f;
  fake_tts_provider_init(&f, &provider);
  turbo_agent_tts_config_init(&cfg);
  cfg.output.sample_rate_hz = 48000u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) ==
        TURBO_TTS_EINVAL && h == NULL);
  cfg.output.sample_rate_hz = 24000u;
  cfg.output.channels = 2u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) ==
        TURBO_TTS_EINVAL);
  cfg.output.channels = 1u;
  cfg.max_audio_ms = 21u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) ==
        TURBO_TTS_EINVAL);
  cfg.max_audio_ms = 30000u;
  cfg.abi_version = 99u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) ==
        TURBO_TTS_EINVAL);
  cfg.abi_version = TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION;
  provider.poll_pcm = NULL;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) ==
        TURBO_TTS_EINVAL);
  fake_tts_provider_init(&f, &provider);
  cfg.max_text_bytes = 1u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) == 0);
  CHECK(turbo_agent_speech_output_begin(h, 1u, "AB", 2u, "v", "en")
        == TURBO_TTS_ECAPACITY);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  cfg.max_text_bytes = 4096u;
  CHECK(turbo_agent_speech_output_create(&cfg, &provider, &h) == 0);
  f.begin_error = 1;
  CHECK(turbo_agent_speech_output_begin(h, 2u, "good", 4u, "v", "en")
        == TURBO_TTS_EPROVIDER);
  CHECK(turbo_agent_speech_output_get_state(h) == TURBO_TTS_FAILED);
  CHECK(f.cancel_calls == 0);
  CHECK(turbo_agent_speech_output_destroy(&h) == 0);
  return 0;
}

int main(void) {
  CHECK(test_finite_synthesis_and_borrowed_text() == 0);
  CHECK(test_would_block_and_source_done() == 0);
  CHECK(test_cancel_then_wait_for_settle_and_generation_fence() == 0);
  CHECK(test_provider_fault_waits_for_abort_settlement() == 0);
  CHECK(test_invalid_provider_pcm_and_capacity() == 0);
  CHECK(test_plain_utf8_refusal_and_valid_unicode() == 0);
  CHECK(test_bad_config_and_begin_failure() == 0);
  puts("SpeechOutput M4.0 strict C11 fake TTS provider and PCM lifecycle PASS");
  return 0;
}
