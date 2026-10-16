#include "turbo_agent_speech_capture.h"
#include "turbo_agent_voice_coordinator.h"

#include <salts_capture.h>
#include <salts_playback.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This program dynamically imports BOTH official published SaltsUtils.Native
 * 4.3.0-rc.11 Capture and Playback DSOs, not the original source test-null
 * Playback from #202. The ONLY allowed device IDs come from a PRIVATE
 * single-null-sink PulseAudio server, with its monitor as the sole source.
 *
 * Real original M1 bounded queue + M2 Native Capture adapter are compiled
 * unchanged from exact review SHA 9e51d377. ASR recognition and TTS
 * synthesis are clearly TEST-ONLY deterministic, no model/voice weights.
 * This test never opens the default microphone or a physical speaker. */
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL published virtual Capture/Playback line %d: %s\n", \
          __LINE__, #x); return 1; \
} } while (0)
#define NATIVE_WAIT_MS 6000u

typedef struct fake_asr {
  uint64_t generation;
  uint64_t last_timestamp;
  uint32_t begins;
  uint32_t accepted;
  uint32_t finals;
  uint32_t cancels;
} fake_asr_t;

static int asr_begin(void *userdata, uint64_t generation) {
  fake_asr_t *p = (fake_asr_t *)userdata;
  if (!p || p->generation != generation) return -1;
  p->begins++;
  return 0;
}
static int asr_frame(void *userdata, const int16_t *pcm,
                     size_t count, uint64_t timestamp,
                     turbo_agent_speech_transcript_t *partial) {
  fake_asr_t *p = (fake_asr_t *)userdata;
  if (!p || !pcm || !partial || count != 320u) return -1;
  if (p->accepted && timestamp <= p->last_timestamp) return -1;
  p->last_timestamp = timestamp;
  p->accepted++;
  *partial = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_NONE, NULL, 0u
  };
  return 0;
}
static int asr_finish(void *userdata,
                      turbo_agent_speech_transcript_t *final) {
  static const char recognized[] = "virtual caller audio";
  fake_asr_t *p = (fake_asr_t *)userdata;
  if (!p || !final || !p->accepted) return -1;
  p->finals++;
  *final = (turbo_agent_speech_transcript_t){
    TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL,
    recognized, sizeof(recognized) - 1u
  };
  return 0;
}
static void asr_cancel(void *userdata) {
  fake_asr_t *p = (fake_asr_t *)userdata;
  if (p) p->cancels++;
}

typedef struct fake_tts {
  uint64_t generation;
  uint32_t begins;
  uint32_t polls;
  uint32_t frames;
  uint32_t cancels;
  int emitted;
  int canceled;
} fake_tts_t;
static int tts_begin(void *context, uint64_t generation,
                     const char *approved, size_t bytes,
                     const char *voice_id, const char *language,
                     const turbo_tts_format_t *format) {
  fake_tts_t *p = (fake_tts_t *)context;
  if (!p || generation != 42u || !approved || bytes != 2u ||
      memcmp(approved, "ok", bytes) || !voice_id || !language ||
      !format || format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_samples != 480u) return -1;
  p->begins++;
  p->generation = generation;
  p->emitted = 0;
  p->canceled = 0;
  return 0;
}
static int tts_poll(void *context, uint64_t generation,
                    int16_t *samples, size_t capacity,
                    size_t *out_samples, turbo_tts_poll_t *kind) {
  fake_tts_t *p = (fake_tts_t *)context;
  if (!p || generation != p->generation || !samples ||
      !out_samples || !kind || capacity != 480u) return -1;
  p->polls++;
  if (p->canceled) {
    *out_samples = 0u;
    *kind = TURBO_TTS_POLL_CANCELED;
  } else if (p->emitted) {
    *out_samples = 0u;
    *kind = TURBO_TTS_TERMINAL;
  } else {
    for (size_t i = 0u; i < capacity; ++i)
      samples[i] = (int16_t)(3000u + (uint32_t)i);
    p->frames++;
    p->emitted = 1;
    *out_samples = capacity;
    *kind = TURBO_TTS_PCM;
  }
  return 0;
}
static void tts_cancel(void *context, uint64_t generation) {
  fake_tts_t *p = (fake_tts_t *)context;
  if (p && p->generation == generation && !p->canceled) {
    p->canceled = 1;
    p->cancels++;
  }
}

