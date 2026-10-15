# SpeechOutput M4.0 — optional C11 fake TTS (DEFAULT OFF)

**DRAFT / DO NOT MERGE / DO NOT RELEASE.** Tracks
[design #164](https://github.com/qigao/TurboAgent/issues/164),
[design PR #165](https://github.com/qigao/TurboAgent/pull/165)
and [implementation Draft #171](https://github.com/qigao/TurboAgent/pull/171).

This subtree is a dependency-free, standalone C11 speech-output control ABI,
not a synthesizer, Playback device, or replacement for SpeechInput.
The ordinary TurboAgent build does NOT include it. No model is downloaded.

## Concrete ownership and lifecycle

* Config has checked ABI1 size, exact 24000 Hz mono S16 and 480 samples per
  20ms call. A last PCM block may contain fewer complete samples.
  Caller owns PCM, Provider owns its inference context; SpeechOutput owns no
  playback device or PCM queue.
* Create allocates one bounded approved-text buffer (1..4096 UTF-8 bytes
  configurable before create). Begin copies host-approved, user-visible
  plain text. Reject malformed/overlong UTF-8, NUL, controls/bidi, literal
  markup delimiters. Host explicitly selects bounded ASCII voice/language;
  no implicit remote model/voice/language fallback. The Provider may reject
  an unsupported combination. There is no automatic Agent/tool submission.
* Provider begin/poll_pcm/request_cancel callbacks are synchronous,
  non-reentrant and serialized on the host control owner. Provider context
  is borrowed through terminal settlement. It may use bounded asynchronous
  worker resources internally but must report settlement before unload.
* WOULD_BLOCK carries zero samples. PCM carries 1..480 host-owned samples
  and does not imply the audio reached Playback. TERMINAL (synthesis source
  exhausted) and POLL_CANCELED carry no PCM and certify no in-flight
  callbacks/workers for that generation.
* Cancel is idempotent and enters CANCEL_PENDING. A cancel request itself
  is NOT settlement. Poll continues; late frames are zeroed/discarded.
  Destroy remains EBUSY until Provider reports a settled terminal. A poll
  error or malformed output triggers one cancellation and only becomes
  FAILED after settlement. A vendor that never cooperates requires explicit
  process isolation; the C11 API does not promise hard interruption.
* SOURCE_DONE means synth generation done, NOT playback drained/heard.
  Future M4.1 uses existing SaltsUtils Salts::Playback and its own single
  writer: one bounded 960-byte staging frame, explicit short-write retry,
  a bounded 100ms ring, drain and producer quiescence before clear/destroy.
* Monotonic generation IDs reject stale/duplicate turns. Text is scrubbed
  at terminal and destruction; it is not stored as durable Agent state.

## Standalone build and regression gates

    cmake -S speech_output -B build/tts-fake -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build/tts-fake
    ctest --test-dir build/tts-fake --output-on-failure
    cmake --install build/tts-fake --prefix /tmp/tts-fake-install

The standalone installed C11 header/archive is for isolated consumer
acceptance, NOT part of the general TurboAgent.Native installed exports.
The exact installed SaltsUtils.Native Playback pin is a separate M4.1 gate.

The fake Provider deterministically emits a 480-sample frame, an 80-sample
tail and SOURCE_DONE. C11/C++17 and Sanitizer tests cover text ownership,
invalid ABI/format/UTF-8/markup, explicit locales, stale generations,
WOULD_BLOCK, repeated cancellation, late PCM discard, begin/poll failures
that remain CANCEL_PENDING, oversized/zero-count/malformed PCM, frame count
limits and callback reentrancy.

## Future phases (not authorized by M4.0)

* M4.1 — host-owned Salts::Playback null sink, one producer, short-write,
  bounded drain/clear, device failure and exact installed SDK conformance.
* M4.2 — half-duplex Voice Coordinator: checked ASR FINAL → approved
  Harness/Session text → TTS → Playback; explicit push-to-talk interrupt.
* M4.3 — separately approved/licensed TTS model engine with source/model
  hash, no automatic download, bounded memory and true DSO/worker settle.
* M4.4 — optional AEC/full duplex only after independent acoustic/echo tests.

This experimental branch never changes current ASR/Whisper, Agent tools,
main default CMake, or #150 Linux PraktorTools hidden hash_bytes link issue.
