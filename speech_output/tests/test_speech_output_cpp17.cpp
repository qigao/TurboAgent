#include "turbo_agent_speech_output.h"
#include <type_traits>

static_assert(std::is_standard_layout<turbo_tts_provider_t>::value,
              "Public SpeechOutput Provider ABI must be C++17 standard layout");
static_assert(std::is_standard_layout<turbo_tts_config_t>::value,
              "Public SpeechOutput config ABI must be C++17 standard layout");
static_assert(TURBO_TTS_ABI_VERSION == 1u, "SpeechOutput ABI");
static_assert(TURBO_TTS_SAMPLES_PER_FRAME == 480u, "24kHz mono / 20ms");

int main() {
  turbo_tts_provider_t provider{};
  turbo_tts_config_t config{};
  turbo_tts_provider_init(&provider);
  turbo_tts_config_init(&config);
  return provider.struct_size == sizeof(provider) &&
         provider.abi_version == 1u &&
         config.struct_size == sizeof(config) &&
         config.abi_version == 1u &&
         config.format.sample_rate_hz == 24000u &&
         config.format.channels == 1u &&
         config.format.bits_per_sample == 16u &&
         config.format.frame_samples == 480u ? 0 : 1;
}
