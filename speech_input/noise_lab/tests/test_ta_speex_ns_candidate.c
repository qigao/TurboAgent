#include "ta_speex_ns_candidate.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) return __LINE__; } while (0)

int main(void) {
  ta_speex_ns_owner_t *one = NULL, *two = NULL;
  int16_t input[TA_SPEEX_NS_FRAME_SAMPLES] = {0};
  int16_t output[TA_SPEEX_NS_FRAME_SAMPLES] = {0};
  int16_t again[TA_SPEEX_NS_FRAME_SAMPLES] = {0};
  size_t i, j;
  CHECK(ta_speex_ns_create(0, &one) == TA_SPEEX_NS_EINVAL);
  CHECK(ta_speex_ns_create(-12, &one) == TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_create(-12, &two) == TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_create(-12, &one) == TA_SPEEX_NS_EINVAL);
  CHECK(ta_speex_ns_process(one, input, 319u, output) == TA_SPEEX_NS_EINVAL);
  CHECK(ta_speex_ns_processed_frames(one) == 0u);
  for (i=0u; i<100u; ++i) {
    for (j=0u; j<TA_SPEEX_NS_FRAME_SAMPLES; ++j) {
      const int amplitude = j % 20u < 10u ? 2800 : -2800;
      input[j] = (int16_t)(amplitude + (int)(i%5u)*10);
    }
    CHECK(ta_speex_ns_process(one,input,320u,output)==TA_SPEEX_NS_OK);
    CHECK(ta_speex_ns_process(two,input,320u,again)==TA_SPEEX_NS_OK);
    CHECK(memcmp(output,again,sizeof(output))==0);
  }
  CHECK(ta_speex_ns_processed_frames(one) == 100u);
  CHECK(ta_speex_ns_finish(one, output, 319u) == TA_SPEEX_NS_EINVAL);
  CHECK(ta_speex_ns_finish(one, output, 320u) == TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_processed_frames(one) == 100u);
  CHECK(ta_speex_ns_finish(one, output, 320u) == TA_SPEEX_NS_ESETTLED);
  CHECK(ta_speex_ns_process(one, input, 320u, output) ==
        TA_SPEEX_NS_ESETTLED);
  CHECK(ta_speex_ns_destroy(&two) == TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_destroy(&one) == TA_SPEEX_NS_OK);
  CHECK(one == NULL && two == NULL);
  CHECK(ta_speex_ns_create(-18, &one) == TA_SPEEX_NS_OK);
  memset(input,0,sizeof(input));
  for (i=0u;i<TA_SPEEX_NS_MAX_FRAMES;++i)
    CHECK(ta_speex_ns_process(one,input,320u,output)==TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_processed_frames(one)==TA_SPEEX_NS_MAX_FRAMES);
  CHECK(ta_speex_ns_process(one,input,320u,output)==TA_SPEEX_NS_ECAPACITY);
  CHECK(ta_speex_ns_finish(one, output, 320u) == TA_SPEEX_NS_OK);
  CHECK(ta_speex_ns_processed_frames(one)==TA_SPEEX_NS_MAX_FRAMES);
  CHECK(ta_speex_ns_finish(one, output, 320u) == TA_SPEEX_NS_ESETTLED);
  CHECK(ta_speex_ns_destroy(&one)==TA_SPEEX_NS_OK);
  puts("SpeexDSP C11 strict owner, deterministic replay and bounded frames PASS");
  return 0;
}
