#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "turbo_agent_tts_flite_process.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* This is a POSIX control-plane adapter, never a device callback.
 * The child is the only vendor-model owner; the parent retains at most ONE
 * partially decoded 20ms block, and cannot run vendor TTS in-process. */
struct turbo_m43_flite_process {
  char *helper_path;
  uint32_t max_text_bytes, max_wall_ms, grace_ms;
  uint64_t generation, started_ms, cancel_ms;
  pid_t pid;                  /* -1 means waitpid completed or not spawned */
  int read_fd;
  int inflight, canceled, saw_done, exited_ok, header_parsed;
  size_t used, needed;
  uint32_t current_type, current_samples;
  uint32_t samples_seen;
  uint8_t rx[M43_MAX_FRAME_BYTES];
};

static void scrub(void *ptr, size_t n) {
  volatile uint8_t *p = (volatile uint8_t *)ptr;
  while (n--) *p++ = 0u;
}
static uint64_t monotonic_ms(void) {
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0u;
  return (uint64_t)ts.tv_sec * 1000u +
         (uint64_t)ts.tv_nsec / 1000000u;
}
static void close_fd(int *fd) {
  if (*fd >= 0) { (void)close(*fd); *fd = -1; }
}
static int fd_cloexec(int fd) {
  int flags = fcntl(fd, F_GETFD, 0);
  return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}
