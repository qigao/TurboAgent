#include "turbo_agent_tts_playback_bridge.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Test-only fake native device uses the UNMODIFIED installed SaltsUtils
 * v4.3.0-rc.11 public header. Production bridge still calls Salts directly.
 * No implementation here is linked into the product or installed SDK. */
struct salts_playback {
  uint8_t stored[4800];
  size_t count;
  size_t max_write;
  int write_error;
  int clear_error;
  unsigned writes;
  unsigned clears;
  unsigned drains;
};
static salts_playback_t *fake_device_new(void) {
  salts_playback_t *p = calloc(1, sizeof(*p));
  if (p) p->max_write = 4800u;
  return p;
}
int salts_playback_write(salts_playback_t *p, const void *pcm,
                         size_t bytes, size_t *out_written) {
  if (!out_written) return SALTS_PLAYBACK_ERR_FORMAT;
  *out_written = 0u;
  if (!p || !pcm || bytes % 2u) return SALTS_PLAYBACK_ERR_FORMAT;
  p->writes++;
  if (p->write_error) return SALTS_PLAYBACK_ERR_DEVICE;
  size_t capacity = sizeof(p->stored) - p->count;
  size_t n = bytes < capacity ? bytes : capacity;
  if (n > p->max_write) n = p->max_write;
  n &= ~(size_t)1u;
  memcpy(p->stored + p->count, pcm, n);
  p->count += n;
  *out_written = n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *p) {
  if (!p) return SALTS_PLAYBACK_ERR_FORMAT;
  if (p->clear_error) return SALTS_PLAYBACK_ERR_DEVICE;
  p->clears++;
  memset(p->stored, 0, sizeof(p->stored));
  p->count = 0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *p, uint32_t timeout_ms) {
  if (!p || timeout_ms == 0u) return SALTS_PLAYBACK_ERR_FORMAT;
  p->drains++;
  return p->count == 0u ? SALTS_PLAYBACK_OK : SALTS_PLAYBACK_ERR_TIMEOUT;
}

typedef struct fake_synth {
  uint64_t id;
  unsigned step;
  unsigned cancel_count;
  unsigned polls;
  int canceled;
  int produce_late_pcm;
} fake_synth_t;

static int fake_begin(void *ptr, uint64_t id,
                      const char *approved, size_t len,
                      const char *voice, const char *language,
                      const turbo_tts_format_t *format) {
  fake_synth_t *f = (fake_synth_t *)ptr;
  if (!id || !approved || !len || !voice || !language || !format ||
      format->sample_rate_hz != 24000u ||
      format->frame_samples != 480u ||
      format->channels != 1u || format->bits_per_sample != 16u)
    return -1;
  f->id = id;
  f->step = 0u;
  f->canceled = 0;
  return 0;
}
static int fake_poll(void *ptr, uint64_t id, int16_t *samples,
                     size_t capacity, size_t *out_count,
                     turbo_tts_poll_t *kind) {
  fake_synth_t *f = (fake_synth_t *)ptr;
  if (id != f->id || capacity != 480u) return -1;
  f->polls++;
  if (f->canceled) {
    if (f->produce_late_pcm) {
      f->produce_late_pcm = 0;
      for (size_t i = 0u; i < capacity; i++) samples[i] = 5555;
      *out_count = capacity;
      *kind = TURBO_TTS_PCM;
    } else {
      *out_count = 0u;
      *kind = TURBO_TTS_POLL_CANCELED;
    }
    return 0;
  }
  if (f->step == 0u) {
    for (size_t i = 0u; i < capacity; i++)
      samples[i] = (int16_t)i;
    *out_count = capacity;
    *kind = TURBO_TTS_PCM;
  } else if (f->step == 1u) {
    for (size_t i = 0u; i < 80u; i++)
      samples[i] = (int16_t)(500u + i);
    *out_count = 80u;
    *kind = TURBO_TTS_PCM;
  } else {
    *out_count = 0u;
    *kind = TURBO_TTS_TERMINAL;
  }
  f->step++;
  return 0;
}
static void fake_cancel(void *ptr, uint64_t id) {
  fake_synth_t *f = (fake_synth_t *)ptr;
  if (id == f->id) {
    f->cancel_count++;
    f->canceled = 1;
  }
}
static int create_source(fake_synth_t *f, turbo_tts_output_t **out) {
  turbo_tts_provider_t p;
  turbo_tts_config_t c;
  turbo_tts_provider_init(&p);
  turbo_tts_config_init(&c);
  p.context = f;
  p.begin = fake_begin;
  p.poll_pcm = fake_poll;
  p.request_cancel = fake_cancel;
  if (turbo_tts_create(&c, &p, out) != TURBO_TTS_OK) return -1;
  if (turbo_tts_begin(*out, 1u, "Approved response", 17u,
                      "test-voice", "en") != TURBO_TTS_OK) return -1;
  return 0;
}
#define CHECK(c) do {if (!(c)) { \
  fprintf(stderr,"FAIL M4.1 line %d\n", __LINE__);return 1;} \
} while (0)

