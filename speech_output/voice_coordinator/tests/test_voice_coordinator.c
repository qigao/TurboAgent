#include "turbo_agent_voice_coordinator.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TEST ONLY: installed Salts Playback public signatures, no real device. */
struct salts_playback {
  uint8_t ring[4800];
  size_t count;
  size_t max_write;
  int clear_error;
  int write_error;
  unsigned writes;
  unsigned clears;
  unsigned drains;
};
int salts_playback_write(salts_playback_t *p, const void *pcm, size_t bytes,
                         size_t *written) {
  if (!written) return SALTS_PLAYBACK_ERR_FORMAT;
  *written = 0u;
  if (!p || !pcm || (bytes % 2u)) return SALTS_PLAYBACK_ERR_FORMAT;
  p->writes++;
  if (p->write_error) return SALTS_PLAYBACK_ERR_DEVICE;
  size_t available = sizeof(p->ring) - p->count;
  size_t n = bytes < available ? bytes : available;
  if (n > p->max_write) n = p->max_write;
  n &= ~(size_t)1u;
  memcpy(p->ring + p->count, pcm, n);
  p->count += n;
  *written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  if (p->clear_error) return SALTS_PLAYBACK_ERR_DEVICE;
  p->clears++;
  memset(p->ring, 0, sizeof(p->ring));
  p->count = 0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || !timeout_ms) return SALTS_PLAYBACK_ERR_FORMAT;
  p->drains++;
  return p->count ? SALTS_PLAYBACK_ERR_TIMEOUT : SALTS_PLAYBACK_OK;
}

typedef struct fake_provider {
  uint64_t generation;
  unsigned begins;
  unsigned polls;
  unsigned cancels;
  unsigned step;
  unsigned canceled;
  unsigned wait_canceled;
  int fail_begin;
  int late_pcm;
} fake_provider_t;
static int fake_begin(void *ctx, uint64_t id, const char *text, size_t len,
                      const char *voice, const char *lang,
                      const turbo_tts_format_t *format) {
  fake_provider_t *p = ctx;
  if (!id || !text || !len || !voice || !lang || !format ||
      format->sample_rate_hz != 24000u || format->channels != 1u ||
      format->bits_per_sample != 16u || format->frame_samples != 480u)
    return -1;
  p->generation = id;
  p->begins++;
  p->step = 0u;
  p->canceled = 0u;
  return p->fail_begin ? -1 : 0;
}
static int fake_poll(void *ctx, uint64_t id, int16_t *pcm, size_t capacity,
                     size_t *count, turbo_tts_poll_t *kind) {
  fake_provider_t *p = ctx;
  if (id != p->generation || capacity != 480u) return -1;
  p->polls++;
  if (p->canceled) {
    if (p->wait_canceled) {
      p->wait_canceled--;
      if (p->late_pcm) {
        for (size_t i = 0u; i < capacity; ++i) pcm[i] = 999;
        *count = capacity;
        *kind = TURBO_TTS_PCM;
      } else {
        *count = 0u;
        *kind = TURBO_TTS_WOULD_BLOCK;
      }
    } else {
      *count = 0u;
      *kind = TURBO_TTS_POLL_CANCELED;
    }
    return 0;
  }
  if (p->step < 2u) {
    size_t n = p->step ? 80u : 480u;
    for (size_t i = 0u; i < n; ++i)
      pcm[i] = (int16_t)(p->step ? (500u + i) : i);
    *count = n;
    *kind = TURBO_TTS_PCM;
  } else {
    *count = 0u;
    *kind = TURBO_TTS_TERMINAL;
  }
  p->step++;
  return 0;
}
static void fake_cancel(void *ctx, uint64_t id) {
  fake_provider_t *p = ctx;
  if (p->generation == id) { p->cancels++; p->canceled = 1u; }
}
static int setup(fake_provider_t *p, salts_playback_t *device,
                 turbo_tts_output_t **source, turbo_voice_coordinator_t **v) {
  turbo_tts_config_t config;
  turbo_tts_provider_t provider;
  turbo_tts_config_init(&config);
  turbo_tts_provider_init(&provider);
  provider.context = p;
  provider.begin = fake_begin;
  provider.poll_pcm = fake_poll;
  provider.request_cancel = fake_cancel;
  if (turbo_tts_create(&config, &provider, source) != TURBO_TTS_OK) return 1;
  return turbo_voice_create(*source, device, v) != TURBO_VOICE_OK;
}
static void device_init(salts_playback_t *p) {
  memset(p, 0, sizeof(*p));
  p->max_write = sizeof(p->ring);
}
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL M4.2 line %d: %s\n", __LINE__, #x); return 1; \
} } while (0)

