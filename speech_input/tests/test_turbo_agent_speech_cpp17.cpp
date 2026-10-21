#include "turbo_agent_speech_input.h"
#include <type_traits>

static_assert(std::is_standard_layout<turbo_agent_speech_config_t>::value,
              "SpeechInput C11 options must have C++17 standard layout");
static_assert(std::is_standard_layout<turbo_agent_speech_provider_v1_t>::value,
              "SpeechInput Provider table must have C++17 standard layout");

int main() {
  turbo_agent_speech_config_t cfg{};
  turbo_agent_speech_provider_v1_t prov{};
  turbo_agent_speech_config_init(&cfg);
  turbo_agent_speech_provider_v1_init(&prov);
  if (cfg.abi_version != TURBO_AGENT_SPEECH_INPUT_ABI_VERSION ||
      prov.abi_version != TURBO_AGENT_SPEECH_INPUT_ABI_VERSION) return 1;
  turbo_agent_speech_input_t *out = nullptr;
  return turbo_agent_speech_create(&cfg, &prov, &out) ==
             TURBO_AGENT_SPEECH_EINVAL && out == nullptr ? 0 : 2;
}
