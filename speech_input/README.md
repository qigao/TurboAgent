# SpeechInput (M1) — optional C11 speech-to-text admission boundary

**Status:** deterministic input infrastructure only. This is **not** a working
Whisper/SenseVoice microphone client. No model runtime, audio device access,
VAD, remote transport, Session/Tool invocation or background worker is bundled.

The module exists to admit audio as untrusted input without changing any
existing TurboAgent/AgentCompiler/RuntimeTools ownership or capability rules.

## Build

Normal TurboAgent has no speech dependencies and **does not build this module**.

To opt in alongside normal TurboAgent dependencies:

```sh
cmake -S . -B build/voice -DENABLE_SPEECH_INPUT=ON -DBUILD_TESTS=ON
cmake --build build/voice --target turbo_agent_speech_input
```

The installed CMake target, when enabled, is \`TurboAgent::SpeechInput\`.

For **strict standalone C11** tests without Salts/CHttp/Whisper/C++ or
third-party packages:

```sh
cmake -S speech_input -B build/speech-m1 -DBUILD_TESTING=ON
cmake --build build/speech-m1
ctest --test-dir build/speech-m1 --no-tests=error --output-on-failure
```

The separate CI workflow exercises Linux/macOS/Windows builds and Linux
source ASan+UBSan/TSan. Tests use a deterministic mock ASR Provider only.

## Format and bounded ownership

- The initial accepted audio profile is **exactly 16,000 Hz, signed 16-bit
  little-endian PCM, one channel, 320 samples / 20 ms**. Reject any other
  format instead of silently resampling, truncating or opening another device.
- The caller configures a queue of 1–128 frames; no dynamic queue growth.
  Each frame is owned only by one \`SpeechInput\` instance. The hard utterance
  limit is 1–3000 accepted frames (maximum 60 s).
- \`offer_frame(input, utterance_id, pcm, 320, timestamp_us)\` is the only
  cross-thread interface. It **copies** 640 bytes under a try-lock, returns
  \`EBUSY\` on lock collision and \`ECAPACITY\` when full. It performs no
  inference, networking, allocation or blocking wait in capture callbacks.
- \`begin\`, \`pump\`, \`request_finish\`, \`cancel\`, result reads and
  \`destroy\` are caller-serialized on the host's existing control executor.
  \`pump\` runs the selected Provider **synchronously on the caller** with
  a frame budget, without introducing another execution Owner.
- Monotonically increasing \`utterance_id\` and PCM timestamps prevent
  acceptance of stale audio from a previous generation.
- The host **stops Salts::Capture and drains callbacks before**
  \`request_finish()\` and always before \`destroy()\`. A non-terminal object
  cannot be destroyed: \`destroy\` returns \`EBUSY\` without releasing memory.
- Provider context, callbacks and any future DSO lease are borrowed. Keep
  the Provider alive across begin/pump/finish/cancel and until destroy.
  Real Provider lifecycle/DSO unload belongs to the later M2/M3 gate.
- No raw PCM, runtime pointers or tentative transcription persist in
  Agent Session/Journal state. The queue is erased on dequeue/cancel/error.

## Text safety and tool authority

\`SpeechProviderV1.accept_frame\` may produce a **PARTIAL** UTF-8 snapshot.
The core copies it after validating UTF-8, length (max 4096) and Unicode
scalar encoding. PARTIAL is **display only** and cannot be taken as FINAL.

After capture is stopped, \`request_finish\` and \`pump\` commit exactly
one **FINAL** or **NO_SPEECH** outcome. \`take_final\` copies a bounded owned
UTF-8 string exactly once; an undersized destination does not consume it.

**SpeechInput never calls the Agent or a tool.** The external trusted host
must separately obtain and approve a FINAL transcript before calling
\`turbo_agent_harness_start_text()\` or Session admission. The existing
Tool Authority, capability checks and approval rules remain the only
tool-execution authorization path. Transcripts (including voice commands
and instructions inside speech) remain **untrusted data**.

Any Provider error, malformed UTF-8, duplicate final or canceled utterance
fails closed: no previously shown partial becomes a command. No automatic
fallback from offline inference to cloud transcription.

## Deferred integration

Next independent slices under [#134](https://github.com/qigao/TurboAgent/issues/134):

1. M2: \`Salts::Capture\` adapter with explicit device/permission and 16kHz
   PCM contract, then stop→callback drain→SpeechInput settle; native hardware
   permission/device tests are distinct from deterministic mock tests.
2. M3: optional local SenseVoice / Whisper Provider DSO or process boundary;
   inference engines with C++ internals **must not** become a dependency of
   the core C11 module or ordinary TurboAgent SDK. Check runtime/model licenses
   and pinned weight hashes before packaging.
3. M4: explicit-consent remote ASR over CNet/CHttp; no automatic cloud upload.
4. M5: opt-in CLI push-to-talk and final transcript preview/approval, preserving
   existing Session and AgentCompiler admissions.

M1 changes no existing Agent, Harness, CLI or native capture behavior by default.