static int admitted(turbo_voice_coordinator_t *v, uint64_t id) {
  CHECK(turbo_voice_begin_input(v, id) == TURBO_VOICE_OK);
  CHECK(turbo_voice_asr_event(v, id, TURBO_VOICE_ASR_CHECKED_FINAL) ==
        TURBO_VOICE_OK);
  CHECK(turbo_voice_input_quiesced(v, id) == TURBO_VOICE_OK);
  CHECK(turbo_voice_approved_final(v, id, "Approved", 8u, "fake", "en") ==
        TURBO_VOICE_OK);
  return 0;
}
static int test_admission_fifo_drain(void) {
  fake_provider_t p = {0};
  salts_playback_t dev;
  device_init(&dev);
  dev.max_write = 160u;
  turbo_tts_output_t *tts = NULL;
  turbo_voice_coordinator_t *v = NULL;
  CHECK(setup(&p, &dev, &tts, &v) == 0);
  size_t invalid_written = 100u;
  CHECK(turbo_voice_pump(v, TURBO_VOICE_MAX_POLL_BUDGET + 1u,
                         &invalid_written) == TURBO_VOICE_EINVAL &&
        invalid_written == 0u);
  CHECK(turbo_voice_drain(v, TURBO_VOICE_MAX_DRAIN_MS + 1u) ==
        TURBO_VOICE_EINVAL);
  CHECK(turbo_voice_begin_input(v, 7u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_asr_event(v, 7u, TURBO_VOICE_ASR_PARTIAL) ==
        TURBO_VOICE_IGNORED && p.begins == 0u);
  CHECK(turbo_voice_approved_final(v, 7u, "Wrong", 5u, "fake", "en") ==
        TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_input_quiesced(v, 7u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_approved_final(v, 7u, "Wrong", 5u, "fake", "en") ==
        TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_asr_event(v, 6u, TURBO_VOICE_ASR_CHECKED_FINAL) ==
        TURBO_VOICE_ESTALE);
  CHECK(turbo_voice_asr_event(v, 7u, TURBO_VOICE_ASR_CHECKED_FINAL) ==
        TURBO_VOICE_OK);
  CHECK(turbo_voice_approved_final(v, 7u, "Approved", 8u, "fake", "en") ==
        TURBO_VOICE_OK && p.begins == 1u);
  CHECK(turbo_voice_asr_event(v, 7u, TURBO_VOICE_ASR_PARTIAL) ==
        TURBO_VOICE_EBUSY); /* No output echo can be accepted as ASR. */
  CHECK(turbo_voice_begin_input(v, 8u) == TURBO_VOICE_EBUSY);
  size_t written = 0u;
  for (unsigned i = 0; i < 24u &&
       turbo_voice_get_state(v) != TURBO_VOICE_DRAIN_PENDING; ++i)
    CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_DRAIN_PENDING);
  CHECK(p.polls == 3u && dev.count == 1120u);
  for (unsigned i = 0u; i < 560u; ++i) {
    int16_t sample;
    memcpy(&sample, dev.ring + i * 2u, sizeof(sample));
    CHECK(sample == (int16_t)(i < 480u ? i : 500u + i - 480u));
  }
  CHECK(turbo_voice_drain(v, 10u) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
  dev.count = 0u; /* pretend a native device callback consumed the ring */
  CHECK(turbo_voice_drain(v, 10u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_COMPLETED);
  CHECK(turbo_voice_begin_input(v, 7u) == TURBO_VOICE_ESTALE);
  CHECK(turbo_voice_begin_input(v, 8u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_interrupt(v, 8u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_begin_input(v, 9u) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_input_quiesced(v, 8u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_INTERRUPTED);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  return 0;
}
static int test_clear_failure_noncooperative(void) {
  fake_provider_t p = {0};
  salts_playback_t dev;
  device_init(&dev);
  turbo_tts_output_t *tts = NULL;
  turbo_voice_coordinator_t *v = NULL;
  CHECK(setup(&p, &dev, &tts, &v) == 0);
  CHECK(admitted(v, 20u) == 0);
  size_t written = 0u;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(written == 960u && dev.count == 960u);
  unsigned writes = dev.writes;
  dev.clear_error = 1;
  p.wait_canceled = 3u;
  p.late_pcm = 1;
  CHECK(turbo_voice_interrupt(v, 20u) == TURBO_VOICE_EDEVICE);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_CANCEL_PENDING);
  CHECK(p.cancels == 1u);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EDEVICE);
  CHECK(p.polls == 1u && dev.writes == writes);
  dev.clear_error = 0;
  for (unsigned i = 0; i < 3u; ++i) {
    CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EBUSY);
    CHECK(dev.count == 0u && dev.writes == writes);
    CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
  }
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_INTERRUPTED);
  CHECK(dev.clears == 1u && p.cancels == 1u);
  CHECK(turbo_voice_begin_input(v, 21u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_interrupt(v, 21u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_input_quiesced(v, 21u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  return 0;
}
static int test_errors_never_mark_completed(void) {
  fake_provider_t p = {0};
  salts_playback_t dev;
  device_init(&dev);
  dev.write_error = 1;
  turbo_tts_output_t *tts = NULL;
  turbo_voice_coordinator_t *v = NULL;
  CHECK(setup(&p, &dev, &tts, &v) == 0);
  CHECK(admitted(v, 50u) == 0);
  size_t written = 9u;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EDEVICE);
  CHECK(written == 0u);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_CANCEL_PENDING);
  CHECK(turbo_voice_drain(v, 10u) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_FAILED);
  CHECK(turbo_voice_begin_input(v, 51u) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  return 0;
}
static int test_begin_failure_retains_provider(void) {
  fake_provider_t p = {0};
  p.fail_begin = 1;
  p.wait_canceled = 2u;
  salts_playback_t dev;
  device_init(&dev);
  turbo_tts_output_t *tts = NULL;
  turbo_voice_coordinator_t *v = NULL;
  CHECK(setup(&p, &dev, &tts, &v) == 0);
  CHECK(turbo_voice_begin_input(v, 90u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_asr_event(v, 90u, TURBO_VOICE_ASR_CHECKED_FINAL) ==
        TURBO_VOICE_OK);
  CHECK(turbo_voice_input_quiesced(v, 90u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_approved_final(v, 90u, "Approved", 8u, "fake", "en") ==
        TURBO_VOICE_EPROVIDER);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_CANCEL_PENDING);
  CHECK(p.cancels == 1u && dev.writes == 0u && dev.clears == 0u);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
  size_t written = 0u;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_state(v) == TURBO_VOICE_FAILED);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  return 0;
}
/* M4.2 must tell the Host which borrowed obligations prevent release,
 * rather than conflating a permanently failed clear with Provider settlement.
 * No infinite loop or sleep; these are fixed deterministic owner ticks. */
static int test_snapshot_stalled_provider_and_device_clear(void) {
  fake_provider_t p = {0};
  salts_playback_t dev;
  device_init(&dev);
  turbo_tts_output_t *tts = NULL;
  turbo_voice_coordinator_t *v = NULL;
  CHECK(setup(&p, &dev, &tts, &v) == 0);

  turbo_voice_snapshot_t snapshot;
  turbo_voice_snapshot_init(&snapshot);
  snapshot.abi_version++;
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_EINVAL);
  turbo_voice_snapshot_init(&snapshot);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.generation == 0u && snapshot.blockers == 0u);

  CHECK(admitted(v, 100u) == 0);
  size_t written = 0u;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(written == 960u && dev.count == 960u);
  const unsigned initial_writes = dev.writes;
  const unsigned initial_polls = p.polls;

  dev.clear_error = 1;
  p.wait_canceled = 1000u;  /* deterministic, non-cooperative until released */
  p.late_pcm = 1;
  CHECK(turbo_voice_interrupt(v, 100u) == TURBO_VOICE_EDEVICE);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.state == TURBO_VOICE_CANCEL_PENDING);
  CHECK((snapshot.blockers & TURBO_VOICE_BLOCK_DEVICE_CLEAR) != 0u);
  CHECK((snapshot.blockers & TURBO_VOICE_BLOCK_PROVIDER_SETTLE) != 0u);
  CHECK((snapshot.blockers & TURBO_VOICE_BLOCK_INPUT_QUIESCE) == 0u);

  /* Still-failed clear permits no Provider callback and no new PCM write. */
  for (unsigned i = 0u; i < 32u; ++i) {
    written = 17u;
    CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EDEVICE);
    CHECK(written == 0u);
    CHECK(turbo_voice_begin_input(v, 101u) == TURBO_VOICE_EBUSY);
    CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
    CHECK(v != NULL);
  }
  CHECK(dev.writes == initial_writes && p.polls == initial_polls);
  CHECK(p.cancels == 1u && dev.clears == 0u);

  dev.clear_error = 0;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EBUSY);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK((snapshot.blockers & TURBO_VOICE_BLOCK_DEVICE_CLEAR) == 0u);
  CHECK((snapshot.blockers & TURBO_VOICE_BLOCK_PROVIDER_SETTLE) != 0u);
  CHECK(dev.count == 0u && dev.clears == 1u);

  /* Even after ring clear, uncooperative Provider is still a DSO barrier. */
  for (unsigned i = 0u; i < 64u; ++i) {
    CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_EBUSY);
    CHECK(written == 0u);
    CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
    CHECK(snapshot.blockers == TURBO_VOICE_BLOCK_PROVIDER_SETTLE);
    CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_EBUSY);
  }
  CHECK(dev.writes == initial_writes && p.cancels == 1u);
  CHECK(turbo_voice_begin_input(v, 101u) == TURBO_VOICE_EBUSY);

  /* Explicit external settlement (not a forced DSO unload) releases barrier. */
  p.wait_canceled = 0u;
  CHECK(turbo_voice_pump(v, 1u, &written) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.state == TURBO_VOICE_INTERRUPTED);
  CHECK(snapshot.blockers == 0u && snapshot.generation == 100u);

  CHECK(turbo_voice_begin_input(v, 101u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.state == TURBO_VOICE_INPUT);
  CHECK(snapshot.blockers == TURBO_VOICE_BLOCK_INPUT_QUIESCE);
  CHECK(turbo_voice_asr_event(v, 100u, TURBO_VOICE_ASR_CHECKED_FINAL) ==
        TURBO_VOICE_ESTALE);
  CHECK(turbo_voice_interrupt(v, 101u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.state == TURBO_VOICE_CANCEL_PENDING);
  CHECK(snapshot.blockers == TURBO_VOICE_BLOCK_INPUT_QUIESCE);
  CHECK(turbo_voice_input_quiesced(v, 101u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_get_snapshot(v, &snapshot) == TURBO_VOICE_OK);
  CHECK(snapshot.state == TURBO_VOICE_INTERRUPTED && snapshot.blockers == 0u);
  CHECK(turbo_voice_destroy(&v) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&tts) == TURBO_TTS_OK);
  return 0;
}

int main(void) {
  CHECK(test_admission_fifo_drain() == 0);
  CHECK(test_clear_failure_noncooperative() == 0);
  CHECK(test_errors_never_mark_completed() == 0);
  CHECK(test_begin_failure_retains_provider() == 0);
  CHECK(test_snapshot_stalled_provider_and_device_clear() == 0);
  puts("PASS M4.2 checked FINAL + quiesced input; ASR PARTIAL rejected");
  puts("PASS M4.2 FIFO/short-write/drain, generation and ASR-event isolation");
  puts("PASS M4.2 failed clear/noncooperative Provider retain borrowed DSO");
  puts("PASS M4.2 device and Provider failures cannot mark output played");
  puts("PASS M4.2 read-only blocker snapshot, failed clear and stalled DSO");
  return 0;
}
