#define _POSIX_C_SOURCE 200809L
#include "turbo_agent_tts_flite_process.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef TEST_M43_HELPER_PATH
#error "Exact built helper path is required; never search PATH"
#endif
#define MUST(x) do { if(!(x)) {fprintf(stderr,"M4.3 fail at %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;} }while(0)

static void nap_ms(long millis) {
  struct timespec t={millis/1000,(millis%1000)*1000000L};
  (void)nanosleep(&t,NULL);
}
static int setup(turbo_m43_flite_process_t **ctx,turbo_tts_output_t **tts,
                 uint32_t max_wall) {
  turbo_m43_flite_config_t fc;
  turbo_m43_flite_config_init(&fc);
  fc.helper_absolute_path=TEST_M43_HELPER_PATH;
  fc.max_wall_ms=max_wall;
  fc.cancel_grace_ms=40u;
  if(turbo_m43_flite_create(&fc,ctx)!=TURBO_TTS_OK)return 1;
  turbo_tts_provider_t p;
  if(turbo_m43_flite_bind(*ctx,&p)!=TURBO_TTS_OK)return 1;
  turbo_tts_config_t tc;
  turbo_tts_config_init(&tc);
  tc.max_text_bytes=128u;
  return turbo_tts_create(&tc,&p,tts)!=TURBO_TTS_OK;
}
static int start(turbo_tts_output_t *t,uint64_t gen,const char *text) {
  return turbo_tts_begin(t,gen,text,strlen(text),
                         TURBO_M43_FLITE_VOICE_ID,TURBO_M43_FLITE_LANGUAGE);
}
static int settle(turbo_tts_output_t *t,
                  turbo_tts_state_t expected,size_t *pcm_frames) {
  for(unsigned i=0;i<2200u;i++) {
    int16_t pcm[480]={0};
    size_t n=0;
    turbo_tts_poll_t kind=TURBO_TTS_WOULD_BLOCK;
    int r=turbo_tts_poll(t,pcm,480u,&n,&kind);
    if(r==TURBO_TTS_EPROVIDER && expected==TURBO_TTS_FAILED) {
      nap_ms(2); continue;
    }
    if(r!=TURBO_TTS_OK) return 1;
    if(kind==TURBO_TTS_PCM) {
      if(!pcm_frames) return 1;
      ++*pcm_frames;
    }
    if(turbo_tts_state(t)==expected) return 0;
    nap_ms(2);
  }
  return 1;
}
static int run_normal(void) {
  turbo_m43_flite_process_t *ctx=NULL;
  turbo_tts_output_t *tts=NULL;
  MUST(setup(&ctx,&tts,3000u)==0);
  MUST(start(tts,42u,"normal")==TURBO_TTS_OK);
  unsigned frames=0;
  for(unsigned i=0;i<1200u;i++) {
    int16_t pcm[480]={0};
    size_t n=0;
    turbo_tts_poll_t kind=TURBO_TTS_WOULD_BLOCK;
    MUST(turbo_tts_poll(tts,pcm,480u,&n,&kind)==TURBO_TTS_OK);
    if(kind==TURBO_TTS_PCM) {
      if(frames==0u) {
        MUST(n==480u && pcm[0]==1000 && pcm[199]==1199);
      } else if(frames==1u) {
        MUST(n==120u && pcm[0]==-300 && pcm[119]==-419);
      } else return 1;
      frames++;
    }
    if(turbo_tts_state(tts)==TURBO_TTS_SOURCE_DONE) break;
    nap_ms(2);
  }
  MUST(frames==2u && turbo_tts_state(tts)==TURBO_TTS_SOURCE_DONE);
  MUST(start(tts,42u,"normal")==TURBO_TTS_ESTALE);
  MUST(turbo_tts_destroy(&tts)==TURBO_TTS_OK);
  MUST(turbo_m43_flite_destroy(&ctx)==TURBO_TTS_OK);
  puts("PASS M4.3 process: 480+120 PCM, exact order, terminal/reap, stale refused");
  return 0;
}
static int run_cancel(const char *text,int after_first_pcm) {
  turbo_m43_flite_process_t *ctx=NULL;
  turbo_tts_output_t *tts=NULL;
  MUST(setup(&ctx,&tts,2500u)==0);
  MUST(start(tts,53u,text)==TURBO_TTS_OK);
  if(after_first_pcm) {
    int saw=0;
    for(unsigned i=0;i<800u;i++) {
      int16_t pcm[480]={0};
      size_t n=0;
      turbo_tts_poll_t kind=TURBO_TTS_WOULD_BLOCK;
      MUST(turbo_tts_poll(tts,pcm,480u,&n,&kind)==TURBO_TTS_OK);
      if(kind==TURBO_TTS_PCM) {
        MUST(n==480u && pcm[0]==1000);
        saw=1;break;
      }
      nap_ms(2);
    }
    MUST(saw);
  }
  nap_ms(25);
  MUST(turbo_tts_cancel(tts)==TURBO_TTS_OK);
  MUST(turbo_tts_destroy(&tts)==TURBO_TTS_EBUSY);
  MUST(turbo_m43_flite_destroy(&ctx)==TURBO_TTS_EBUSY);
  MUST(settle(tts,TURBO_TTS_CANCELED,NULL)==0);
  MUST(turbo_tts_destroy(&tts)==TURBO_TTS_OK);
  MUST(turbo_m43_flite_destroy(&ctx)==TURBO_TTS_OK);
  printf("PASS M4.3 process: %s cancel settles/reaps, no PCM after cancel\n",text);
  return 0;
}
static int run_fault(const char *text) {
  turbo_m43_flite_process_t *ctx=NULL;
  turbo_tts_output_t *tts=NULL;
  MUST(setup(&ctx,&tts,2200u)==0);
  MUST(start(tts,64u,text)==TURBO_TTS_OK);
  MUST(settle(tts,TURBO_TTS_FAILED,NULL)==0);
  MUST(turbo_tts_destroy(&tts)==TURBO_TTS_OK);
  MUST(turbo_m43_flite_destroy(&ctx)==TURBO_TTS_OK);
  printf("PASS M4.3 process: %s fails closed, reaps child\n",text);
  return 0;
}
static int run_rejections(void) {
  turbo_m43_flite_config_t c;
  turbo_m43_flite_config_init(&c);
  c.helper_absolute_path="/definitely/not/a/tts/helper";
  turbo_m43_flite_process_t *ctx=NULL;
  MUST(turbo_m43_flite_create(&c,&ctx)==TURBO_TTS_EINVAL && ctx==NULL);
  c.helper_absolute_path=TEST_M43_HELPER_PATH;
  c.max_text_bytes=129u;
  MUST(turbo_m43_flite_create(&c,&ctx)==TURBO_TTS_EINVAL);
  c.max_text_bytes=128u;
  MUST(turbo_m43_flite_create(&c,&ctx)==TURBO_TTS_OK);
  turbo_tts_provider_t p;
  MUST(turbo_m43_flite_bind(ctx,&p)==TURBO_TTS_OK);
  turbo_tts_config_t conf;
  turbo_tts_config_init(&conf);
  conf.max_text_bytes=128u;
  turbo_tts_output_t *tts=NULL;
  MUST(turbo_tts_create(&conf,&p,&tts)==TURBO_TTS_OK);
  MUST(turbo_tts_begin(tts,9u,"normal",6u,"not-real","en")==
       TURBO_TTS_EPROVIDER);
  MUST(settle(tts,TURBO_TTS_FAILED,NULL)==0);
  MUST(turbo_tts_destroy(&tts)==TURBO_TTS_OK);
  MUST(turbo_m43_flite_destroy(&ctx)==TURBO_TTS_OK);
  puts("PASS M4.3 process: exact helper/voice/format and bounds fail closed");
  return 0;
}
int main(void) {
  MUST(run_rejections()==0);
  MUST(run_normal()==0);
  MUST(run_cancel("hang",0)==0);
  MUST(run_cancel("ignore",0)==0);
  MUST(run_cancel("cancel",1)==0);
  MUST(run_fault("bad")==0);
  MUST(run_fault("die")==0);
  MUST(run_fault("hang")==0);
  puts("PASS M4.3 all process Provider ownership gates");
  return 0;
}
