#include "turbo_agent_voice_coordinator.h"

/* Exercise exact current-head archived symbol resolution against real
 * published Salts::Playback without creating/starting an audio device. */
int main(void) {
  if (turbo_voice_get_generation(NULL) != 0u) return 1;
  if (turbo_voice_get_state(NULL) != TURBO_VOICE_FAILED) return 2;
  return 0;
}
