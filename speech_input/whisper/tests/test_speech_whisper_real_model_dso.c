/* CI-only installed Whis​​per CPU model + real Provider ELF lifecycle proof.
 * Host is strict C11 and deliberately NOT linked to libwhisper or the
 * production Provider image. Only a new maintainer-approved CI label
 * event supplies model bytes; this test never downloads or stores them.
 *
 * Flow: dlopen -> SHA-qualified real ggml-tiny -> M1 PCM -> real whisper_full
 * -> FINAL / cross-thread abort / monotonic deadline -> M1 settle ->
 * owner destroy -> dlclose -> verify ELF image is no longer mapped.
 *
 * Only request_abort may run concurrently with Provider finish. In
 * particular, NEVER call destroy/dlclose while the decode worker runs.
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "turbo_agent_speech_whisper.h"
#include "turbo_agent_speech_input.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define REQUIRE(condition) do { if (!(condition)) { \
  fprintf(stderr, "FAIL:%s:%d: %s\n", __FILE__, __LINE__, #condition); \
  return 1; \
} } while (0)

typedef void (*config_init_fn)(turbo_agent_speech_whisper_config_t *);
typedef int (*create_fn)(const turbo_agent_speech_whisper_config_t *,
                         turbo_agent_speech_whisper_t **);
typedef int (*provider_fn)(turbo_agent_speech_whisper_t *,
                           turbo_agent_speech_provider_v1_t *);
typedef int (*abort_fn)(turbo_agent_speech_whisper_t *);
typedef int (*last_error_fn)(const turbo_agent_speech_whisper_t *);
typedef int (*destroy_fn)(turbo_agent_speech_whisper_t **);

typedef struct image_api_s {
  void *image;
  config_init_fn config_init;
  create_fn create;
  provider_fn provider;
  abort_fn abort_decode;
  last_error_fn last_error;
  destroy_fn destroy;
} image_api_t;

static int symbol_get(void *image, const char *name, void *dst,
                      size_t size) {
  void *address;
  if (size != sizeof(address)) return -1;
  dlerror();
  address = dlsym(image, name);
  if (dlerror() || !address) return -1;
  memcpy(dst, &address, size); /* ELF/POSIX function pointer representation */
  return 0;
}
#define SYMBOL(api, field, name) \
  do { if (symbol_get((api)->image, name, &(api)->field, \
                      sizeof((api)->field)) != 0) return -1; } while (0)

static int image_load(const char *path, image_api_t *api) {
  memset(api, 0, sizeof(*api));
  api->image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!api->image) {
    fprintf(stderr, "FAIL: cannot dlopen real Provider: %s\n", dlerror());
    return -1;
  }
  SYMBOL(api, config_init, "turbo_agent_speech_whisper_config_init");
  SYMBOL(api, create, "turbo_agent_speech_whisper_create");
  SYMBOL(api, provider, "turbo_agent_speech_whisper_provider");
  SYMBOL(api, abort_decode, "turbo_agent_speech_whisper_request_abort");
  SYMBOL(api, last_error, "turbo_agent_speech_whisper_last_error");
  SYMBOL(api, destroy, "turbo_agent_speech_whisper_destroy");
  return 0;
}
static int image_unload(const char *path, image_api_t *api) {
  void *probe;
  if (!api->image || dlclose(api->image) != 0) return -1;
  api->image = NULL;
  probe = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
  if (probe) {
    dlclose(probe);
    return -1;
  }
  memset(api, 0, sizeof(*api));
  return 0;
}

static int hex_nibble(char x) {
  if (x >= '0' && x <= '9') return x - '0';
  if (x >= 'a' && x <= 'f') return x - 'a' + 10;
  if (x >= 'A' && x <= 'F') return x - 'A' + 10;
  return -1;
}
static int decode_sha(const char *hex, uint8_t sha[32]) {
  int nonzero = 0;
  if (strlen(hex) != 64u) return -1;
  for (unsigned i = 0; i < 32u; i++) {
    int hi = hex_nibble(hex[2u * i]);
    int lo = hex_nibble(hex[2u * i + 1u]);
    if (hi < 0 || lo < 0) return -1;
    sha[i] = (uint8_t)((hi << 4) | lo);
    nonzero |= sha[i];
  }
  return nonzero ? 0 : -1;
}
static uint16_t get16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}
static uint32_t get32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
         ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}
/* Only the CI-canonicalized 44-byte mono 16kHz S16LE RIFF/WAVE
 * container is accepted. Read one 20ms frame at a time, never persist PCM. */
