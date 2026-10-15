#define _POSIX_C_SOURCE 200809L
/* Strict C11 fake Whisper engine. Built against the *actual pinned v1.9.5
 * public whisper.h*, but not linked to the upstream implementation. This
 * qualifies the Provider v1 PCM/abort/result/owner interface with real
 * deterministic speech bytes and no remote or licensed model dependency.
 * A separate job builds/links/runs the real upstream Whisper C API. */
#include "turbo_agent_speech_whisper.h"
#include <cmeta_crypto.h>
#include <whisper.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <math.h>
#include <time.h>

#define REQUIRE(x) do { if (!(x)) { \
  fprintf(stderr,"%s:%d failed %s\n",__FILE__,__LINE__,#x); return 1; \
} } while (0)

struct whisper_context {
  unsigned runs;
  float first_sample;
};
static atomic_int st_decode_entered;
static atomic_int st_hold_until_abort;
static atomic_int st_engine_skips_abort_poll;
static atomic_int st_decode_calls;
static atomic_int st_frees;
static struct whisper_context *st_last_ctx;

struct whisper_context_params whisper_context_default_params(void) {
  struct whisper_context_params params = {0};
  return params;
}
struct whisper_context *whisper_init_from_buffer_with_params(
    void *buffer, size_t length, struct whisper_context_params params) {
  struct whisper_context *ctx;
  if (!buffer || length < 8 || ((uint8_t *)buffer)[0] != 'M' ||
      params.use_gpu) return NULL;
  ctx = (struct whisper_context *)calloc(1, sizeof(*ctx));
  st_last_ctx = ctx;
  return ctx;
}
void whisper_free(struct whisper_context *ctx) {
  if (!ctx) return;
  atomic_fetch_add(&st_frees, 1);
  free(ctx);
  st_last_ctx = NULL;
}
struct whisper_full_params whisper_full_default_params(
    enum whisper_sampling_strategy strategy) {
  struct whisper_full_params params = {0};
  params.strategy = strategy;
  return params;
}
int whisper_full(struct whisper_context *ctx,
                 struct whisper_full_params params,
                 const float *samples, int count) {
  if (!ctx || !samples || count <= 0 || params.n_threads == 0 ||
      params.translate || !params.no_context ||
      !params.no_timestamps || params.vad ||
      !params.abort_callback || !params.language) return -1;
  ctx->first_sample = samples[0];
  ctx->runs++;
  atomic_fetch_add(&st_decode_calls, 1);
  atomic_store(&st_decode_entered, 1);
  if (atomic_load(&st_engine_skips_abort_poll)) {
    /* Simulate a third-party decoder that ignores its last abort callback
     * and incorrectly returns success after the configured time budget. */
    struct timespec pause = { .tv_sec = 0, .tv_nsec = 8000000L };
    (void)nanosleep(&pause, NULL);
    return 0;
  }
  if (atomic_load(&st_hold_until_abort)) {
    for (int i=0;i<300000;i++) {
      if (params.abort_callback(params.abort_callback_user_data))
        return -1;
      sched_yield();
    }
    return -1;
  }
  return params.abort_callback(params.abort_callback_user_data) ? -1 : 0;
}
int whisper_full_n_segments(struct whisper_context *ctx) {
  return ctx && ctx->runs ? 1 : 0;
}
const char *whisper_full_get_segment_text(struct whisper_context *ctx, int index) {
  return ctx && ctx->runs && index == 0 ? " recognized" : NULL;
}

static void temp_model_path(char *dst, size_t cap) {
  char cwd[2048];
  if (!getcwd(cwd, sizeof(cwd))) abort();
  if (snprintf(dst, cap, "%s/speech-mock-engine.bin", cwd) <= 0) abort();
}
static int create_owner(turbo_agent_speech_whisper_t **owner,
                        uint32_t decode_ms) {
  static const char blob[] = "MOCK_C11_VALIDATED_MODEL";
  turbo_agent_speech_whisper_config_t config;
  unsigned char digest[32];
  char path[2600];
  FILE *file;
  temp_model_path(path, sizeof(path));
  file=fopen(path,"wb");
  if (!file) return -1;
  if (fwrite(blob,1,sizeof(blob),file)!=sizeof(blob)) {
    fclose(file);
    return -1;
  }
  fclose(file);
  if (cmeta_sha256(blob,sizeof(blob),digest)!=0) return -1;
  turbo_agent_speech_whisper_config_init(&config);
  config.model_path=path;
  config.max_model_bytes=1024;
  config.max_utterance_frames=10;
  config.max_text_bytes=100;
  config.language=TURBO_AGENT_WHISPER_LANGUAGE_JA;
  config.cpu_threads=1;
  config.max_decode_ms=decode_ms;
  memcpy(config.expected_sha256,digest,sizeof(digest));
  int status=turbo_agent_speech_whisper_create(&config,owner);
  remove(path);
  return status;
}