static int fd_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static int pipe_pair(int fds[2]) {
  if (pipe(fds) != 0) return -1;
  /* Child stdin/stdout remain blocking: only PARENT ends are set
   * nonblocking after spawn, to keep child progress backpressured. */
  if (!fd_cloexec(fds[0]) || !fd_cloexec(fds[1])) {
    close_fd(&fds[0]); close_fd(&fds[1]);
    return -1;
  }
  return 0;
}
static void reap_nonblocking(turbo_m43_flite_process_t *c) {
  if (c->pid <= 0) return;
  int status = 0;
  pid_t rc = waitpid(c->pid, &status, WNOHANG);
  if (rc == c->pid || (rc < 0 && errno == ECHILD)) {
    c->exited_ok = rc == c->pid &&
                   WIFEXITED(status) && WEXITSTATUS(status) == 0;
    c->pid = -1;
  }
}
static int write_memfd(int fd,const uint8_t *p,size_t n) {
  while (n) {
    ssize_t got=write(fd,p,n);
    if (got<0 && errno==EINTR) continue;
    if (got<=0) return -1;
    p+=(size_t)got; n-=(size_t)got;
  }
  return 0;
}
static int spawn_child(turbo_m43_flite_process_t *c,
                       const char *text, size_t size) {
  /* Immutable anonymous Linux memory FD avoids SIGPIPE, blocked writes,
   * shell quoting, argv leaks and on-disk retention of approved text.
   * The helper receives the sealed request as its standard input. */
  int mem=memfd_create("turbo-m43-approved",MFD_CLOEXEC|MFD_ALLOW_SEALING);
  if (mem<0) return -1;
  uint8_t request[M43_REQUEST_HEADER + TURBO_M43_FLITE_MAX_TEXT_BYTES]={0};
  m43_set_u32(request,M43_REQUEST_MAGIC);
  m43_set_u32(request+4,M43_WIRE_VERSION);
  m43_set_u32(request+8,(uint32_t)size);
  m43_set_u32(request+12,M43_TOTAL_SAMPLE_LIMIT);
  memcpy(request+M43_REQUEST_HEADER,text,size);
  int ready=write_memfd(mem,request,M43_REQUEST_HEADER+size)==0 &&
            lseek(mem,0,SEEK_SET)==0 &&
            fcntl(mem,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|
                              F_SEAL_SHRINK|F_SEAL_SEAL)==0;
  scrub(request,sizeof(request));
  if(!ready) {close_fd(&mem);return -1;}
  int output[2]={-1,-1};
  if(pipe_pair(output)!=0) {close_fd(&mem);return -1;}

  posix_spawn_file_actions_t a;
  if(posix_spawn_file_actions_init(&a)!=0) {
    close_fd(&mem);close_fd(&output[0]);close_fd(&output[1]);
    return -1;
  }
  int ok=posix_spawn_file_actions_adddup2(&a,mem,STDIN_FILENO)==0 &&
         posix_spawn_file_actions_adddup2(&a,output[1],STDOUT_FILENO)==0 &&
         posix_spawn_file_actions_addclose(&a,mem)==0 &&
         posix_spawn_file_actions_addclose(&a,output[0])==0 &&
         posix_spawn_file_actions_addclose(&a,output[1])==0;
  pid_t child=-1;
  char *argv[]={c->helper_path,NULL};
  char *envp[]={"LANG=C.UTF-8",NULL};
  int rc=ok?posix_spawn(&child,c->helper_path,&a,NULL,argv,envp):EINVAL;
  (void)posix_spawn_file_actions_destroy(&a);
  close_fd(&mem);close_fd(&output[1]);
  if(rc!=0) {close_fd(&output[0]);return -1;}
  c->pid=child;
  c->inflight=1;
  c->read_fd=output[0];
  c->started_ms=monotonic_ms();
  if(!fd_nonblock(c->read_fd)) {
    c->canceled=1;
    c->cancel_ms=monotonic_ms();
    (void)kill(c->pid,SIGTERM);
    return -1;
  }
  c->needed=M43_REPLY_HEADER;
  c->used=0u;
  return 0;
}
static int provider_begin(void *ptr,uint64_t generation,
                          const char *text,size_t size,
                          const char *voice,const char *language,
                          const turbo_tts_format_t *format) {
  turbo_m43_flite_process_t *c = (turbo_m43_flite_process_t *)ptr;
  if (!c || !generation || c->inflight || c->pid > 0) return -1;
  c->generation = generation;
  c->canceled = 0;
  c->saw_done = 0;
  c->exited_ok = 0;
  c->header_parsed = 0;
  c->samples_seen = 0u;
  c->used = 0u;
  c->needed = M43_REPLY_HEADER;
  scrub(c->rx,sizeof(c->rx));
  if (!text || !size || size > c->max_text_bytes ||
      !voice || strcmp(voice,TURBO_M43_FLITE_VOICE_ID) != 0 ||
      !language || strcmp(language,TURBO_M43_FLITE_LANGUAGE) != 0 ||
      !format || format->sample_rate_hz != 24000u ||
      format->channels != 1u || format->bits_per_sample != 16u ||
      format->frame_samples != M43_MAX_SAMPLES) {
    c->canceled = 1;
    return -1;
  }
  /* SpeechOutput performs the authoritative UTF-8/markup admission. The
   * child independently validates its restricted English input. */
  return spawn_child(c,text,size);
}
static void provider_cancel(void *ptr,uint64_t generation) {
  turbo_m43_flite_process_t *c = (turbo_m43_flite_process_t *)ptr;
  if (!c || c->generation != generation || c->canceled) return;
  c->canceled = 1;
  c->cancel_ms = monotonic_ms();
  if (c->pid > 0) (void)kill(c->pid,SIGTERM);
}
static int settle_canceled(turbo_m43_flite_process_t *c,
                           size_t *out_samples,turbo_tts_poll_t *out_kind) {
  (void)out_samples;
  reap_nonblocking(c);
  if (c->pid > 0 && monotonic_ms() - c->cancel_ms > c->grace_ms) {
    (void)kill(c->pid,SIGKILL);
    reap_nonblocking(c);
  }
  if (c->pid > 0) return 0; /* WOULD_BLOCK; never declare DSO safe early */
  close_fd(&c->read_fd);
  scrub(c->rx,sizeof(c->rx));
  c->inflight = 0;
  c->used = 0u;
  *out_kind = TURBO_TTS_POLL_CANCELED;
  return 0;
}
static int poll_done(turbo_m43_flite_process_t *c,
                     turbo_tts_poll_t *kind) {
  /* Confirm that a child cannot send extra bytes after DONE. */
  uint8_t extra = 0u;
  ssize_t n = read(c->read_fd,&extra,1u);
  if (n > 0) return -1;
  if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
      errno != EINTR) return -1;
  if (n != 0) return 0;
  reap_nonblocking(c);
  if (c->pid > 0) return 0;
  if (!c->exited_ok) return -1;
  close_fd(&c->read_fd);
  c->inflight = 0;
  *kind = TURBO_TTS_TERMINAL;
  return 0;
}
static int provider_poll(void *ptr,uint64_t generation,int16_t *dst,
                         size_t capacity,size_t *samples,
                         turbo_tts_poll_t *kind) {
  turbo_m43_flite_process_t *c = (turbo_m43_flite_process_t *)ptr;
  if (!c || c->generation != generation || !dst || !samples || !kind ||
      capacity != M43_MAX_SAMPLES) return -1;
  *samples = 0u;
  *kind = TURBO_TTS_WOULD_BLOCK;
  if (c->canceled) return settle_canceled(c,samples,kind);
  if (!c->inflight || c->read_fd < 0) return -1;
  if (monotonic_ms() - c->started_ms > c->max_wall_ms) return -1;
  if (c->saw_done) return poll_done(c,kind);

  if (c->used < c->needed) {
    ssize_t n = read(c->read_fd,c->rx+c->used,c->needed-c->used);
    if (n < 0 && (errno==EINTR || errno==EAGAIN ||
                   errno==EWOULDBLOCK)) return 0;
    if (n <= 0) return -1; /* EOF without complete terminal is an error */
    c->used += (size_t)n;
    if (c->used < c->needed) return 0;
  }
  if (!c->header_parsed) {
    c->current_type = m43_get_u32(c->rx);
    c->current_samples = m43_get_u32(c->rx+4);
    if (c->current_type == M43_DONE_MAGIC &&
        c->current_samples == 0u) {
      c->saw_done = 1;
      c->used = 0u;
      return 0;
    }
    if (c->current_type != M43_PCM_MAGIC ||
        c->current_samples == 0u ||
        c->current_samples > M43_MAX_SAMPLES ||
        c->samples_seen > M43_TOTAL_SAMPLE_LIMIT-c->current_samples)
      return -1;
    c->needed = M43_REPLY_HEADER + (size_t)c->current_samples*2u;
    c->header_parsed = 1;
    if (c->used < c->needed) return 0;
  }
  for (size_t i=0u;i<c->current_samples;++i)
    dst[i] = m43_get_s16(c->rx+M43_REPLY_HEADER+2u*i);
  *samples = c->current_samples;
  *kind = TURBO_TTS_PCM;
  c->samples_seen += c->current_samples;
  scrub(c->rx,sizeof(c->rx));
  c->used = 0u;
  c->needed = M43_REPLY_HEADER;
  c->header_parsed = 0;
  return 0;
}
void turbo_m43_flite_config_init(turbo_m43_flite_config_t *c) {
  if (!c) return;
  memset(c,0,sizeof(*c));
  c->struct_size=sizeof(*c);
  c->abi_version=TURBO_M43_FLITE_ABI_VERSION;
  c->max_text_bytes=TURBO_M43_FLITE_MAX_TEXT_BYTES;
  c->max_wall_ms=10000u;
  c->cancel_grace_ms=200u;
}
int turbo_m43_flite_create(const turbo_m43_flite_config_t *cfg,
                            turbo_m43_flite_process_t **out) {
  if (!out) return TURBO_TTS_EINVAL;
  *out = NULL;
  if (!cfg || cfg->struct_size!=sizeof(*cfg) ||
      cfg->abi_version!=TURBO_M43_FLITE_ABI_VERSION ||
      !cfg->helper_absolute_path || cfg->helper_absolute_path[0]!='/' ||
      !cfg->max_text_bytes || cfg->max_text_bytes>TURBO_M43_FLITE_MAX_TEXT_BYTES ||
      !cfg->max_wall_ms || cfg->max_wall_ms>30000u ||
      !cfg->cancel_grace_ms || cfg->cancel_grace_ms>1000u) return TURBO_TTS_EINVAL;
  size_t n = strnlen(cfg->helper_absolute_path,4096u);
  if (n == 4096u || access(cfg->helper_absolute_path,X_OK)!=0)
    return TURBO_TTS_EINVAL;
  turbo_m43_flite_process_t *c = calloc(1u,sizeof(*c));
  if (!c) return TURBO_TTS_ENOMEM;
  c->helper_path = malloc(n+1u);
  if (!c->helper_path) {free(c);return TURBO_TTS_ENOMEM;}
  memcpy(c->helper_path,cfg->helper_absolute_path,n+1u);
  c->max_text_bytes=cfg->max_text_bytes;
  c->max_wall_ms=cfg->max_wall_ms;
  c->grace_ms=cfg->cancel_grace_ms;
  c->pid=-1;
  c->read_fd=-1;
  *out=c;
  return TURBO_TTS_OK;
}
int turbo_m43_flite_bind(turbo_m43_flite_process_t *c,
                          turbo_tts_provider_t *p) {
  if (!c || !p || c->inflight) return TURBO_TTS_EINVAL;
  /* Self-contained DSO: do not require a host-exported SpeechOutput
   * initializer or resolve an unversioned symbol across loader images. */
  memset(p, 0, sizeof(*p));
  p->struct_size = sizeof(*p);
  p->abi_version = TURBO_TTS_ABI_VERSION;
  p->context=c;
  p->begin=provider_begin;
  p->poll_pcm=provider_poll;
  p->request_cancel=provider_cancel;
  return TURBO_TTS_OK;
}
int turbo_m43_flite_destroy(turbo_m43_flite_process_t **handle) {
  if (!handle || !*handle) return TURBO_TTS_EINVAL;
  turbo_m43_flite_process_t *c=*handle;
  if (c->inflight || c->pid>0) return TURBO_TTS_EBUSY;
  close_fd(&c->read_fd);
  scrub(c->helper_path,strlen(c->helper_path));
  free(c->helper_path);
  scrub(c,sizeof(*c));
  free(c);
  *handle=NULL;
  return TURBO_TTS_OK;
}