static int wav_frame_count(const char *path, size_t *out_frames) {
  uint8_t h[44];
  FILE *f = fopen(path, "rb");
  long size;
  uint32_t bytes;
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 44 ||
      fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) {
    fclose(f);
    return -1;
  }
  fclose(f);
  bytes = get32(h + 40);
  if (memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVEfmt ", 8) != 0 ||
      memcmp(h + 36, "data", 4) != 0 || get32(h + 16) != 16u ||
      get16(h + 20) != 1u || get16(h + 22) != 1u ||
      get32(h + 24) != 16000u || get32(h + 28) != 32000u ||
      get16(h + 32) != 2u || get16(h + 34) != 16u ||
      (uint64_t)bytes + 44u != (uint64_t)size ||
      get32(h + 4) != bytes + 36u || bytes == 0u ||
      bytes % 640u != 0u || bytes / 640u > 3000u) return -1;
  *out_frames = (size_t)(bytes / 640u);
  return 0;
}

static int stage_pcm(const char *wav_path, size_t frames,
                     turbo_agent_speech_input_t *input,
                     uint64_t utterance_id) {
  uint8_t bytes[640];
  int16_t pcm[320];
  FILE *wav = fopen(wav_path, "rb");
  size_t pumped;
  if (!wav || fseek(wav, 44L, SEEK_SET) != 0) {
    if (wav) fclose(wav);
    return -1;
  }
  for (size_t frame = 0; frame < frames; frame++) {
    if (fread(bytes, 1, sizeof(bytes), wav) != sizeof(bytes)) {
      fclose(wav);
      return -1;
    }
    for (size_t i = 0; i < 320u; i++) {
      const uint16_t v = get16(bytes + 2u * i);
      pcm[i] = (int16_t)(v < 32768u ? (int)v : (int)v - 65536);
    }
    memset(bytes, 0, sizeof(bytes));
    if (turbo_agent_speech_offer_frame(input, utterance_id, pcm, 320u,
        (uint64_t)(frame + 1u) * 20000u) != 0 ||
        turbo_agent_speech_pump(input, 1u, &pumped) != 0 ||
        pumped != 1u) {
      memset(pcm, 0, sizeof(pcm));
      fclose(wav);
      return -1;
    }
    memset(pcm, 0, sizeof(pcm));
  }
  if (fgetc(wav) != EOF || ferror(wav) || fclose(wav) != 0) return -1;
  return turbo_agent_speech_request_finish(input);
}

typedef struct decode_work_s {
  turbo_agent_speech_input_t *input;
  atomic_int started, finished;
  int rc;
} decode_work_t;

static void *decode_worker(void *context) {
  decode_work_t *work = (decode_work_t *)context;
  size_t pumped = 0;
  atomic_store_explicit(&work->started, 1, memory_order_release);
  work->rc = turbo_agent_speech_pump(work->input, 1u, &pumped);
  atomic_store_explicit(&work->finished, 1, memory_order_release);
  return NULL;
}
static uint64_t ns_from_timespec(const struct timespec *t) {
  return (uint64_t)t->tv_sec * UINT64_C(1000000000) +
         (uint64_t)t->tv_nsec;
}
/* The decode worker has no queued PCM work left. Requiring measurable
 * CPU time *inside* its synchronous final pump, while it is still running,
 * prevents an abort-before-worker-start from qualifying as real inference
 * cancellation. Actual engine entry is not externally instrumented. */
static int run_abort_in_worker(image_api_t *api,
                               turbo_agent_speech_whisper_t *owner,
                               turbo_agent_speech_input_t *input) {
  decode_work_t work = {0};
  pthread_t thread;
  clockid_t cpu_clock;
  struct timespec beginning, current;
  const struct timespec poll_pause = {0, 2000000L};
  int observed_cpu = 0;
  int abort_rc, joined;
  work.input = input;
  atomic_init(&work.started, 0);
  atomic_init(&work.finished, 0);
  if (pthread_create(&thread, NULL, decode_worker, &work) != 0) return -1;
  if (pthread_getcpuclockid(thread, &cpu_clock) != 0 ||
      clock_gettime(cpu_clock, &beginning) != 0) {
    (void)api->abort_decode(owner);
    (void)pthread_join(thread, NULL);
    return -1;
  }
  for (unsigned poll = 0u; poll < 6000u; poll++) {
    if (atomic_load_explicit(&work.finished, memory_order_acquire)) break;
    if (atomic_load_explicit(&work.started, memory_order_acquire) &&
        clock_gettime(cpu_clock, &current) == 0 &&
        ns_from_timespec(&current) >= ns_from_timespec(&beginning) +
                                     UINT64_C(20000000)) {
      observed_cpu = 1; /* >=20ms worker CPU time, not just wall time */
      break;
    }
    (void)nanosleep(&poll_pause, NULL);
  }
  abort_rc = api->abort_decode(owner); /* sole concurrent Owner operation */
  joined = pthread_join(thread, NULL);
  if (!observed_cpu || abort_rc != 0 || joined != 0 ||
      work.rc != TURBO_AGENT_SPEECH_EPROVIDER) return -1;
  return 0;
}