static int test_ordered_short_write_and_bounded_drain(void) {
  fake_synth_t f = {0};
  turbo_tts_output_t *source = NULL;
  salts_playback_t *device = fake_device_new();
  turbo_tts_playback_bridge_t *bridge = NULL;
  CHECK(device != NULL);
  CHECK(create_source(&f, &source) == 0);
  CHECK(turbo_tts_playback_bridge_create(source, device, &bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  device->max_write = 160u; /* deliberately short even with ring capacity */
  size_t bytes = 0u;
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK && bytes == 160u);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_BACKPRESSURE);
  CHECK(f.polls == 1u); /* must NOT poll new PCM while suffix remains */
  for (unsigned tries = 0u; tries < 20u &&
       turbo_tts_playback_bridge_get_state(bridge) !=
           TURBO_TTS_PLAYBACK_SOURCE_DONE; tries++) {
    CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
          TURBO_TTS_PLAYBACK_OK);
  }
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_SOURCE_DONE);
  CHECK(f.polls == 3u);
  CHECK(device->count == (480u + 80u) * 2u);
  for (unsigned sample = 0u; sample < 480u; sample++) {
    unsigned pos = sample * 2u;
    int16_t actual = (int16_t)((unsigned)device->stored[pos] |
                    ((unsigned)device->stored[pos + 1u] << 8u));
    CHECK(actual == (int16_t)sample);
  }
  for (unsigned sample = 0u; sample < 80u; sample++) {
    unsigned pos = (480u + sample) * 2u;
    int16_t actual = (int16_t)((unsigned)device->stored[pos] |
                    ((unsigned)device->stored[pos + 1u] << 8u));
    CHECK(actual == (int16_t)(500u + sample));
  }
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_EBUSY && bridge != NULL);
  CHECK(turbo_tts_playback_bridge_drain(bridge, 10u) ==
        TURBO_TTS_PLAYBACK_EBUSY && device->drains == 1u);
  /* Fake native audio callback consumes the buffered PCM; bridge owns
   * neither device nor stream, so it does not "play" or mutate source. */
  device->count = 0u;
  CHECK(turbo_tts_playback_bridge_drain(bridge, 10u) ==
        TURBO_TTS_PLAYBACK_OK && device->drains == 2u);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_DRAINED);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  free(device);
  return 0;
}

static int test_cancel_before_settle_quiesces_writer_and_ring(void) {
  fake_synth_t f = {0};
  turbo_tts_output_t *source = NULL;
  salts_playback_t *device = fake_device_new();
  turbo_tts_playback_bridge_t *bridge = NULL;
  CHECK(device != NULL);
  CHECK(create_source(&f, &source) == 0);
  CHECK(turbo_tts_playback_bridge_create(source, device, &bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  size_t bytes = 0u;
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK && bytes == 960u && device->count == 960u);
  f.produce_late_pcm = 1;
  unsigned prev_writes = device->writes;
  CHECK(turbo_tts_playback_bridge_cancel(bridge) == TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_cancel(bridge) == TURBO_TTS_PLAYBACK_OK);
  CHECK(f.cancel_count == 1u && device->clears == 1u && device->count == 0u);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_EBUSY);
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCEL_PENDING);
  CHECK(device->writes == prev_writes && device->count == 0u);
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCELED);
  CHECK(device->writes == prev_writes && device->count == 0u);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  free(device);
  return 0;
}

