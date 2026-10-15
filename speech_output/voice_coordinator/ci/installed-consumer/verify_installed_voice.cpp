#include "turbo_agent_voice_coordinator.h"
#include <type_traits>
static_assert(std::is_enum<turbo_voice_state_t>::value, "M4.2 ABI enum");
static_assert(TURBO_VOICE_MAX_POLL_BUDGET == 8u, "bounded owner tick");
int main() {
  const turbo_voice_coordinator_t *v = nullptr;
  return turbo_voice_get_generation(v) == 0u &&
         turbo_voice_get_state(v) == TURBO_VOICE_FAILED ? 0 : 1;
}