static int run_case(const char *dso, const char *model_path,
                    const uint8_t sha[32], const char *wav_path,
                    size_t frames, unsigned generation, unsigned kind) {
  image_api_t api;
  turbo_agent_speech_whisper_config_t model_config;
  turbo_agent_speech_config_t speech_config;
  turbo_agent_speech_provider_v1_t provider;
  turbo_agent_speech_whisper_t *owner = NULL;
  turbo_agent_speech_input_t *input = NULL;
  uint64_t id = generation + 1u;
  size_t pumped = 0u, length = 0u;
  turbo_agent_speech_transcript_kind_t terminal;
  char transcript[1025] = {0};
  int decode_rc;

  REQUIRE(image_load(dso, &api) == 0);
  api.config_init(&model_config);
  model_config.model_path = model_path;
  memcpy(model_config.expected_sha256, sha, 32u);
  model_config.max_model_bytes = 128u * 1024u * 1024u;
  model_config.max_utterance_frames = (uint32_t)frames;
  model_config.max_text_bytes = 1024u;
  model_config.cpu_threads = 2u;
  model_config.max_decode_ms = kind == 2u ? 1u : 120000u;
  model_config.language = TURBO_AGENT_WHISPER_LANGUAGE_EN;
  REQUIRE(api.create(&model_config, &owner) == 0 && owner != NULL);
  REQUIRE(api.provider(owner, &provider) == 0);
  turbo_agent_speech_config_init(&speech_config);
  speech_config.max_utterance_frames = (uint32_t)frames;
  speech_config.max_text_bytes = 1024u;
  REQUIRE(turbo_agent_speech_create(&speech_config, &provider, &input) == 0);
  REQUIRE(turbo_agent_speech_begin(input, id) == 0);
  /* The Owner must not be released while M1 has a borrowed callback. */
  REQUIRE(api.destroy(&owner) == TURBO_AGENT_SPEECH_EBUSY && owner != NULL);
  REQUIRE(stage_pcm(wav_path, frames, input, id) == 0);
  if (kind == 1u) {
    REQUIRE(run_abort_in_worker(&api, owner, input) == 0);
    decode_rc = TURBO_AGENT_SPEECH_EPROVIDER;
  } else {
    decode_rc = turbo_agent_speech_pump(input, 1u, &pumped);
  }
  if (kind == 0u) {
    REQUIRE(decode_rc == 0);
    REQUIRE(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
    REQUIRE(turbo_agent_speech_take_final(input, transcript, sizeof(transcript),
                                         &length, &terminal) == 0);
    REQUIRE(terminal == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL);
    REQUIRE(length > 0u && length <= 1024u);
    REQUIRE(strstr(transcript, "country") != NULL);
  } else {
    REQUIRE(decode_rc == TURBO_AGENT_SPEECH_EPROVIDER);
    REQUIRE(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FAILED);
    REQUIRE(api.last_error(owner) == TURBO_AGENT_WHISPER_EABORT);
    REQUIRE(turbo_agent_speech_take_final(input, transcript, sizeof(transcript),
                                         &length, &terminal) ==
            TURBO_AGENT_SPEECH_ESTATE);
    REQUIRE(length == 0u && transcript[0] == '\0');
  }
  memset(transcript, 0, sizeof(transcript));
  REQUIRE(turbo_agent_speech_destroy(&input) == 0 && input == NULL);
  REQUIRE(api.destroy(&owner) == 0 && owner == NULL);
  REQUIRE(image_unload(dso, &api) == 0);
  printf("real_whisper_generation=%u scenario=%s settled=true dso_unmapped=true\n",
         generation, kind == 0u ? "final" : kind == 1u ? "abort" : "deadline");
  return 0;
}

int main(int argc, char **argv) {
  uint8_t digest[32];
  size_t frames = 0u;
  if (argc != 5 || argv[1][0] != '/' || argv[2][0] != '/' ||
      argv[4][0] != '/' || decode_sha(argv[3], digest) != 0 ||
      wav_frame_count(argv[4], &frames) != 0) {
    fputs("usage: real-dso-test ABS_PROVIDER_DSO ABS_MODEL SHA256 ABS_CANONICAL_WAV\n", stderr);
    return 2;
  }
  /* At least two independent real-image generations per terminal outcome.
   * No mock Whisper symbols, network, microphone or Agent Session. */
  for (unsigned repetition = 0u; repetition < 2u; repetition++) {
    for (unsigned kind = 0u; kind < 3u; kind++) {
      if (run_case(argv[1], argv[2], digest, argv[4], frames,
                   repetition * 3u + kind, kind) != 0) return 1;
    }
  }
  puts("PASS: 6 real-weight Whisper CPU Provider ELF settle/unload generations");
  return 0;
}