static int require_virtual_devices(
    char *out_capture_id, size_t max_bytes,
    salts_playback_device_t *out_playback) {
  const char *explicit_permit = getenv("M42_VIRTUAL_PULSE_ONLY");
  const char *server = getenv("PULSE_SERVER");
  if (!explicit_permit || strcmp(explicit_permit, "1") != 0 ||
      !server || strncmp(server, "unix:", 5u)) return -1;
  salts_capture_device_t caps[SALTS_CAPTURE_MAX_DEVICES] = {{0}};
  int count = salts_capture_list_audio_devices(
      caps, SALTS_CAPTURE_MAX_DEVICES);
  if (count != 1 || caps[0].type != SALTS_CAPTURE_TYPE_AUDIO ||
      !caps[0].id[0] || !strstr(caps[0].name, "TurboAgentVirtual")) {
    fprintf(stderr, "unsafe Capture context: count=%d name='%s'\n",
            count, count > 0 ? caps[0].name : "none");
    return -1;
  }
  if (strlen(caps[0].id) >= max_bytes) return -1;
  strcpy(out_capture_id, caps[0].id);

  salts_playback_device_t devices[SALTS_PLAYBACK_MAX_DEVICES] = {{0}};
  size_t output_count = 0u;
  if (salts_playback_list_devices(
       devices, SALTS_PLAYBACK_MAX_DEVICES, &output_count) !=
      SALTS_PLAYBACK_OK || output_count != 1u ||
      devices[0].id_size == 0u ||
      !strstr(devices[0].name, "TurboAgentVirtual")) {
    fprintf(stderr, "unsafe Playback enumeration count=%zu name='%s'\n",
            output_count,
            output_count ? devices[0].name : "none");
    return -1;
  }
  *out_playback = devices[0]; /* unchanged opaque enumeration identity */
  fprintf(stderr, "Virtual devices: capture '%s' id='%s', output '%s' opaque=%u\n",
          caps[0].name, out_capture_id,
          out_playback->name, out_playback->id_size);
  return 0;
}

static int new_input(fake_asr_t *p, turbo_agent_speech_input_t **input) {
  turbo_agent_speech_config_t cfg;
  turbo_agent_speech_provider_v1_t hooks;
  turbo_agent_speech_config_init(&cfg);
  turbo_agent_speech_provider_v1_init(&hooks);
  cfg.queue_frames = 128u;
  cfg.max_utterance_frames = 128u;
  cfg.max_text_bytes = 128u;
  hooks.context = p;
  hooks.begin = asr_begin;
  hooks.accept_frame = asr_frame;
  hooks.finish = asr_finish;
  hooks.cancel = asr_cancel;
  return turbo_agent_speech_create(&cfg, &hooks, input);
}
static int new_tts(fake_tts_t *p, turbo_tts_output_t **out) {
  turbo_tts_provider_t hooks;
  turbo_tts_config_t cfg;
  turbo_tts_config_init(&cfg);
  turbo_tts_provider_init(&hooks);
  hooks.context = p;
  hooks.begin = tts_begin;
  hooks.poll_pcm = tts_poll;
  hooks.request_cancel = tts_cancel;
  return turbo_tts_create(&cfg, &hooks, out);
}

