/* Test-only C11 Whisper v1.9.5 ABI emulator.
 * This is NOT acoustic inference or an alternative production engine.
 * It is linked *inside* the separately dlopen()'d Provider image so real
 * callback/owner leases, cross-image M1 ABI and dlclose() can be exercised. */
#include <whisper.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "The test-only dynamic loader fixture requires an ELF toolchain"
#endif
#define TEST_EXPORT __attribute__((visibility("default")))

struct whisper_context {
  unsigned decoded;
  float first_float;
};
static atomic_int fake_hold = ATOMIC_VAR_INIT(0);
static atomic_int fake_entered = ATOMIC_VAR_INIT(0);
static atomic_int fake_decode_count = ATOMIC_VAR_INIT(0);
static atomic_int fake_context_freed = ATOMIC_VAR_INIT(0);

TEST_EXPORT void speech_test_fake_hold(int value) {
  atomic_store_explicit(&fake_hold, value, memory_order_release);
  atomic_store_explicit(&fake_entered, 0, memory_order_release);
}
TEST_EXPORT int speech_test_fake_entered(void) {
  return atomic_load_explicit(&fake_entered, memory_order_acquire);
}
TEST_EXPORT int speech_test_fake_total_decodes(void) {
  return atomic_load_explicit(&fake_decode_count, memory_order_acquire);
}
TEST_EXPORT int speech_test_fake_total_frees(void) {
  return atomic_load_explicit(&fake_context_freed, memory_order_acquire);
}

struct whisper_context_params whisper_context_default_params(void) {
  struct whisper_context_params params = {0};
  return params;
}
struct whisper_context *whisper_init_from_buffer_with_params(
    void *bytes, size_t length, struct whisper_context_params params) {
  if (!bytes || length < 16u ||
      ((const unsigned char *)bytes)[0] != (unsigned char)'M' ||
      params.use_gpu) return NULL;
  return (struct whisper_context *)calloc(1u, sizeof(struct whisper_context));
}
void whisper_free(struct whisper_context *ctx) {
  if (!ctx) return;
  free(ctx);
  atomic_fetch_add_explicit(&fake_context_freed, 1, memory_order_relaxed);
}
struct whisper_full_params whisper_full_default_params(
    enum whisper_sampling_strategy sampling) {
  struct whisper_full_params result = {0};
  result.strategy = sampling;
  return result;
}
int whisper_full(struct whisper_context *ctx, struct whisper_full_params params,
                 const float *samples, int count) {
  if (!ctx || !samples || count < 320 || !params.abort_callback ||
      params.n_threads < 1 || params.translate || !params.no_context ||
      params.vad || !params.no_timestamps || !params.language) return -1;
  ctx->first_float = samples[0];
  ctx->decoded++;
  atomic_fetch_add_explicit(&fake_decode_count, 1, memory_order_relaxed);
  atomic_store_explicit(&fake_entered, 1, memory_order_release);
  for (;;) {
    if (params.abort_callback(params.abort_callback_user_data)) return -1;
    if (!atomic_load_explicit(&fake_hold, memory_order_acquire)) break;
    sched_yield();
  }
  return 0;
}
int whisper_full_n_segments(struct whisper_context *ctx) {
  return ctx && ctx->decoded ? 1 : 0;
}
const char *whisper_full_get_segment_text(struct whisper_context *ctx,
                                           int index) {
  return ctx && ctx->decoded && index == 0 ? " utterance" : NULL;
}
