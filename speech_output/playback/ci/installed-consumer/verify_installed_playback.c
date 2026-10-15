#include "salts_playback.h"
#include "turbo_agent_tts_playback_bridge.h"
#include <stddef.h>

/* Only link/import published symbols; NEVER open a real device in CI. */
int main(void) {
  salts_playback_config_t c = {
    24000u, 1u, SALTS_PLAYBACK_FORMAT_S16, 100u
  };
  size_t (*capacity)(const salts_playback_t *) =
      &salts_playback_get_available;
  int (*write_pcm)(salts_playback_t *, const void *, size_t, size_t *) =
      &salts_playback_write;
  int (*clear_pcm)(salts_playback_t *) = &salts_playback_clear;
  int (*drain_pcm)(salts_playback_t *, uint32_t) = &salts_playback_drain;
  /* Invalid inputs touch no device, but force the real bridge object to
   * link and resolve salts_playback_write/clear/drain from installed SDK. */
  if (turbo_tts_playback_bridge_create(NULL, NULL, NULL) !=
      TURBO_TTS_PLAYBACK_EINVAL ||
      turbo_tts_playback_bridge_cancel(NULL) !=
      TURBO_TTS_PLAYBACK_EINVAL)
    return 2;
  return c.sample_rate != 24000u || c.channels != 1u ||
         c.format != SALTS_PLAYBACK_FORMAT_S16 ||
         c.buffer_duration_ms != 100u ||
         !capacity || !write_pcm || !clear_pcm || !drain_pcm;
}
