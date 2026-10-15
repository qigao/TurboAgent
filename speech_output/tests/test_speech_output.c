#include "turbo_agent_speech_output.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {unsigned step,canceled,started;uint64_t generation;} fake_t;
static int begin(void *ctx,uint64_t id,const char *text,size_t len){
 fake_t *f=ctx;if(!id||!len||!text)return -1;
 f->generation=id;f->started++;f->step=0;return 0;
}
static int poll(void *ctx,uint64_t id,int16_t *pcm,size_t cap,size_t *n,turbo_tts_poll_t *k){
 fake_t *f=ctx;if(id!=f->generation||cap!=480)return -1;
 if(f->step==0){for(size_t i=0;i<480;i++)pcm[i]=(int16_t)(i&1?1000:-1000);*n=480;*k=TURBO_TTS_PCM;}
 else if(f->step==1){for(size_t i=0;i<80;i++)pcm[i]=(int16_t)i;*n=80;*k=TURBO_TTS_PCM;}
 else {*n=0;*k=TURBO_TTS_TERMINAL;}
 f->step++;return 0;
}
static void cancel(void *ctx,uint64_t id){fake_t *f=ctx;if(id==f->generation)f->canceled++;}
#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL line %d\n",__LINE__);return 1;}}while(0)
int main(void){
 fake_t f={0};turbo_tts_provider_t provider;turbo_tts_output_t *o=NULL;
 int16_t pcm[480]={0};size_t n=0;turbo_tts_poll_t kind=TURBO_TTS_WOULD_BLOCK;
 turbo_tts_provider_init(&provider);
 provider.context=&f;provider.begin=begin;provider.poll=poll;provider.cancel=cancel;
 CHECK(turbo_tts_create(&provider,&o)==0);
 CHECK(turbo_tts_begin(o,1,"Hello",5)==0);
 CHECK(turbo_tts_destroy(&o)==-2&&o);
 CHECK(turbo_tts_poll(o,pcm,480,&n,&kind)==0&&n==480&&kind==TURBO_TTS_PCM&&pcm[0]==-1000);
 CHECK(turbo_tts_poll(o,pcm,480,&n,&kind)==0&&n==80&&kind==TURBO_TTS_PCM&&pcm[79]==79);
 CHECK(turbo_tts_poll(o,pcm,480,&n,&kind)==0&&n==0&&kind==TURBO_TTS_TERMINAL);
 CHECK(turbo_tts_state(o)==TURBO_TTS_DONE);
 CHECK(turbo_tts_begin(o,1,"Same",4)!=0);
 CHECK(turbo_tts_begin(o,2,"\x1b",1)!=0);
 CHECK(turbo_tts_begin(o,2,"\xc0\xaf",2)!=0);
 CHECK(turbo_tts_begin(o,2,"Second",6)==0);
 CHECK(turbo_tts_cancel(o)==0&&turbo_tts_cancel(o)==0);
 CHECK(f.canceled==1&&turbo_tts_state(o)==TURBO_TTS_CANCELED);
 CHECK(turbo_tts_begin(o,3,"Third",5)==0);
 CHECK(turbo_tts_poll(o,pcm,1,&n,&kind)!=0);
 CHECK(turbo_tts_cancel(o)==0);
 CHECK(turbo_tts_destroy(&o)==0&&o==NULL);
 CHECK(f.canceled==2&&f.started==3);
 puts("PASS: bounded C11 TTS fake PCM, terminal, stale generations, cancel, UTF-8");
 return 0;
}
