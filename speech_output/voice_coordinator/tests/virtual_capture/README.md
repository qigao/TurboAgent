# Released Salts Capture native callback: isolated virtual PulseAudio source

**Opt-in test only / DRAFT / DO NOT MERGE / DO NOT RELEASE.**
The default TurboAgent audio/M1/M2/M3/M4/Harness builds remain unchanged.

This is **not** a fake \`salts_capture_*\` implementation: the test imports
the actual SHA/size-pinned released \`Salts::Capture\` DSO from SaltsUtils
4.3.0-rc.11 (with Salts 2.3.0-rc.16) and directly compiles immutable
TurboAgent original M1 SpeechInput, M2 CaptureBridge and M2's native
\`turbo_agent_speech_capture_salts.c\` adapter (SHA
\`9e51d377bd84ee91ce5a47c94f24f73e4e01bba7\`).

## Security/permission isolation

The sole audio service is a **dedicated, short-lived, private** PulseAudio
daemon started with \`-n\`, no OS default modules, with only:

- one UNIX-local \`module-native-protocol-unix\` socket in a mode-0700
  temporary directory, no TCP/network;
- one software \`module-null-sink\` named \`ta_ci_sink\`, whose monitor
  \`ta_ci_sink.monitor\` is the only audio *source*;
- a bounded producer streaming synthetically generated DIGITAL ZERO to
  that null sink, never actual sound hardware.

Before any \`salts_capture_start\` or native M2 creation, the script
asserts exactly one source, exactly one null sink, **zero physical cards**,
and explicitly makes the monitor the only default source. The C11 test
**refuses to run** without \`M42_VIRTUAL_PULSE_ONLY=1\`, an explicit
\`PULSE_SERVER=unix:...\`, exactly one Salts audio device, and its native
description containing \`TurboAgentVirtual\` or \`ta_ci_sink\`. It passes
the exact enumerated device ID. The test NEVER calls
\`salts_audio_capture_create(NULL, valid)\` or accepts a device fallback.

The virtual monitor provides borrowed 16k mono S16LE PCM; callback bytes
are never persisted. The original M2 adapter requires exact 20ms/640-byte
native periods; different native packet sizes are a **hard error**, not
implicitly resampled/coalesced/truncated.

## True released Capture lifetime

1. Open exact virtual source through published \`salts_audio_capture_create\`.
   Start actual miniaudio Capture callback. Observe callbacks via C11
   atomics and timestamp order; stop and wait for native thread exit.
   Verify no callbacks arrive after stop (explicit bounded observation),
   then destroy the released native handle.
**Stop/callback interleaving (additional strict gate):** the real published
Capture callback deliberately blocks behind a timed C11/POSIX condition
variable while a separate serialized control operation calls the published
`salts_capture_stop`. The host first proves stop **has not returned**
while the callback is still borrowing native data. It then explicitly
releases the callback, joins the stop worker, checks both callbacks and
stop are fully settled and verifies no subsequent callback can occur.
The callback watchdog releases on timeout rather than forcing DSO
destruction, but the test still fails. This uses original `ma_device_stop`,
not an adapter-simulated stop or a synthetic pthread standing in for the
OS audio callback.

2. Create the original M1 bounded queue and immutable original M2 Salts
   Native Adapter using the same exact virtual-source ID. Real native
   callback immediately copies borrowed frames to M1; fake ASR Provider
   is invoked only by the serialized Host \`pump()\`, never by the native
   callback. Verify active \`destroy\` refuses; \`finish\` closes admission
   before native stop/join; then the original M1 yields **one** checked
   FINAL and rejects its duplicate. Original Capture and M1 handles are
   released only after all callbacks have settled.
3. In a separate generation, the original M2 Native Adapter's cancel
   closes callback admission and stops the true native Capture before
   M1 Provider abort. No final transcript or new worker is produced.

20 consecutive CTests and a post-run ELF provenance audit must pass.
The executable must have \`NEEDED libsalts_capture.so\`, import the actual
\`salts_capture_start/stop/salts_audio_capture_create\`, and define
original \`turbo_agent_speech_salts_capture_create\`,
\`turbo_agent_speech_capture_bridge_offer\` and
\`turbo_agent_speech_offer_frame\`. An unresolved runtime dependency,
inconsistent PulseAudio source identity or callback-after-stop fails CI.

## Explicitly open gates

A PulseAudio *virtual monitor* is **not a real microphone**, nor a
physical device permission grant. A synthetic zero input and test-only
deterministic ASR completion do not qualify human-voice recognition,
Whisper weights/VAD, realistic background noise, echo cancellation or
barge-in. These native Capture tests do not open a real speaker and
do not claim real TTS. The full original Capture DSO is prebuilt; only
the original M1/M2 test sources are compiled with local sanitizers, so
this is NOT a source-TSan qualification of the released Capture DSO.

No production backend fallback, new PCM queue, raw recording storage,
model download, merge, release or default-enable is authorized.