static int test_zero_write_backpressure_and_device_loss(void) {
  fake_synth_t f = {0};
  turbo_tts_output_t *source = NULL;
  salts_playback_t *device = fake_device_new();
  turbo_tts_playback_bridge_t *bridge = NULL;
  CHECK(device != NULL);
  CHECK(create_source(&f, &source) == 0);
  CHECK(turbo_tts_playback_bridge_create(source, device, &bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  device->max_write = 0u;
  size_t accepted = 100u;
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_OK && accepted == 0u);
  CHECK(f.polls == 1u);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_BACKPRESSURE);
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_OK && accepted == 0u && f.polls == 1u);
  device->write_error = 1;
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_EDEVICE);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCEL_PENDING);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_EBUSY);
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCELED);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  free(device);
  return 0;
}

static int test_failed_clear_retains_owner_until_success(void) {
  fake_synth_t f = {0};
  turbo_tts_output_t *source = NULL;
  salts_playback_t *device = fake_device_new();
  turbo_tts_playback_bridge_t *bridge = NULL;
  CHECK(device != NULL);
  CHECK(create_source(&f, &source) == 0);
  CHECK(turbo_tts_playback_bridge_create(source, device, &bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  device->clear_error = 1;
  CHECK(turbo_tts_playback_bridge_cancel(bridge) ==
        TURBO_TTS_PLAYBACK_EDEVICE);
  CHECK(f.cancel_count == 1u);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCEL_PENDING);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_EBUSY);
  size_t accepted = 100u;
  /* No Provider callback/early unload until the device clear succeeds. */
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_EDEVICE && accepted == 0u);
  CHECK(f.polls == 0u && device->clears == 0u);
  device->clear_error = 0;
  CHECK(turbo_tts_playback_bridge_cancel(bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(device->clears == 1u && f.cancel_count == 1u);
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &accepted) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCELED);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  free(device);
  return 0;
}

static int test_explicit_cancel_of_source_done_discards_ring(void) {
  fake_synth_t f = {0};
  turbo_tts_output_t *source = NULL;
  salts_playback_t *device = fake_device_new();
  turbo_tts_playback_bridge_t *bridge = NULL;
  CHECK(device != NULL);
  CHECK(create_source(&f, &source) == 0);
  CHECK(turbo_tts_playback_bridge_create(source, device, &bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  size_t bytes = 0u;
  CHECK(turbo_tts_playback_bridge_pump(bridge, 3u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_SOURCE_DONE);
  CHECK(device->count == 1120u);
  CHECK(turbo_tts_playback_bridge_cancel(bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(device->clears == 1u && device->count == 0u);
  CHECK(f.cancel_count == 0u); /* Source already terminated; ring only */
  CHECK(turbo_tts_playback_bridge_pump(bridge, 1u, &bytes) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_playback_bridge_get_state(bridge) ==
        TURBO_TTS_PLAYBACK_CANCELED);
  CHECK(turbo_tts_playback_bridge_destroy(&bridge) ==
        TURBO_TTS_PLAYBACK_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  free(device);
  return 0;
}

int main(void) {
  CHECK(test_ordered_short_write_and_bounded_drain() == 0);
  CHECK(test_cancel_before_settle_quiesces_writer_and_ring() == 0);
  CHECK(test_zero_write_backpressure_and_device_loss() == 0);
  CHECK(test_failed_clear_retains_owner_until_success() == 0);
  CHECK(test_explicit_cancel_of_source_done_discards_ring() == 0);
  puts("PASS: playback borrowed owner, 480+80 S16 PCM FIFO, short write retry");
  puts("PASS: no second queue, cancel-first quiesce/clear and late PCM discard");
  puts("PASS: zero-write full-ring, device loss, source vs device drain states");
  return 0;
}
