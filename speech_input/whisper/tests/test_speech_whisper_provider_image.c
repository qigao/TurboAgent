/* Actual ELF dlopen/dlclose + strict C11 Provider/M1 ABI gate.
 * Inference is test-only deterministic whisper v1.9.5 ABI emulation.
 * No model weights are used and no acoustic quality is claimed. */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "turbo_agent_speech_whisper.h"
#include "turbo_agent_speech_input.h"
#include <cmeta_crypto.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); return 1; \
} } while (0)

typedef void (*config_init_fn)(turbo_agent_speech_whisper_config_t *);
typedef int (*create_fn)(const turbo_agent_speech_whisper_config_t *,
                         turbo_agent_speech_whisper_t **);
typedef int (*provider_fn)(turbo_agent_speech_whisper_t *,
                           turbo_agent_speech_provider_v1_t *);
typedef int (*abort_fn)(turbo_agent_speech_whisper_t *);
typedef int (*destroy_fn)(turbo_agent_speech_whisper_t **);
typedef void (*fake_hold_fn)(int);
typedef int (*fake_status_fn)(void);

typedef struct image_api_s {
  void *image;
  config_init_fn config_init;
  create_fn create;
  provider_fn provider;
  abort_fn abort_decode;
  destroy_fn destroy;
  fake_hold_fn hold;
  fake_status_fn entered;
  fake_status_fn decodes;
  fake_status_fn frees;
} image_api_t;

static int get_sym(void *image, const char *name, void *dst, size_t size) {
  void *symbol;
  if (size != sizeof(symbol)) return -1;
  dlerror();
  symbol = dlsym(image, name);
  if (dlerror() != NULL || !symbol) return -1;
  memcpy(dst, &symbol, size); /* POSIX ELF function-symbol representation */
  return 0;
}
#define REQUIRE_SYMBOL(api, member, symbol) \
  do { if (get_sym((api)->image, symbol, &(api)->member, \
                   sizeof((api)->member)) != 0) return -1; } while (0)

static int load_image(const char *path, image_api_t *api) {
  memset(api, 0, sizeof(*api));
  api->image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!api->image) {
    fprintf(stderr, "Provider DSO failed to load: %s\n", dlerror());
    return -1;
  }
  REQUIRE_SYMBOL(api, config_init, "turbo_agent_speech_whisper_config_init");
  REQUIRE_SYMBOL(api, create, "turbo_agent_speech_whisper_create");
  REQUIRE_SYMBOL(api, provider, "turbo_agent_speech_whisper_provider");
  REQUIRE_SYMBOL(api, abort_decode, "turbo_agent_speech_whisper_request_abort");
  REQUIRE_SYMBOL(api, destroy, "turbo_agent_speech_whisper_destroy");
  REQUIRE_SYMBOL(api, hold, "speech_test_fake_hold");
  REQUIRE_SYMBOL(api, entered, "speech_test_fake_entered");
  REQUIRE_SYMBOL(api, decodes, "speech_test_fake_total_decodes");
  REQUIRE_SYMBOL(api, frees, "speech_test_fake_total_frees");
  return 0;
}
static int unload_image(const char *path, image_api_t *api) {
  void *still;
  if (!api->image || dlclose(api->image) != 0) return -1;
  api->image = NULL;
  /* Provider image must actually unload; no pending native callback or
   * escaped lease can keep it mapped after final settlement. */
  still = dlopen(path, RTLD_NOLOAD | RTLD_NOW);
  if (still) {
    (void)dlclose(still);
    return -1;
  }
  return 0;
}

typedef struct request_abort_s {
  image_api_t *api;
  turbo_agent_speech_whisper_t *owner;
  atomic_int fired;
} request_abort_t;
static void *abort_when_running(void *arg) {
  request_abort_t *req = (request_abort_t *)arg;
  for (int n = 0; n < 2000000; n++) {
    if (req->api->entered()) {
      atomic_store_explicit(&req->fired,
                            req->api->abort_decode(req->owner) == 0 ? 1 : -1,
                            memory_order_release);
      return NULL;
    }
    sched_yield();
  }
  atomic_store_explicit(&req->fired, -1, memory_order_release);
  return NULL;
}

