# M4.2 + original M3 Whisper C11 handoff (test-only)

This branch preserves the stacked M1/M2, actual bounded Harness/Session,
M4.0/M4.1/M4.2 and TTS Provider ownership contracts. It adds one **original**
Whisper M3 Provider source file, never a second application ASR queue or
replacement Agent execution interface.

## Exact ownership and version inputs

- Original M1/M2 + actual Harness/Session: TurboAgent exact source
  `9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`.
- Original real M3 C11 Whisper Provider: TurboAgent Draft #145 commit
  `4614360e6ee2e174d96cbdd9ce41c5388c81d064`.
- Reviewed real Whisper v1.9.5 public C/GGML header snapshot:
  `ggml-org/whisper.cpp@d1be6fde11ac6e0407606b4e42fe72d34add8037`.
- Current M4.2 bridge, Harness-approved FINAL gate and opt-in test code:
  source PR HEAD.
- Native dependencies remain published Salts and SaltsUtils SDKs, and the
  real Harness job's original official SDK resolver; no source fallback.

The CI fetches both original snapshots and asserts exact full commit SHAs
before any build. A separate C11 test compiles the **actual M3 Provider
source** against the published Salts::Core crypto target, original M1
SpeechInput and genuine upstream C ABI headers under Address+Undefined,
standalone Undefined and Thread sanitizers. The unchanged M3 #145
test-only fake engine supplies deterministic v1.9.5 symbols. Its
original C11 mock tests exercise SHA, cancellation, decode timeouts,
NO_SPEECH, stationary-noise threshold 49/50 and generation reset.
No model weight has been fetched or bundled.

An additional **real Harness execution test in one process** builds
original M1/M2 from source and compiles original M3 source and the
unchanged fake-Whisper-engine C ABI into that executable. The test creates
a short explicit fake model file through a safe, absolute, caller-owned
temporary path; it SHA-verifies the snapshot and unlinks the file after
model construction. A wrong digest must cause EINTEGRITY with no
Provider. During one generation 42, a synthetic real pthread Capture
callback (16kHz, mono S16LE) feeds original M2; M1 uses original M3
callback table to obtain a *model-free simulated* FINAL after stop/join.
Only a verified one-shot FINAL can enter original actual Harness/Agent
as **untrusted user input**. The Harness returns deterministic local
"ok" via a public mock transport. Only a completed run plus Host approval
admits canonical final text into the sole bounded M4.1 24kHz PCM staging
block and independent native drain. Zero PCM and input cancellation
must trigger no Harness, no TTS and no Playback writes; original M3
Owner cannot be destroyed while its utterance remains active.

## Explicitly NOT qualified

The original v1.9.5 **C ABI header is genuine**; the vendor inference
implementation in this branch is a TEST DOUBLE, not real Whisper model
acoustics. Previously separately authorized real tiny-model JFK tests
from #143/#144 remain historical evidence only and do not automatically
authorize fresh weights. No real model downloads, runtime model search,
microphone or speaker opens, user audio persistence, external Agent
network/tool calls, acoustic echo/AEC or true vendor-inference latency
claim. Real imported Salts::Capture / Playback negative ABI and separate
Linux TTS Provider DSO/TSan gates remain inherited; the full production
Harness itself is not built with source TSan in the focused real-Harness
job. Production defaults remain OFF.

Draft / DO NOT MERGE / DO NOT RELEASE; #134/#137/#164 remain OPEN.
