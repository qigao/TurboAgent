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
  /* This REAL published Playback call rejects unsupported PCM rate before
   * calloc/ma_context_init/ma_device_init (source tag rc.11 audited).
   * It MUST NOT open an audio device. */
  salts_playback_config_t invalid_config = {
      12345u, 1u, SALTS_PLAYBACK_FORMAT_S16, 100u
  };
  salts_playback_t *native = (salts_playback_t *)(uintptr_t)1u;
  if (salts_playback_create(NULL, &invalid_config, &native) !=
      SALTS_PLAYBACK_ERR_FORMAT || native != NULL)
    return 5;
  if (turbo_voice_get_generation(NULL) != 0u) return 1;
  if (turbo_voice_get_state(NULL) != TURBO_VOICE_FAILED) return 2;
  return 0;
}