static int run_same_process_duplex(const char *capture_id,
                                  const salts_playback_device_t *output_id) {
  salts_playback_config_t pcm = {
    24000u, 1u, SALTS_PLAYBACK_FORMAT_S16, 10u
  };
  salts_playback_t *output = NULL;
  CHECK(salts_playback_create(output_id, &pcm, &output) == SALTS_PLAYBACK_OK);
  CHECK(output && salts_playback_get_state(output) ==
                  SALTS_PLAYBACK_STATE_STOPPED);
  CHECK(salts_playback_get_available(output) == 480u);

  fake_asr_t asr = {.generation=42u};
  fake_tts_t tts = {0};
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_salts_capture_t *native = NULL;
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  CHECK(new_input(&asr, &input) == TURBO_AGENT_SPEECH_OK);
  CHECK(new_tts(&tts, &source) == TURBO_TTS_OK);
  CHECK(turbo_voice_create(source, output, &voice) == TURBO_VOICE_OK);
  CHECK(turbo_voice_begin_input(voice, 42u) == TURBO_VOICE_OK);
  CHECK(turbo_agent_speech_salts_capture_create(
      input, 42u, capture_id, &native) == TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_start(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_voice_approved_final(
      voice, 42u, "ok", 2u, "local", "en") == TURBO_VOICE_EBUSY);
  CHECK(tts.begins == 0u && salts_playback_get_buffered(output) == 0u);

  size_t accepted = 0u;
  const uint64_t cutoff = cmeta_monotonic_ms() + NATIVE_WAIT_MS;
  while (!accepted) {
    CHECK(cmeta_monotonic_ms() < cutoff);
    CHECK(turbo_agent_speech_salts_capture_first_error(native) ==
          TURBO_AGENT_SPEECH_OK);
    size_t pumped = 0u;
    CHECK(turbo_agent_speech_pump(input, 8u, &pumped) ==
          TURBO_AGENT_SPEECH_OK);
    accepted += pumped;
    if (!accepted) cmeta_sleep_ms(2u);
  }
  CHECK(turbo_agent_speech_salts_capture_finish(native) ==
        TURBO_AGENT_SPEECH_OK);
  CHECK(turbo_agent_speech_salts_capture_destroy(&native) ==
        TURBO_AGENT_SPEECH_OK && native == NULL);
  const uint64_t settle_limit = cmeta_monotonic_ms() + 1000u;
  while (turbo_agent_speech_get_state(input) ==
         TURBO_AGENT_SPEECH_DRAINING) {
    CHECK(cmeta_monotonic_ms() < settle_limit);
    size_t pumped = 0u;
    CHECK(turbo_agent_speech_pump(input, 128u, &pumped) ==
          TURBO_AGENT_SPEECH_OK);
    accepted += pumped;
  }
  CHECK(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
  char raw_asr[128] = {0};
  size_t n = 0u;
  turbo_agent_speech_transcript_kind_t kind = TURBO_AGENT_SPEECH_NONE;
  CHECK(turbo_agent_speech_take_final(input, raw_asr, sizeof(raw_asr),
      &n, &kind) == TURBO_AGENT_SPEECH_OK);
  CHECK(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL &&
        n == strlen("virtual caller audio") &&
        strcmp(raw_asr, "virtual caller audio") == 0);
  CHECK(turbo_agent_speech_take_final(input, raw_asr, sizeof(raw_asr),
      &n, &kind) == TURBO_AGENT_SPEECH_ESTATE);
  CHECK(turbo_agent_speech_destroy(&input) == TURBO_AGENT_SPEECH_OK);
  CHECK(asr.begins == 1u && asr.finals == 1u &&
        asr.cancels == 0u && accepted == asr.accepted);

  CHECK(turbo_voice_asr_event(
      voice, 43u, TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_ESTALE);
  CHECK(turbo_voice_asr_event(
      voice, 42u, TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_OK);
  CHECK(turbo_voice_input_quiesced(voice, 42u) == TURBO_VOICE_OK);
  /* This "ok" is an explicit HOST APPROVED test-only answer, not raw
   * ASR text and not a real remote Harness/LLM or authentic voice. */
  CHECK(turbo_voice_approved_final(
      voice, 42u, "ok", 2u, "local", "en") == TURBO_VOICE_OK);
  size_t written = 0u;
  CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  CHECK(written == 480u && salts_playback_get_buffered(output) == 480u);
  CHECK(tts.begins == 1u && tts.frames == 1u && tts.polls == 1u);
  CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK &&
        written == 0u && tts.polls == 1u);
  CHECK(salts_playback_drain(output, 5u) == SALTS_PLAYBACK_ERR_BUSY);
  CHECK(salts_playback_start(output) == SALTS_PLAYBACK_OK);
  size_t total_written = 480u;
  const uint64_t deadline = cmeta_monotonic_ms() + 2000u;
  while (turbo_voice_get_state(voice) != TURBO_VOICE_DRAIN_PENDING) {
    CHECK(cmeta_monotonic_ms() < deadline);
    written = 0u;
    CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
    total_written += written;
    cmeta_sleep_ms(1u);
  }
  CHECK(total_written == 960u && tts.frames == 1u && tts.polls == 2u);
  CHECK(turbo_voice_drain(voice, 1000u) == TURBO_VOICE_OK);
  CHECK(salts_playback_get_buffered(output) == 0u);
  CHECK(turbo_voice_get_state(voice) == TURBO_VOICE_COMPLETED);
  CHECK(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  CHECK(salts_playback_stop(output) == SALTS_PLAYBACK_OK);
  salts_playback_destroy(output);
  puts("PASS original M1/M2 virtual Capture callback -> checked FINAL -> M4.2 on RELEASED Salts Playback DSO drain");
  return 0;
}
static int test_native_playback_interrupt(
    const salts_playback_device_t *output_id) {
  salts_playback_config_t pcm = {
    24000u, 1u, SALTS_PLAYBACK_FORMAT_S16, 100u
  };
  salts_playback_t *output = NULL;
  CHECK(salts_playback_create(output_id, &pcm, &output) == SALTS_PLAYBACK_OK);
  CHECK(salts_playback_start(output) == SALTS_PLAYBACK_OK);
  fake_tts_t tts = {0};
  turbo_tts_output_t *source = NULL;
  turbo_voice_coordinator_t *voice = NULL;
  CHECK(new_tts(&tts, &source) == TURBO_TTS_OK);
  CHECK(turbo_voice_create(source, output, &voice) == TURBO_VOICE_OK);
  CHECK(turbo_voice_begin_input(voice, 42u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_asr_event(
      voice, 42u, TURBO_VOICE_ASR_CHECKED_FINAL) == TURBO_VOICE_OK);
  CHECK(turbo_voice_input_quiesced(voice, 42u) == TURBO_VOICE_OK);
  CHECK(turbo_voice_approved_final(
      voice, 42u, "ok", 2u, "local", "en") == TURBO_VOICE_OK);
  size_t written = 0u;
  CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  CHECK(written == 960u && tts.frames == 1u);
  CHECK(turbo_voice_interrupt(voice, 42u) == TURBO_VOICE_OK);
  CHECK(salts_playback_get_buffered(output) == 0u);
  CHECK(salts_playback_get_state(output) == SALTS_PLAYBACK_STATE_PLAYING);
  CHECK(tts.cancels == 1u);
  written = 1u;
  CHECK(turbo_voice_pump(voice, 1u, &written) == TURBO_VOICE_OK);
  CHECK(written == 0u && turbo_voice_get_state(voice) ==
                         TURBO_VOICE_INTERRUPTED);
  CHECK(turbo_voice_destroy(&voice) == TURBO_VOICE_OK);
  CHECK(turbo_tts_destroy(&source) == TURBO_TTS_OK);
  CHECK(salts_playback_stop(output) == SALTS_PLAYBACK_OK);
  salts_playback_destroy(output);
  puts("PASS RELEASED Salts Playback DSO active native callback clear/restart on M4.2 interrupt");
  return 0;
}
int main(void) {
  char capture_id[128] = {0};
  salts_playback_device_t output_id = {0};
  CHECK(require_virtual_devices(capture_id, sizeof(capture_id),
                                &output_id) == 0);
  CHECK(run_same_process_duplex(capture_id, &output_id) == 0);
  CHECK(test_native_playback_interrupt(&output_id) == 0);
  puts("PASS fully published Salts Capture+Playback DSOs on isolated virtual PulseAudio only (NO physical audio)");
  return 0;
}
