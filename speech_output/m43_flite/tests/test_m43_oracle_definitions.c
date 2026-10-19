#include <stdint.h>

#ifndef M43_APPROVED_PCM_SAMPLES
#error "Reviewed PCM count compile definition missing"
#endif
#ifndef M43_APPROVED_PCM_FNV64
#error "Reviewed PCM FNV64 compile definition missing"
#endif

/* Model-free compile-only conformance: protects the exact same CMake macro
 * formation used by the manually licensed real-voice test. CMake must not
 * break 0x...ULL into a function-style macro or extra compiler arguments. */
int main(void) {
  return M43_APPROVED_PCM_SAMPLES == 80u &&
         (uint64_t)M43_APPROVED_PCM_FNV64 ==
             UINT64_C(0x1122334455667788) ? 0 : 1;
}