static int transcript_cycle(void) {
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t api;
  turbo_agent_speech_input_t *m1=NULL;
  turbo_agent_speech_config_t options;
  char transcript[100];
  size_t pumped=0, length=0;
  turbo_agent_speech_transcript_kind_t kind;
  int16_t pcm[320]={0};
  pcm[0]=16384;
  REQUIRE(create_owner(&owner, 30000u)==0 && owner!=NULL);
  REQUIRE(turbo_agent_speech_whisper_provider(owner,&api)==0);
  REQUIRE(api.struct_size==sizeof(api) && api.abi_version==1);
  turbo_agent_speech_config_init(&options);
  options.max_utterance_frames=10;
  options.max_text_bytes=100;
  REQUIRE(turbo_agent_speech_create(&options,&api,&m1)==0);
  REQUIRE(turbo_agent_speech_begin(m1,11)==0);
  REQUIRE(turbo_agent_speech_offer_frame(m1,11,pcm,320,10000)==0);
  REQUIRE(atomic_load(&st_decode_calls)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==TURBO_AGENT_SPEECH_EBUSY);
  REQUIRE(turbo_agent_speech_request_finish(m1)==0);
  REQUIRE(turbo_agent_speech_pump(m1,1,&pumped)==0 && pumped==1);
  REQUIRE(atomic_load(&st_decode_calls)==0);
  REQUIRE(turbo_agent_speech_pump(m1,1,&pumped)==0 && pumped==0);
  REQUIRE(atomic_load(&st_decode_calls)==1);
  REQUIRE(st_last_ctx && fabsf(st_last_ctx->first_sample-0.5f)<0.0001f);
  REQUIRE(turbo_agent_speech_take_final(m1,transcript,
          sizeof(transcript),&length,&kind)==0);
  REQUIRE(kind==TURBO_AGENT_SPEECH_TRANSCRIPT_FINAL);
  REQUIRE(length==11 && strcmp(transcript," recognized")==0);
  REQUIRE(turbo_agent_speech_take_final(m1,transcript,
          sizeof(transcript),&length,&kind)==TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_destroy(&m1)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==0);
  REQUIRE(atomic_load(&st_frees)==1);
  return 0;
}

static int digital_silence(void) {
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t api;
  turbo_agent_speech_input_t *m1=NULL;
  turbo_agent_speech_config_t options;
  size_t pumped=0, length=999;
  int16_t silence[320]={0};
  char text[32];
  turbo_agent_speech_transcript_kind_t kind;
  REQUIRE(create_owner(&owner, 30000u)==0);
  REQUIRE(turbo_agent_speech_whisper_provider(owner,&api)==0);
  turbo_agent_speech_config_init(&options);
  REQUIRE(turbo_agent_speech_create(&options,&api,&m1)==0);
  REQUIRE(turbo_agent_speech_begin(m1,33)==0);
  REQUIRE(turbo_agent_speech_offer_frame(m1,33,silence,320,1000)==0);
  REQUIRE(turbo_agent_speech_request_finish(m1)==0);
  REQUIRE(turbo_agent_speech_pump(m1,5,&pumped)==0);
  REQUIRE(turbo_agent_speech_get_state(m1)==TURBO_AGENT_SPEECH_NO_SPEECH);
  REQUIRE(turbo_agent_speech_take_final(m1,text,sizeof(text),&length,&kind)==0);
  REQUIRE(kind==TURBO_AGENT_SPEECH_TRANSCRIPT_NO_SPEECH && length==0);
  REQUIRE(atomic_load(&st_decode_calls)==1); /* silence never calls Whisper */
  REQUIRE(turbo_agent_speech_destroy(&m1)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==0);
  return 0;
}

typedef struct abort_worker_s { turbo_agent_speech_whisper_t *owner; } abort_worker_t;
static void *abort_worker(void *ctx) {
  abort_worker_t *worker=(abort_worker_t *)ctx;
  for (int i=0; i<1000000; ++i) {
    if (atomic_load(&st_decode_entered)) {
      (void)turbo_agent_speech_whisper_request_abort(worker->owner);
      return NULL;
    }
    sched_yield();
  }
  return NULL;
}

static int abort_and_recover(void) {
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t api;
  turbo_agent_speech_input_t *m1=NULL;
  turbo_agent_speech_config_t options;
  pthread_t thread;
  abort_worker_t worker;
  int16_t pcm[320]={1};
  size_t pumped=0;
  REQUIRE(create_owner(&owner, 30000u)==0);
  REQUIRE(turbo_agent_speech_whisper_provider(owner,&api)==0);
  turbo_agent_speech_config_init(&options);
  REQUIRE(turbo_agent_speech_create(&options,&api,&m1)==0);
  REQUIRE(turbo_agent_speech_begin(m1,44)==0);
  REQUIRE(turbo_agent_speech_offer_frame(m1,44,pcm,320,1000)==0);
  REQUIRE(turbo_agent_speech_request_finish(m1)==0);
  atomic_store(&st_decode_entered,0);
  atomic_store(&st_hold_until_abort,1);
  worker.owner=owner;
  REQUIRE(pthread_create(&thread,NULL,abort_worker,&worker)==0);
  /* First pump consumes PCM; next pump enters actual synchronous inference. */
  REQUIRE(turbo_agent_speech_pump(m1,1,&pumped)==0);
  REQUIRE(turbo_agent_speech_pump(m1,1,&pumped)==TURBO_AGENT_SPEECH_EPROVIDER);
  REQUIRE(pthread_join(thread,NULL)==0);
  REQUIRE(turbo_agent_speech_whisper_last_error(owner)==
          TURBO_AGENT_WHISPER_EABORT);
  REQUIRE(turbo_agent_speech_get_state(m1)==TURBO_AGENT_SPEECH_FAILED);
  REQUIRE(turbo_agent_speech_destroy(&m1)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==0);
  atomic_store(&st_hold_until_abort,0);
  return 0;
}

