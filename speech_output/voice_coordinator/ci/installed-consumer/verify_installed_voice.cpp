#include "turbo_agent_voice_coordinator.h"
#include <type_traits>
static_assert(std::is_enum<turbo_voice_state_t>::value, "M4.2 ABI enum");
static_assert(TURBO_VOICE_MAX_POLL_BUDGET == 8u, "bounded owner tick");
int main() {
  turbo_voice_snapshot_t snapshot{};
  turbo_voice_snapshot_init(&snapshot);
  if (snapshot.struct_size != sizeof(snapshot) ||
      snapshot.abi_version != TURBO_VOICE_SNAPSHOT_ABI_VERSION) return 3;
  if (turbo_voice_get_snapshot(nullptr, &snapshot) != TURBO_VOICE_EINVAL)
    return 4;
  const turbo_voice_coordinator_t *v = nullptr;
  return turbo_voice_get_generation(v) == 0u &&
         turbo_voice_get_state(v) == TURBO_VOICE_FAILED ? 0 : 1;
}
