# M4.2 Host final-handoff conformance (test-only)

This fixture links the original **M1 SpeechInput + M2 CaptureBridge** from
[TurboAgent #136 exact commit `9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`](https://github.com/qigao/TurboAgent/commit/9e51d377bd84ee91ce5a47c94f24f73e4e01bba7),
with current-HEAD M4.0 SpeechOutput, M4.1 Playback Bridge, and M4.2
Voice Coordinator. The original sources are checked out in CI by full SHA,
compiled with the same C11 sanitizer flags and **never vendored/copied**
into the output branch.

The M2 bridge consumes only 16 kHz mono S16LE 320-sample / 640-byte
borrowed microphone callback frames. It immediately copies them to M1's
existing bounded queue. M4.0 output uses **different** explicit 24 kHz mono
S16 480-sample / 960-byte frames. No resampler, fallback, raw PCM
persistence or second device owner is introduced.

## Host admission ordering

1. Begin SpeechInput M1 + CaptureBridge M2 and Voice Coordinator on the
   same generation under one serialized Host owner.
2. A real Linux pthread executes a borrowed capture callback; the test
   intentionally reuses its input buffer as soon as the offer returns.
   The M1 provider emits a speculative PARTIAL, visible to UI only.
3. The Host must call M2 stop-admission, stop the native capture source
   and **join its callback thread** before invoking M2 finish or telling
   Voice Coordinator that input is quiescent. The fixture uses an actual
   pthread_join; real Salts Capture stop/permission is not invoked here.
4. Only a completed M2 finish followed by the original M1 pump and a
   successful **one-shot** `turbo_agent_speech_take_final()` with nonempty
   FINAL may authorize a `CHECKED_FINAL` Host event. Duplicate or stale
   generations fail; NO_SPEECH and any Capture byte-count/capacity error
   fail closed.
5. The checked ASR final text remains untrusted; it does **not**
   authorize Agent tools, direct Session invocation or TTS. Only a
   separately supplied, Host-approved **simulated** committed
   user-visible Harness FINAL begins SpeechOutput. No actual Harness
   run occurs in this fixture.
6. M4.1 still owns the sole 480-sample staging block, preserves
   short-write FIFO under backpressure and requires independently
   successful ring drain before Voice Coordinator completes.

## Release consumer and explicit exclusions

A separate installed C11 and C++17 consumer checks SHA-pinned official
SaltsUtils 4.3.0-rc.11 and Salts 2.3.0-rc.16 **Capture and Playback**
target/link identities. It invokes their unsupported 12345 Hz PCM
configuration failures, which are validated in the exact release source
before native miniaudio context or device initialization. No microphone
or output device is opened.

The inherited optional Linux DSO test independently proves real
`dlopen`, a TSan-instrumented pthread Provider worker, cancel-first,
finish-first, settle/join and `dlclose`. The Host handoff fixture does
**not** claim real model recognition, a production Session/Harness FINAL,
hardware permissions, acoustic AEC, a real native Playback null/physical
device, noncooperative process kill, or other OS native DSO results.

Default option `ENABLE_TTS_HOST_HANDOFF_TEST=OFF`. All involved speech
PRs are **DRAFT / DO NOT MERGE / DO NOT RELEASE**, ordinary TurboAgent
native SDK and Agent defaults unchanged.
