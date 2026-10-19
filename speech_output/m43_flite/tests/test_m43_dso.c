#define _POSIX_C_SOURCE 200809L
#include "turbo_agent_tts_flite_dso.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef TEST_M43_HELPER_PATH
#error "Exact fixture path is required"
#endif
#ifndef TEST_M43_DSO_PATH
#error "Exact provider DSO path is required"
#endif

#define CHECK(x) do { if(!(x)) { \
  fprintf(stderr,"FAIL M4.3 dlopen line %d: %s\n",__LINE__,#x); \
  return 1; } } while(0)

static void nap_ms(long ms) {
  struct timespec ts = { ms/1000, (ms%1000)*1000000L };
  (void)nanosleep(&ts,NULL);
}
static int execute_case(int scenario) {
  unsigned unloads = 0u;
  void *module=dlopen(TEST_M43_DSO_PATH,RTLD_NOW|RTLD_LOCAL);
  CHECK(module != NULL);
  void *symbol=dlsym(module,"turbo_m43_flite_dso_entry");
  CHECK(symbol != NULL);
  turbo_m43_flite_dso_entry_fn entry = NULL;
  _Static_assert(sizeof(entry)==sizeof(symbol),
                 "POSIX function-pointer dlsym slot must match");
  memcpy(&entry,&symbol,sizeof(entry));
  CHECK(entry != NULL);
  const turbo_m43_flite_dso_api_t *api=entry();
  CHECK(api && api->struct_size==sizeof(*api) &&
        api->abi_version==TURBO_M43_FLITE_DSO_ABI_VERSION);
  CHECK(api->config_init && api->create && api->bind && api->destroy &&
        api->test_set_unload_counter);
  api->test_set_unload_counter(&unloads);

  turbo_m43_flite_config_t cfg;
  api->config_init(&cfg);
  cfg.helper_absolute_path=TEST_M43_HELPER_PATH;
  cfg.cancel_grace_ms=30u;
  cfg.max_wall_ms=2200u;
  turbo_m43_flite_process_t *process=NULL;
  CHECK(api->create(&cfg,&process)==TURBO_TTS_OK && process!=NULL);
  turbo_tts_provider_t provider;
  CHECK(api->bind(process,&provider)==TURBO_TTS_OK);
  CHECK(provider.struct_size==sizeof(provider) &&
        provider.abi_version==TURBO_TTS_ABI_VERSION);

  turbo_tts_config_t output_cfg;
  turbo_tts_config_init(&output_cfg);
  output_cfg.max_text_bytes=128u;
  turbo_tts_output_t *output=NULL;
  CHECK(turbo_tts_create(&output_cfg,&provider,&output)==TURBO_TTS_OK);
  const char *text=scenario==1?"ignore":(scenario==2?"bad":"normal");
  CHECK(turbo_tts_begin(output,42u,text,strlen(text),
                        TURBO_M43_FLITE_VOICE_ID,
                        TURBO_M43_FLITE_LANGUAGE)==TURBO_TTS_OK);
  CHECK(turbo_tts_destroy(&output)==TURBO_TTS_EBUSY);
  CHECK(api->destroy(&process)==TURBO_TTS_EBUSY);
  CHECK(unloads==0u);

  if (scenario==1) {
    nap_ms(20);
    CHECK(turbo_tts_cancel(output)==TURBO_TTS_OK);
    CHECK(turbo_tts_destroy(&output)==TURBO_TTS_EBUSY);
    CHECK(api->destroy(&process)==TURBO_TTS_EBUSY);
  }
  unsigned frames=0u;
  int saw_error=0,settled=0;
  for(unsigned tick=0;tick<1200u;tick++) {
    int16_t pcm[480]={0};
    size_t count=0u;
    turbo_tts_poll_t kind=TURBO_TTS_WOULD_BLOCK;
    int rc=turbo_tts_poll(output,pcm,480u,&count,&kind);
    if(scenario==2 && rc==TURBO_TTS_EPROVIDER) {
      saw_error=1;
    } else {
      CHECK(rc==TURBO_TTS_OK);
      if(kind==TURBO_TTS_PCM) {
        CHECK(scenario==0);
        CHECK(count==((frames==0u)?480u:120u));
        CHECK(pcm[0]==(frames==0u?1000:-300));
        frames++;
      }
    }
    turbo_tts_state_t state=turbo_tts_state(output);
    if((scenario==0 && state==TURBO_TTS_SOURCE_DONE) ||
       (scenario==1 && state==TURBO_TTS_CANCELED) ||
       (scenario==2 && state==TURBO_TTS_FAILED)) {
      settled=1;break;
    }
    nap_ms(2);
  }
  CHECK(settled);
  CHECK((scenario!=0 || frames==2u) &&
        (scenario!=2 || saw_error));
  CHECK(unloads==0u);
  CHECK(turbo_tts_destroy(&output)==TURBO_TTS_OK && output==NULL);
  CHECK(api->destroy(&process)==TURBO_TTS_OK && process==NULL);
  CHECK(unloads==0u);
  CHECK(dlclose(module)==0);
  CHECK(unloads==1u); /* actual DSO destructor ran exactly once */
  printf("PASS M4.3 DSO %d: child terminal/reaped, borrowed owners destroyed before dlclose\n",
         scenario);
  return 0;
}
int main(void) {
  CHECK(execute_case(0)==0);
  CHECK(execute_case(1)==0);
  CHECK(execute_case(2)==0);
  puts("PASS M4.3 dlopen/true worker process/cancel/reap/dlclose 3/3");
  return 0;
}