static int monotonic_decode_deadline(void) {
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t api;
  turbo_agent_speech_input_t *m1=NULL;
  turbo_agent_speech_config_t options;
  int16_t pcm[320]={1};
  size_t pumped=0;
  REQUIRE(create_owner(&owner, 1u)==0);
  REQUIRE(turbo_agent_speech_whisper_provider(owner,&api)==0);
  turbo_agent_speech_config_init(&options);
  REQUIRE(turbo_agent_speech_create(&options,&api,&m1)==0);
  REQUIRE(turbo_agent_speech_begin(m1,55u)==0);
  REQUIRE(turbo_agent_speech_offer_frame(m1,55u,pcm,320,2000u)==0);
  REQUIRE(turbo_agent_speech_request_finish(m1)==0);
  atomic_store(&st_hold_until_abort,1);
  atomic_store(&st_decode_entered,0);
  REQUIRE(turbo_agent_speech_pump(m1,1u,&pumped)==0 && pumped==1);
  /* No worker cancellation: the Provider must independently abort after
   * its own 1ms monotonic deadline, without publishing a transcript. */
  REQUIRE(turbo_agent_speech_pump(m1,1u,&pumped)==TURBO_AGENT_SPEECH_EPROVIDER);
  REQUIRE(atomic_load(&st_decode_entered)==1);
  REQUIRE(turbo_agent_speech_whisper_last_error(owner)==TURBO_AGENT_WHISPER_EABORT);
  REQUIRE(turbo_agent_speech_get_state(m1)==TURBO_AGENT_SPEECH_FAILED);
  REQUIRE(turbo_agent_speech_destroy(&m1)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==0);
  atomic_store(&st_hold_until_abort,0);
  return 0;
}

static int successful_decode_after_deadline_must_fail(void) {
  turbo_agent_speech_whisper_t *owner=NULL;
  turbo_agent_speech_provider_v1_t api;
  turbo_agent_speech_input_t *m1=NULL;
  turbo_agent_speech_config_t options;
  size_t pumped=0, length=0;
  char transcript[32];
  turbo_agent_speech_transcript_kind_t kind;
  int16_t pcm[320]={2};
  REQUIRE(create_owner(&owner, 1u)==0);
  REQUIRE(turbo_agent_speech_whisper_provider(owner,&api)==0);
  turbo_agent_speech_config_init(&options);
  REQUIRE(turbo_agent_speech_create(&options,&api,&m1)==0);
  REQUIRE(turbo_agent_speech_begin(m1,56u)==0);
  REQUIRE(turbo_agent_speech_offer_frame(m1,56u,pcm,320,3000u)==0);
  REQUIRE(turbo_agent_speech_request_finish(m1)==0);
  atomic_store(&st_hold_until_abort,0);
  atomic_store(&st_engine_skips_abort_poll,1);
  REQUIRE(turbo_agent_speech_pump(m1,1u,&pumped)==0 && pumped==1);
  /* The fake engine intentionally returns SUCCESS *without polling* its
   * callback, after the real 1ms monotonic time budget has expired. */
  REQUIRE(turbo_agent_speech_pump(m1,1u,&pumped)==TURBO_AGENT_SPEECH_EPROVIDER);
  REQUIRE(turbo_agent_speech_whisper_last_error(owner)==TURBO_AGENT_WHISPER_EABORT);
  REQUIRE(turbo_agent_speech_get_state(m1)==TURBO_AGENT_SPEECH_FAILED);
  REQUIRE(turbo_agent_speech_take_final(m1,transcript,sizeof(transcript),
                                        &length,&kind)==TURBO_AGENT_SPEECH_ESTATE);
  REQUIRE(turbo_agent_speech_destroy(&m1)==0);
  REQUIRE(turbo_agent_speech_whisper_destroy(&owner)==0);
  atomic_store(&st_engine_skips_abort_poll,0);
  return 0;
}

int main(void) {
  atomic_init(&st_decode_entered,0);
  atomic_init(&st_hold_until_abort,0);
  atomic_init(&st_engine_skips_abort_poll,0);
  atomic_init(&st_decode_calls,0);
  atomic_init(&st_frees,0);
  REQUIRE(transcript_cycle()==0);
  REQUIRE(digital_silence()==0);
  REQUIRE(abort_and_recover()==0);
  REQUIRE(monotonic_decode_deadline()==0);
  REQUIRE(successful_decode_after_deadline_must_fail()==0);
  puts("Whisper mock C-ABI Provider PCM, no-speech, cancel, polled and unpolled deadline PASS");
  return 0;
}
