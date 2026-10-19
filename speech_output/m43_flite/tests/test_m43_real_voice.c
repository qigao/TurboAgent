#define _POSIX_C_SOURCE 200809L
#include "turbo_agent_tts_flite_dso.h"
#include "turbo_agent_voice_coordinator.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* MANUAL, EXPLICITLY LICENSE-AUTHORIZED CPU acceptance only. It composes
 * actual optional Flite executable + real dlopen Provider + actual M4.2
 * VoiceCoordinator/Playback Bridge C11 production sources against the exact
 * installed published Salts Playback header. The device functions below
 * are a TEST-ONLY bounded sink; no OS speaker, mic or model downloads. */
#ifndef TEST_M43_REAL_HELPER_PATH
#error "Pinned exact real Flite helper must be supplied"
#endif
#ifndef TEST_M43_DSO_PATH
#error "Exact source-built DSO must be supplied"
#endif
#ifndef M43_APPROVED_PCM_SAMPLES
#error "Expected real-voice sample count must be reviewed and pinned"
#endif
#ifndef M43_APPROVED_PCM_FNV64
#error "Expected real-voice PCM fingerprint must be reviewed and pinned"
#endif
#define VERIFY(x) do { if(!(x)) { \
  fprintf(stderr,"M4.3 REAL FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); \
  return 1; } } while(0)

struct salts_playback {
  uint8_t data[1920]; /* exactly two 20 ms PCM blocks; bounded */
  size_t pending;
  size_t consumed_samples;
  size_t nonzero_samples;
  unsigned writes, clear_calls;
  uint64_t pcm_hash;
};

int salts_playback_write(salts_playback_t *handle,const void *pcm,size_t bytes,
                         size_t *out_written) {
  if(!out_written)return SALTS_PLAYBACK_ERR_FORMAT;
  *out_written=0u;
  if(!handle || !pcm || (bytes % sizeof(int16_t)))
    return SALTS_PLAYBACK_ERR_FORMAT;
  struct salts_playback *p=(struct salts_playback*)handle;
  p->writes++;
  size_t available=sizeof(p->data)-p->pending;
  size_t n=bytes<available?bytes:available;
  n-=n%sizeof(int16_t);
  memcpy(p->data+p->pending,pcm,n);
  p->pending+=n;
  *out_written=n;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_clear(salts_playback_t *handle) {
  if(!handle) return SALTS_PLAYBACK_ERR_FORMAT;
  struct salts_playback *p=(struct salts_playback*)handle;
  p->clear_calls++;
  memset(p->data,0,sizeof(p->data));
  p->pending=0u;
  return SALTS_PLAYBACK_OK;
}
int salts_playback_drain(salts_playback_t *handle,uint32_t ms) {
  if(!handle || ms==0u) return SALTS_PLAYBACK_ERR_FORMAT;
  return ((struct salts_playback*)handle)->pending==0u ?
      SALTS_PLAYBACK_OK : SALTS_PLAYBACK_ERR_TIMEOUT;
}
/* The fake playback callback consumes only the host-written, bounded ring.
 * No Provider/Voice/Agent work takes place in any device callback. */
static void consume(struct salts_playback *p) {
  for(size_t i=0;i<p->pending;i++) {
    p->pcm_hash^=p->data[i];
    p->pcm_hash*=UINT64_C(1099511628211);
  }
  for(size_t i=0;i<p->pending;i+=2u) {
    uint16_t sample=(uint16_t)((uint16_t)p->data[i] |
                    ((uint16_t)p->data[i+1u]<<8u));
    if(sample!=0u) p->nonzero_samples++;
  }
  p->consumed_samples+=p->pending/2u;
  memset(p->data,0,sizeof(p->data));
  p->pending=0u;
}
static uint64_t now_ms(void) {
  struct timespec t={0};
  (void)clock_gettime(CLOCK_MONOTONIC,&t);
  return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}
static void nap_ms(unsigned ms) {
  struct timespec t={(time_t)(ms/1000u),(long)(ms%1000u)*1000000L};
  (void)nanosleep(&t,NULL);
}

int main(void) {
  unsigned unloads=0u;
  void *library=dlopen(TEST_M43_DSO_PATH,RTLD_NOW|RTLD_LOCAL);
  VERIFY(library!=NULL);
  void *symbol=dlsym(library,"turbo_m43_flite_dso_entry");
  VERIFY(symbol!=NULL);
  turbo_m43_flite_dso_entry_fn entry=NULL;
  _Static_assert(sizeof(entry)==sizeof(symbol),
                 "POSIX dlsym pointer representation mismatch");
  memcpy(&entry,&symbol,sizeof(entry));
  VERIFY(entry!=NULL);
  const turbo_m43_flite_dso_api_t *api=entry();
  VERIFY(api && api->struct_size==sizeof(*api) &&
         api->abi_version==TURBO_M43_FLITE_DSO_ABI_VERSION);
  api->test_set_unload_counter(&unloads);

  turbo_m43_flite_config_t cfg;
  api->config_init(&cfg);
  cfg.helper_absolute_path=TEST_M43_REAL_HELPER_PATH;
  cfg.max_wall_ms=20000u;
  turbo_m43_flite_process_t *ctx=NULL;
  VERIFY(api->create(&cfg,&ctx)==TURBO_TTS_OK && ctx!=NULL);
  turbo_tts_provider_t provider;
  VERIFY(api->bind(ctx,&provider)==TURBO_TTS_OK);

  turbo_tts_config_t outcfg;
  turbo_tts_config_init(&outcfg);
  outcfg.max_text_bytes=128u;
  turbo_tts_output_t *tts=NULL;
  VERIFY(turbo_tts_create(&outcfg,&provider,&tts)==TURBO_TTS_OK);
  struct salts_playback device={0};
  device.pcm_hash=UINT64_C(14695981039346656037);
  turbo_voice_coordinator_t *voice=NULL;
  VERIFY(turbo_voice_create(tts,(salts_playback_t*)&device,&voice)==TURBO_VOICE_OK);

  const char *approved="Hello world.";
  VERIFY(turbo_voice_begin_input(voice,42u)==TURBO_VOICE_OK);
  VERIFY(turbo_voice_asr_event(voice,42u,TURBO_VOICE_ASR_PARTIAL)==
         TURBO_VOICE_IGNORED);
  VERIFY(turbo_voice_asr_event(voice,42u,TURBO_VOICE_ASR_CHECKED_FINAL)==
         TURBO_VOICE_OK);
  VERIFY(turbo_voice_input_quiesced(voice,42u)==TURBO_VOICE_OK);
  VERIFY(turbo_voice_approved_final(voice,42u,approved,strlen(approved),
          TURBO_M43_FLITE_VOICE_ID,TURBO_M43_FLITE_LANGUAGE)==TURBO_VOICE_OK);
  VERIFY(api->destroy(&ctx)==TURBO_TTS_EBUSY && unloads==0u);

  uint64_t deadline=now_ms()+25000u;
  while(now_ms()<deadline) {
    turbo_voice_state_t state=turbo_voice_get_state(voice);
    if(state==TURBO_VOICE_COMPLETED) break;
    VERIFY(state==TURBO_VOICE_SPEAKING ||
           state==TURBO_VOICE_DRAIN_PENDING);
    if(state==TURBO_VOICE_SPEAKING) {
      size_t written=0u;
      VERIFY(turbo_voice_pump(voice,2u,&written)==TURBO_VOICE_OK);
    }
    consume(&device); /* Simulate actual native output callback consuming PCM */
    if(turbo_voice_get_state(voice)==TURBO_VOICE_DRAIN_PENDING) {
      VERIFY(turbo_voice_drain(voice,10u)==TURBO_VOICE_OK);
    }
    nap_ms(2u);
  }
  VERIFY(turbo_voice_get_state(voice)==TURBO_VOICE_COMPLETED);
  VERIFY(device.clear_calls==0u && device.writes>0u);
  VERIFY(device.consumed_samples==(size_t)M43_APPROVED_PCM_SAMPLES);
  VERIFY(device.nonzero_samples>0u);
  VERIFY(device.pcm_hash==(uint64_t)M43_APPROVED_PCM_FNV64);
  VERIFY(unloads==0u);

  VERIFY(turbo_voice_destroy(&voice)==TURBO_VOICE_OK);
  VERIFY(turbo_tts_destroy(&tts)==TURBO_TTS_OK);
  VERIFY(api->destroy(&ctx)==TURBO_TTS_OK);
  VERIFY(dlclose(library)==0);
  VERIFY(unloads==1u);
  printf("PASS M4.3 manual REAL Flite: %zu PCM samples, hash %016llx,"
         " true DSO settle/unload; playback was a test sink only\n",
         device.consumed_samples,(unsigned long long)device.pcm_hash);
  return 0;
}