static int run_one(const char *path, int flavor) {
  image_api_t api;
  turbo_agent_speech_whisper_config_t model = {0};
  turbo_agent_speech_whisper_t *owner = NULL;
  turbo_agent_speech_provider_v1_t provider;
  turbo_agent_speech_input_t *input = NULL;
  turbo_agent_speech_config_t input_config;
  char fixture[] = "/tmp/turboagent-speech-verified-XXXXXX";
  static const char model_blob[] = "MOCK_SIGNED_SPEECH_MODEL_ONLY";
  uint8_t sha[32];
  int fd;
  size_t count = 0, length = 0;
  char transcript[80];
  turbo_agent_speech_transcript_kind_t kind;
  int16_t pcm[320] = {0};
  pthread_t worker;
  request_abort_t req = {0};

  ASSERT(load_image(path, &api) == 0);
  ASSERT(api.decodes() == 0 && api.frees() == 0);
  fd = mkstemp(fixture);
  ASSERT(fd >= 0);
  ASSERT(write(fd, model_blob, sizeof(model_blob)) ==
         (ssize_t)sizeof(model_blob));
  ASSERT(close(fd) == 0);
  ASSERT(cmeta_sha256(model_blob, sizeof(model_blob), sha) == 0);
  api.config_init(&model);
  model.model_path = fixture;
  memcpy(model.expected_sha256, sha, sizeof(sha));
  model.max_model_bytes = 1024;
  model.max_utterance_frames = 10;
  model.max_text_bytes = 64;
  model.cpu_threads = 1;
  model.max_decode_ms = flavor == 2 ? 1u : 30000u;
  model.language = TURBO_AGENT_WHISPER_LANGUAGE_EN;
  ASSERT(api.create(&model, &owner) == 0 && owner);
  ASSERT(unlink(fixture) == 0); /* decoder uses only hashed memory snapshot */
  ASSERT(api.provider(owner, &provider) == 0);
  turbo_agent_speech_config_init(&input_config);
  input_config.max_utterance_frames = 10;
  input_config.max_text_bytes = 64;
  ASSERT(turbo_agent_speech_create(&input_config, &provider, &input) == 0);
  ASSERT(turbo_agent_speech_begin(input, 55u) == 0);
  ASSERT(api.destroy(&owner) == TURBO_AGENT_SPEECH_EBUSY && owner != NULL);
  pcm[0] = 16384;
  ASSERT(turbo_agent_speech_offer_frame(input, 55u, pcm, 320, 20000u) == 0);
  ASSERT(turbo_agent_speech_request_finish(input) == 0);
  ASSERT(turbo_agent_speech_pump(input, 1u, &count) == 0 && count == 1u);
  api.hold(flavor != 0);
  req.api = &api;
  req.owner = owner;
  atomic_init(&req.fired, 0);
  if (flavor == 1) ASSERT(pthread_create(&worker, NULL, abort_when_running, &req) == 0);

  if (flavor == 0) {
    ASSERT(turbo_agent_speech_pump(input, 1u, &count) == 0);
    ASSERT(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FINAL);
    ASSERT(turbo_agent_speech_take_final(input, transcript, sizeof(transcript),
                                         &length, &kind) == 0);
    ASSERT(kind == TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL);
    ASSERT(length == strlen(" utterance") &&
           strcmp(transcript, " utterance") == 0);
  } else {
    ASSERT(turbo_agent_speech_pump(input, 1u, &count) ==
           TURBO_AGENT_SPEECH_EPROVIDER);
    ASSERT(turbo_agent_speech_get_state(input) == TURBO_AGENT_SPEECH_FAILED);
    ASSERT(turbo_agent_speech_take_final(input, transcript, sizeof(transcript),
                                         &length, &kind) ==
           TURBO_AGENT_SPEECH_ESTATE);
  }
  if (flavor == 1) {
    ASSERT(pthread_join(worker, NULL) == 0);
    ASSERT(atomic_load_explicit(&req.fired, memory_order_acquire) == 1);
  }
  ASSERT(api.entered() == 1 && api.decodes() == 1);
  ASSERT(turbo_agent_speech_destroy(&input) == 0 && !input);
  ASSERT(api.destroy(&owner) == 0 && !owner);
  ASSERT(api.frees() == 1); /* exactly one model owner settled */
  ASSERT(unload_image(path, &api) == 0);
  return 0;
}

int main(int argc, char **argv) {
  ASSERT(argc == 2 && argv[1][0] == '/');
  for (int n = 0; n < 20; n++) {
    ASSERT(run_one(argv[1], 0) == 0); /* natural FINAL */
    ASSERT(run_one(argv[1], 1) == 0); /* explicit concurrent abort */
    ASSERT(run_one(argv[1], 2) == 0); /* deadline abort without worker */
  }
  puts("PASS: 60 independent Provider DSO open/setttle/unload generations");
  return 0;
}
