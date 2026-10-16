# Full same-process actual Harness/TTS DSO with BOTH shipped Salts native devices

**DRAFT / DO NOT MERGE / DO NOT RELEASE; all defaults OFF.**

This independent opt-in Linux CI is stacked on #204. It intentionally
qualifies a single process and same generation across the previously
separately qualified lanes:

1. Actual published SaltsUtils.Native v4.3.0-rc.11 **Salts::Capture**
   device callback from a **private single-null-sink PulseAudio monitor**.
   Immutable original TurboAgent M2 native Capture adapter (SHA
   \`9e51d377...\`) closes admission, stops and joins native Capture before
   original M1 caller-driven bounded queue/pump.
2. Actual immutable M3 **C11 Whisper Provider** source (SHA
   \`4614360e...\`) with a short SHA-verified temporary **test model** and
   original reviewed v1.9.5 **fake Whisper C ABI implementation**. It
   emits exactly one checked nonempty \`" utterance"\` FINAL. This is NOT
   genuine model inference or voice recognition.
3. **Original full production TurboAgent::Harness/Agent/Session DSO**
   built from immutable source and 1-worker/1-slot original CMeta
   threadpool, using a deterministic local transport returning \`"ok"\`.
   The original strict C11 Host policy verifies real run state, result,
   one-shot result ownership, matching run ID and **explicit Host
   authorization**, never speaking speculative/ASR/tool text.
4. A genuine \`dlopen\`-loaded C11 **TTS Provider DSO** with a true pthread
   worker. PCM is explicitly synthetic 480-sample S16 mono 24k. The
   single original current-HEAD M4.0/M4.1/M4.2 staging/writer passes
   through actual published **Salts::Playback** DSO using the same
   verified isolated Pulse null sink, with 10ms native 480-byte ring
   backpressure. Completion requires genuine native callback consumption
   and \`salts_playback_drain\`, then worker settle/join, Provider Owner
   release and one actual DSO \`dlclose\`.
5. Independent cancellation case runs the same actual M1/M2/M3/Harness
   admission and loaded TTS worker with the actual native Playback DSO
   active, then verifies original device clear/stop/restart, rejected
   late PCM, no premature destruction/unload or future generation, real
   TTS settle/join, and only then **one** DSO unload observer event.

Both **published** audio libraries must occur in the resulting executable
ELF \`DT_NEEDED\`, along with the real production Harness DSO. The
TTS Provider DSO must be loaded only through \`dlopen\`, **not**
\`DT_NEEDED\`. The executable must import the published
\`salts_capture_start/stop\`, \`salts_playback_create/start/clear/drain\`,
and original \`turbo_agent_harness_start_text\`, while locally defining
the original M2 native Capture adapter, the original M3 C11 entry point,
and current M4 authorization policy. The workflow repeats this complete
program eight times inside a single dedicated, short-lived, private
PulseAudio software null sink + monitor, never opening physical devices,
and separately runs all inherited original M3/M4 sanitizers and real
Harness/DSO gates unchanged.

A *different* published audio SDK set is not silently substituted:
the real Harness workflow's original official SDK resolver selects
Salts.Native v2.3.0-rc.17, SaltsUtils.Native v4.3.0-rc.11,
CHttp.Native v2.1.0-rc.6, while #204's independent released dual-DSO
consumer explicitly remains on Salts rc.16 / SaltsUtils rc.11.
Both use original released Salts::Capture and ::Playback targets.

This test **does not** prove real Whisper weights, real human speech,
natural voice synthesis, acoustic AEC/barge-in, physical microphone/
speaker permission, cross-platform real audio or full-source production
Harness/Capture/Playback TSan. Independent source-instrumented M3/M4 and
loaded TTS worker sanitizer tests remain separate; the original full
Harness and published SDK libraries here are Release native binaries,
not full-source TSan builds.

No model downloading, no fallback, no raw PCM persistence, no SDK
publication, no production backend switch or merges. #164 remains OPEN.
