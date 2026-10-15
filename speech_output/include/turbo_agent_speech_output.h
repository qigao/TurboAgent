#ifndef TURBO_AGENT_SPEECH_OUTPUT_H
#define TURBO_AGENT_SPEECH_OUTPUT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TURBO_TTS_ABI_VERSION 1u
#define TURBO_TTS_SAMPLES_PER_FRAME 480u
typedef struct turbo_tts_output turbo_tts_output_t;
typedef enum {TURBO_TTS_IDLE,TURBO_TTS_ACTIVE,TURBO_TTS_DONE,TURBO_TTS_CANCELED,TURBO_TTS_FAILED} turbo_tts_state_t;
typedef enum {TURBO_TTS_WOULD_BLOCK,TURBO_TTS_PCM,TURBO_TTS_TERMINAL} turbo_tts_poll_t;
typedef struct turbo_tts_provider {
 size_t struct_size;uint32_t abi_version;void *context;
 int (*begin)(void *,uint64_t,const char *,size_t);
 int (*poll)(void *,uint64_t,int16_t *,size_t,size_t *,turbo_tts_poll_t *);
 void (*cancel)(void *,uint64_t);
} turbo_tts_provider_t;
void turbo_tts_provider_init(turbo_tts_provider_t *);
int turbo_tts_create(const turbo_tts_provider_t *,turbo_tts_output_t **);
int turbo_tts_begin(turbo_tts_output_t *,uint64_t,const char *,size_t);
int turbo_tts_poll(turbo_tts_output_t *,int16_t *,size_t,size_t *,turbo_tts_poll_t *);
int turbo_tts_cancel(turbo_tts_output_t *);
turbo_tts_state_t turbo_tts_state(const turbo_tts_output_t *);
int turbo_tts_destroy(turbo_tts_output_t **);
#ifdef __cplusplus
}
#endif
#endif
