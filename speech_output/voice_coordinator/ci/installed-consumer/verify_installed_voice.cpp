#include "turbo_agent_voice_coordinator.h"
#include <salts_capture.h>
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
  /* Published Playback safely rejects bad format before opening any device. */
  salts_playback_config_t invalid_config{};
  invalid_config.sample_rate = 12345u;
  invalid_config.channels = 1u;
  invalid_config.format = SALTS_PLAYBACK_FORMAT_S16;
  invalid_config.buffer_duration_ms = 100u;
  salts_playback_t *native = nullptr;
  if (salts_playback_create(nullptr, &invalid_config, &native) !=
      SALTS_PLAYBACK_ERR_FORMAT || native != nullptr)
    return 5;
  salts_audio_capture_config_t invalid_capture{};
  invalid_capture.sample_rate = 12345;
  invalid_capture.channels = 1;
  invalid_capture.bits_per_sample = 16;
  invalid_capture.frame_size_ms = 20;
  if (salts_audio_capture_create(nullptr, &invalid_capture) != nullptr)
    return 6;
  const turbo_voice_coordinator_t *v = nullptr;
  return turbo_voice_get_generation(v) == 0u &&
         turbo_voice_get_state(v) == TURBO_VOICE_FAILED ? 0 : 1;
}
