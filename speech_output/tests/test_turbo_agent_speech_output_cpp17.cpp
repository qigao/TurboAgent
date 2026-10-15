#include "turbo_agent_speech_output.h"

#include <cstdint>
#include <type_traits>

static_assert(std::is_standard_layout<turbo_agent_tts_format_t>::value,
              "Public TTS format must remain C11-compatible");
static_assert(std::is_standard_layout<turbo_agent_tts_provider_v1_t>::value,
              "Public TTS Provider ABI must remain a plain C struct");

int main() {
  turbo_agent_tts_config_t config{};
  turbo_agent_tts_provider_v1_t provider{};
  turbo_agent_tts_config_init(&config);
  turbo_agent_tts_provider_v1_init(&provider);
  return config.abi_version == TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION &&
         config.output.sample_rate_hz == 24000u &&
         config.output.channels == 1u &&
         config.output.bits_per_sample == 16u &&
         config.output.frame_ms == 20u &&
         provider.abi_version == TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION ?
         0 : 1;
}
