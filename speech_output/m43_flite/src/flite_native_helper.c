#define _POSIX_C_SOURCE 200809L
/* Opt-in helper compiled ONLY when an independently licensed, pinned Flite
 * 2.2 C SDK and its explicitly approved CMU kal16 voice are provided.
 * The default CI never builds, downloads, loads or executes this vendor.
 * The subprocess is the resource-budgeted owner of the entire Flite waveform. */
#include "protocol.h"
#include "turbo_agent_tts_flite_process.h"
#include <flite.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

extern cst_voice *register_cmu_us_kal16(const char *voice_dir);

_Static_assert(sizeof(short)==sizeof(int16_t),"Flite must use S16");
static int read_all(int fd,uint8_t *p,size_t n) {
  while (n) {
    ssize_t got=read(fd,p,n);
    if (got<0 && errno==EINTR) continue;
    if (got<=0) return -1;
    n-=(size_t)got; p+=(size_t)got;
  }
  return 0;
}
static int write_all(int fd,const uint8_t *p,size_t n) {
  while (n) {
    ssize_t sent=write(fd,p,n);
    if (sent<0 && errno==EINTR) continue;
    if (sent<=0) return -1;
    n-=(size_t)sent; p+=(size_t)sent;
  }
  return 0;
}
static void scrub(void *ptr,size_t bytes) {
  volatile uint8_t *p=ptr;
  while(bytes--) *p++=0u;
}
static void limit_resources(void) {
  /* CPU cap is the authoritative hard stop for noncooperative Flite.
   * RLIMIT_AS limits whole-wave vendor memory allocation before it can
   * affect the host. No disk file, core dump or default audio backend. */
  const struct rlimit mem={256u*1024u*1024u,256u*1024u*1024u};
  const struct rlimit cpu={5u,5u};
  const struct rlimit none={0u,0u};
  if (setrlimit(RLIMIT_AS,&mem)!=0 ||
      setrlimit(RLIMIT_CPU,&cpu)!=0 ||
      setrlimit(RLIMIT_FSIZE,&none)!=0 ||
      setrlimit(RLIMIT_CORE,&none)!=0)
    _exit(124);
}
static int plain_english(const char *s,size_t n) {
  if (!n || n>TURBO_M43_FLITE_MAX_TEXT_BYTES) return 0;
  for(size_t i=0u;i<n;i++) {
    unsigned char c=(unsigned char)s[i];
    /* Flite kal16 is English. No implicit locale fallback, markup/SSML,
     * control bytes or unreviewed non-ASCII normalization. */
    if (c<0x20u || c>0x7eu || c=='<' || c=='>') return 0;
  }
  return 1;
}
static int reply_control(uint32_t type,uint32_t n) {
  uint8_t h[M43_REPLY_HEADER]={0};
  m43_set_u32(h,type);m43_set_u32(h+4,n);
  return write_all(STDOUT_FILENO,h,sizeof(h));
}
int main(void) {
  limit_resources();
  uint8_t request[M43_REQUEST_HEADER]={0};
  if(read_all(STDIN_FILENO,request,sizeof(request))!=0 ||
     m43_get_u32(request)!=M43_REQUEST_MAGIC ||
     m43_get_u32(request+4)!=M43_WIRE_VERSION) return 2;
  uint32_t n=m43_get_u32(request+8);
  uint32_t budget=m43_get_u32(request+12);
  if(!n || n>TURBO_M43_FLITE_MAX_TEXT_BYTES ||
     budget!=M43_TOTAL_SAMPLE_LIMIT) return 2;
  char text[TURBO_M43_FLITE_MAX_TEXT_BYTES+1u]={0};
  if(read_all(STDIN_FILENO,(uint8_t *)text,n)!=0 ||
     !plain_english(text,n)) {
    scrub(text,sizeof(text));
    (void)reply_control(M43_ERROR_MAGIC,1u);
    return 2;
  }
  text[n]='\0';
  (void)flite_init();
  /* Never call flite_voice_select(), which can treat file:/http: names
   * as downloadable assets. This is a compiled, exact named CMU voice. */
  cst_voice *voice=register_cmu_us_kal16(NULL);
  if(!voice){scrub(text,sizeof(text));return 3;}
  cst_wave *wave=flite_text_to_wave(text,voice);
  scrub(text,sizeof(text));
  if(!wave) {(void)reply_control(M43_ERROR_MAGIC,2u);return 3;}
  if(wave->sample_rate!=16000 || wave->num_channels!=1 ||
     wave->num_samples<0 || !wave->samples) {
    delete_wave(wave);
    (void)reply_control(M43_ERROR_MAGIC,3u);
    return 3;
  }
  /* Explicitly approved 16k -> 24k resampling is confined to the helper.
   * No hidden host format conversion or native playback fallback. */
  cst_wave_resample(wave,24000);
  if(wave->sample_rate!=24000 || wave->num_channels!=1 ||
     wave->num_samples<0 || (uint32_t)wave->num_samples>budget ||
     (wave->num_samples>0 && !wave->samples)) {
    delete_wave(wave);
    (void)reply_control(M43_ERROR_MAGIC,4u);
    return 3;
  }
  uint8_t buf[M43_MAX_FRAME_BYTES]={0};
  size_t total=(size_t)wave->num_samples;
  for(size_t offset=0u;offset<total;) {
    size_t amount=total-offset;
    if(amount>M43_MAX_SAMPLES) amount=M43_MAX_SAMPLES;
    m43_set_u32(buf,M43_PCM_MAGIC);
    m43_set_u32(buf+4,(uint32_t)amount);
    for(size_t i=0u;i<amount;++i)
      m43_set_s16(buf+M43_REPLY_HEADER+2u*i,
                  (int16_t)wave->samples[offset+i]);
    if(write_all(STDOUT_FILENO,buf,M43_REPLY_HEADER+2u*amount)!=0) {
      scrub(buf,sizeof(buf));
      delete_wave(wave);
      return 4;
    }
    offset+=amount;
  }
  scrub(buf,sizeof(buf));
  delete_wave(wave);
  if(reply_control(M43_DONE_MAGIC,0u)!=0) return 4;
  return 0;
}
