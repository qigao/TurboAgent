#include "turbo_agent_speech_whisper.h"
#include <cmeta_crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define ta_getcwd _getcwd
#else
#include <unistd.h>
#define ta_getcwd getcwd
#endif

#define CHECK(expr) do { if (!(expr)) { \
  fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr); \
  return 1; \
} } while (0)

static int test_invalid_model_and_digest(void) {
  turbo_agent_speech_whisper_config_t c;
  turbo_agent_speech_whisper_t *owner = NULL;
  turbo_agent_speech_provider_v1_t p;
  unsigned char bytes[64] = {0};
  char cwd[4096], path[4200];
  FILE *file;
  const char *fake = "not-a-valid-ggml-model-binary-for-negative-tests";
  const size_t fake_len = strlen(fake);
  CHECK(ta_getcwd(cwd, sizeof(cwd)) != NULL);
  CHECK(snprintf(path, sizeof(path), "%s/%s", cwd, "ta-invalid-model.bin") > 0);
  file = fopen(path, "wb");
  CHECK(file != NULL);
  CHECK(fwrite(fake, 1, fake_len, file) == fake_len);
  CHECK(fclose(file) == 0);
  CHECK(cmeta_sha256(fake, fake_len, bytes) == 0);
  turbo_agent_speech_whisper_config_init(&c);
  CHECK(c.struct_size == sizeof(c) &&
        c.abi_version == TURBO_AGENT_SPEECH_WHISPER_ABI_VERSION);
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_SPEECH_EINVAL && owner == NULL);
  c.model_path = "relative-model.bin";
  memcpy(c.expected_sha256, bytes, 32u);
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_SPEECH_EINVAL && owner == NULL);
  c.model_path = path;
  c.abi_version = 999u;
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_SPEECH_EINVAL);
  c.abi_version = TURBO_AGENT_SPEECH_WHISPER_ABI_VERSION;
  c.cpu_threads = 0u;
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_SPEECH_EINVAL);
  c.cpu_threads = 2u;
  c.max_utterance_frames = TURBO_AGENT_SPEECH_MAX_UTTERANCE_FRAMES + 1u;
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_SPEECH_EINVAL);
  c.max_utterance_frames = 5u;
  c.max_model_bytes = 3u;
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_WHISPER_EMODEL && owner == NULL);
  c.max_model_bytes = 65536u;
  c.expected_sha256[0] ^= 1u;
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_WHISPER_EINTEGRITY && owner == NULL);
  c.expected_sha256[0] ^= 1u;
  /* The hash passes, but the actual v1.9.5 engine must reject corrupt
   * header/weights rather than silently choosing a model or algorithm. */
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_WHISPER_EMODEL && owner == NULL);
  c.model_path = "/this/path/must/not/exist/audio-model.bin";
  CHECK(turbo_agent_speech_whisper_create(&c, &owner) ==
        TURBO_AGENT_WHISPER_EMODEL && owner == NULL);
  CHECK(turbo_agent_speech_whisper_destroy(&owner) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(turbo_agent_speech_whisper_request_abort(NULL) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(turbo_agent_speech_whisper_last_error(NULL) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(turbo_agent_speech_whisper_provider(NULL, &p) ==
        TURBO_AGENT_SPEECH_EINVAL);
  CHECK(remove(path) == 0);
  puts("Whisper actual backend invalid-model / SHA256 / version gate PASS");
  return 0;
}

int main(void) {
  CHECK(test_invalid_model_and_digest() == 0);
  return 0;
}
