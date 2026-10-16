# Combined source-TSan original Harness + both native Salts audio DSOs

Independent **test-only, default-OFF Draft** stacked on source-Capture
[#207](https://github.com/qigao/TurboAgent/pull/207) and upstream
SaltsUtils atomic admission [#653](https://github.com/qigao/salts-utils/pull/653).
Neither upstream nor this PR is merged or released.

The same original production Harness, Agent and Session from immutable
TurboAgent `9e51d377`, original M1/M2, original M3 C11 Whisper
Provider (model-free deterministic vendor C ABI), and current M4
Host approval / bounded PCM stage / loaded true pthread TTS Provider
all run under source-level GCC ThreadSanitizer.

**Difference from #206 and #207:** the SAME fullstack process now also
dynamically loads **both** source-instrumented original Linux native audio
shared libraries:

- `libsalts_playback.so` built from untouched SaltsUtils
  `v4.3.0-rc.11` real native PulseAudio/miniaudio backend PLUS actual
  Salts `v2.3.0-rc.17` SPSC atomic acquire/release implementation,
  all compiled under TSan.
- `libsalts_capture.so` built from exact original SaltsUtils rc11
  Linux dispatcher/video/screen/miniaudio sources, changing **only**
  `capture_audio_miniaudio.c` to exact Draft #653
  `4e1c77f8f9992c666c9ac0a77e9f0454a0b8b53a`, replacing the
  unsynchronized public state read with private C11 atomic PCM
  admission. No other source, public structure, or ABI change.

Both shared libraries must be visibly imported by the SAME executable
and resolved by `ldd` to these two source-built TSan images (not the
uninstrumented published DSOs); `nm -D` must reveal `__tsan_init`
and the actual original Capture/Playback symbols. Production source is
never overwritten, published or loaded into user devices.

A private PulseAudio `-n` server has one Unix-only null sink and one
monitor and no physical cards; generated PCM has no user audio/model
weights. The combined program exercises two repetitions, each with an
original Capture callback/stop+join, M3 one-shot checked FINAL, true
Harness/Agent completion and explicit Host authorization, real loaded
TTS worker, and genuine source-TSan native Playback backpressure/drain
or cancellation/clear with delayed worker settle and `dlclose`.

The independent true Harness source-TSan conformance runs four
repetitions; actual Whisper CPU no-weight model rejection runs twice.
`TSAN_OPTIONS=halt_on_error=1:exitcode=66:history_size=7`, **no
suppressions**. The previous released-binary #205 8/8 fullstack
acceptance and separate #206/#207 source-Tsan results remain valid
independently. A data race in this combined test is a hard failure.

**Not covered:** source-level TSan of external prebuilt Salts::Core,
CHttp, third-party PulseAudio/PipeWire and vendor Whisper.cpp CPU
libraries, genuine model inference/voice weights, human microphones
or speakers, permissions, real acoustic echo/AEC or cross-platform
physical audio. No fallback, second PCM queue or raw data persistence.

**DRAFT / DO NOT MERGE / DO NOT RELEASE; M4 issue #164 remains OPEN.**
