#include "turbo_agent_speech_output.h"
#include <stdlib.h>
#include <string.h>
#define MAX_TEXT 4096u
#define MAX_FRAMES 1500u
struct turbo_tts_output {
 turbo_tts_provider_t provider;char *text;
 uint64_t generation;uint32_t frames;turbo_tts_state_t state;
};
static int valid_text(const char *s,size_t n) {
 size_t i=0;if(!s||!n||n>MAX_TEXT)return 0;
 while(i<n){unsigned char a=(unsigned char)s[i++];unsigned k=0;unsigned cp=0;
  if(a<32||a==127)return 0;
   if(a<128)continue;
  if(a>=194&&a<=223){k=1;cp=a&31;}else if(a>=224&&a<=239){k=2;cp=a&15;}
  else if(a>=240&&a<=244){k=3;cp=a&7;}else return 0;
  if(n-i<k)return 0;
  for(unsigned j=0;j<k;j++){unsigned char b=(unsigned char)s[i++];if((b&192)!=128)return 0;cp=(cp<<6)|(b&63);}
  if((k==1&&cp<128)||(k==2&&cp<2048)||(k==3&&cp<65536)||cp>0x10ffff||
     (cp>=0xd800&&cp<=0xdfff)||(cp>=0x80&&cp<=0x9f)||
     (cp>=0x202a&&cp<=0x202e)||(cp>=0x2066&&cp<=0x2069)||cp==0x061c)return 0;
 }
 return 1;
}
void turbo_tts_provider_init(turbo_tts_provider_t *p) {
 if(!p)return;
 memset(p,0,sizeof(*p));p->struct_size=sizeof(*p);p->abi_version=TURBO_TTS_ABI_VERSION;
}
int turbo_tts_create(const turbo_tts_provider_t *p,turbo_tts_output_t **out){
 if(!out)return -1;
 *out=NULL;
 if(!p||p->struct_size!=sizeof(*p)||p->abi_version!=TURBO_TTS_ABI_VERSION||
    !p->begin||!p->poll||!p->cancel)return -1;
 turbo_tts_output_t *o=calloc(1,sizeof(*o));if(!o)return -2;
 o->text=calloc(MAX_TEXT+1u,1u);if(!o->text){free(o);return -2;}
 o->provider=*p;*out=o;return 0;
}
int turbo_tts_begin(turbo_tts_output_t *o,uint64_t g,const char *text,size_t len){
 if(!o||!g||g<=o->generation||o->state==TURBO_TTS_ACTIVE||!valid_text(text,len))return -1;
 memset(o->text,0,MAX_TEXT+1u);memcpy(o->text,text,len);
 o->generation=g;o->frames=0;
 if(o->provider.begin(o->provider.context,g,o->text,len)!=0){
  memset(o->text,0,MAX_TEXT+1u);o->state=TURBO_TTS_FAILED;return -3;
 }
 o->state=TURBO_TTS_ACTIVE;return 0;
}
int turbo_tts_poll(turbo_tts_output_t *o,int16_t *buf,size_t capacity,size_t *n,turbo_tts_poll_t *kind){
 if(!n||!kind)return -1;
 *n=0;*kind=TURBO_TTS_WOULD_BLOCK;
 if(!o||!buf||capacity!=TURBO_TTS_SAMPLES_PER_FRAME||o->state!=TURBO_TTS_ACTIVE)return -1;
 size_t produced=0;turbo_tts_poll_t k=TURBO_TTS_WOULD_BLOCK;
 int rc=o->provider.poll(o->provider.context,o->generation,buf,capacity,&produced,&k);
 if(rc||produced>capacity||(k==TURBO_TTS_PCM&&!produced)||
    (k!=TURBO_TTS_PCM&&produced)||k>TURBO_TTS_TERMINAL||
    (k==TURBO_TTS_PCM&&o->frames==MAX_FRAMES)){
  o->provider.cancel(o->provider.context,o->generation);o->state=TURBO_TTS_FAILED;
  memset(o->text,0,MAX_TEXT+1u);memset(buf,0,capacity*sizeof(*buf));return -3;
 }
 if(k==TURBO_TTS_PCM)o->frames++;
 if(k==TURBO_TTS_TERMINAL){o->state=TURBO_TTS_DONE;memset(o->text,0,MAX_TEXT+1u);}
 *n=produced;*kind=k;return 0;
}
int turbo_tts_cancel(turbo_tts_output_t *o){
 if(!o)return -1;
 if(o->state==TURBO_TTS_CANCELED)return 0;
 if(o->state!=TURBO_TTS_ACTIVE)return -1;
 o->provider.cancel(o->provider.context,o->generation);
 o->state=TURBO_TTS_CANCELED;memset(o->text,0,MAX_TEXT+1u);return 0;
}
turbo_tts_state_t turbo_tts_state(const turbo_tts_output_t *o){return o?o->state:TURBO_TTS_FAILED;}
int turbo_tts_destroy(turbo_tts_output_t **p){
 if(!p||!*p)return -1;
 turbo_tts_output_t *o=*p;if(o->state==TURBO_TTS_ACTIVE)return -2;
 memset(o->text,0,MAX_TEXT+1u);free(o->text);free(o);*p=NULL;return 0;
}
