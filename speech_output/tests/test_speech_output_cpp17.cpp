#include "turbo_agent_speech_output.h"
#include <type_traits>
static_assert(std::is_standard_layout<turbo_tts_provider_t>::value, "C11 provider ABI must be C++17-friendly");
static_assert(TURBO_TTS_ABI_VERSION==1u,"ABI");
static_assert(TURBO_TTS_SAMPLES_PER_FRAME==480u,"24kHz mono 20ms");
int main() {
 turbo_tts_provider_t provider{};
 turbo_tts_provider_init(&provider);
 return provider.struct_size==sizeof(provider)&&provider.abi_version==1u ? 0:1;
}
