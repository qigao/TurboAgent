# Original Salts native-audio lifecycle — explicit NULL-backend qualification

This standalone experimental C11 acceptance is **default OFF** and is
included only in this Draft branch's isolated Linux workflow. It never
changes the installed or production Salts, SaltsUtils, Capture, Playback,
M1/M2/M3, Harness or M4.0/M4.1/M4.2 target configuration.

## Verified source and ABI inputs

- **SaltsUtils release `v4.3.0-rc.11`**, original unmodified files:
  `playback/src/miniaudio_impl.c`, `playback_buffer.c` and
  `salts_playback.c`. This release already contains and tests the
  explicit `SALTS_PLAYBACK_TEST_NULL_BACKEND` compile switch.
- **miniaudio `0.11.22`** from the upstream tag selected by the release
  vcpkg baseline. No wrapper/shim of `ma_context_init` or substitute
  `salts_playback_*` is compiled.
- Original published, SHA/size-verified Salts.Native **2.3.0-rc.16**
  and SaltsUtils.Native **4.3.0-rc.11** SDK targets, particularly
  `Salts::Concurrency`/`Salts::Platform` and `Salts::Capture`.
- The **current HEAD** experimental M4.0 SpeechOutput, M4.1 Playback
  Bridge and M4.2 Voice Coordinator C11 source, directly compiled.
- Real, imported released `Salts::Capture` lives in a **separate**
  executable so the explicitly null-backed miniaudio implementation
  does not interpose on its native dynamic library.

## Real backend lifecycle qualified

The explicit null backend creates a real `ma_context`, `ma_device`,
native playback **callback thread**, and original bounded SPSC ring.
It sends no audible data to a speaker.

The test verifies public **24kHz mono S16** native device initialization,
a 10ms (480-byte) ring, actual native callback consumption/drain,
`start/pause/resume/stop` transitions and their synchronous Host state
notifications. It exercises M4.2 approved output with exactly one
480-sample/960-byte TTS frame: before the device starts, M4.1 accepts
only 480 bytes, retains the suffix, refuses to poll another TTS frame,
then resumes the original ring once callbacks start; only native
consumption and successful `drain` authorize voice COMPLETED.

The same original miniaudio native callback runs during M4.2
interruption: the original `salts_playback_clear` path must stop,
quiesce, clear and restart it; no stale buffer may survive a new input
generation. The unmodified upstream's **test-only failure hooks**
further require unexpected native stop and a stop-during-start race to
remain fatal, not to become false COMPLETED.

Separately, a C11 consumer imports the **official released**
`libsalts_capture.so`, enumerates through the actual OS device
context and rejects invalid sample format and invalid non-enumerated
microphone device IDs. It **never** opens `create(NULL, valid)` or
calls `salts_capture_start`; CI cannot grant physical microphone
permission. Lack of an audio server is recorded as a device-context
failure, NOT a physical microphone positive.

## Explicit unqualified behavior

- This NULL backend is an existing **test-only original source
  configuration**, **not** the published Playback DSO and not physical
  headphone/speaker permission or actual acoustic output.
- Released Capture tests real DSO enumeration and no-device refusal,
  **not** device start/stream/stop/join for a permitted microphone.
- M4 TTS PCM is deterministic test data, not real voice synthesis;
  acoustic AEC/barge-in, VAD/noise gating, model weights and real
  inference/authorization remain separately scoped.
- Full production Harness source is not source-TSan-instrumented in this
  acceptance; existing M3/M4/loaded-DSO source sanitizer gates must
  remain separately green.
- The entire branch and all stacked speech PRs remain
  **DRAFT / DO NOT MERGE / DO NOT RELEASE**. No model downloads or
  production default audio/network change.
