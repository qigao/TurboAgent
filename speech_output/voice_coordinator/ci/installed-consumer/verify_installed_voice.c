#include "turbo_agent_voice_coordinator.h"

/* Exercise exact current-head archived symbol resolution against real
 * published Salts::Playback without creating/starting an audio device. */
int main(void) {
  turbo_voice_snapshot_t snapshot;
  turbo_voice_snapshot_init(&snapshot);
  if (snapshot.struct_size != sizeof(snapshot) ||
      snapshot.abi_version != TURBO_VOICE_SNAPSHOT_ABI_VERSION) return 3;
  if (turbo_voice_get_snapshot(NULL, &snapshot) != TURBO_VOICE_EINVAL)
    return 4;
  if (turbo_voice_get_generation(NULL) != 0u) return 1;
  if (turbo_voice_get_state(NULL) != TURBO_VOICE_FAILED) return 2;
  return 0;
}
