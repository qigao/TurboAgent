#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* No Flite, model, audio or network access in the default CI. This fixture
 * exercises the same *process* transport used by the reviewed real helper. */
static int read_all(uint8_t *p,size_t n) {
  while(n) {
    ssize_t s=read(STDIN_FILENO,p,n);
    if(s<0 && errno==EINTR) continue;
    if(s<=0) return -1;
    p+=(size_t)s;n-=(size_t)s;
  }
  return 0;
}
static int write_all(const uint8_t *p,size_t n) {
  while(n) {
    ssize_t s=write(STDOUT_FILENO,p,n);
    if(s<0 && errno==EINTR) continue;
    if(s<=0) return -1;
    p+=(size_t)s;n-=(size_t)s;
  }
  return 0;
}
static int emit(uint32_t type,uint32_t n,int second) {
  uint8_t b[M43_MAX_FRAME_BYTES]={0};
  m43_set_u32(b,type);m43_set_u32(b+4,n);
  if(type==M43_PCM_MAGIC && n<=M43_MAX_SAMPLES) {
    for(uint32_t i=0;i<n;++i)
      m43_set_s16(b+M43_REPLY_HEADER+2u*i,
                  (int16_t)(second ? (-300-(int)i) : (1000+(int)(i%200u))));
  }
  return write_all(b,M43_REPLY_HEADER+
                   (type==M43_PCM_MAGIC && n<=M43_MAX_SAMPLES ? 2u*n : 0u));
}
int main(void) {
  uint8_t header[M43_REQUEST_HEADER]={0};
  if(read_all(header,sizeof(header))!=0 ||
     m43_get_u32(header)!=M43_REQUEST_MAGIC ||
     m43_get_u32(header+4)!=M43_WIRE_VERSION ||
     m43_get_u32(header+12)!=M43_TOTAL_SAMPLE_LIMIT) return 4;
  uint32_t n=m43_get_u32(header+8);
  if(!n || n>128u) return 4;
  char text[129]={0};
  if(read_all((uint8_t*)text,n)!=0) return 4;
  if(strcmp(text,"die")==0) return 7;
  if(strcmp(text,"bad")==0) {
    (void)emit(M43_PCM_MAGIC,481u,0);
    return 0;
  }
  if(strcmp(text,"ignore")==0) {
    (void)signal(SIGTERM,SIG_IGN);
    for(;;) pause();
  }
  if(strcmp(text,"hang")==0) {
    for(;;) pause();
  }
  if(emit(M43_PCM_MAGIC,480u,0)!=0) return 4;
  if(strcmp(text,"cancel")==0) {
    for(;;) pause();
  }
  if(emit(M43_PCM_MAGIC,120u,1)!=0 ||
     emit(M43_DONE_MAGIC,0u,0)!=0) return 4;
  return 0;
}
