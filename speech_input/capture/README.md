# Speech Capture M2 — opt-in native adapter for the bounded C11 input core

**No native microphone is opened in the deterministic tests.** M2 supplies
two explicit layers under SpeechInput's unchanged single PCM queue:

1. `TurboAgent::SpeechCaptureBridge`: standalone C11 `uint8_t*` borrowed
   capture callback adapter. It validates **exactly 640 bytes**, decodes
   little-endian signed S16 mono/16kHz/20ms into one aligned stack frame,
   then copies that frame into the existing bounded SpeechInput queue.
   There is no second buffering queue, no inference and no Provider callback
   from the capture thread.
2. `TurboAgent::SpeechCaptureSalts`: separately opted-in, native
   `Salts::Capture` device owner. It calls the existing public
   `salts_audio_capture_create`, `set_callback`, `start`, `stop` and
   `destroy` APIs with exact 16kHz, 1-channel, 16-bit, 20-ms format.

## Selection — never a default dependency

Default TurboAgent builds remain unchanged. Opt-in M2 bridge independent of
any Salts native SDK (useful for deterministic tests, producer integration):

```sh
cmake -S speech_input -B build/speech-m2 \
  -DBUILD_TESTING=ON -DENABLE_SPEECH_CAPTURE_BRIDGE=ON
cmake --build build/speech-m2
ctest --test-dir build/speech-m2 --no-tests=error --output-on-failure
```

To link the actual native microphone device (only on a platform/SDK that
exports `Salts::Capture`), configure the full TurboAgent repository with:

```sh
cmake -S . -B build/native-voice -DENABLE_SPEECH_INPUT=ON \
  -DENABLE_SPEECH_CAPTURE_NATIVE=ON
```

A requested native target **must fail CMake configuration** when the
installed SaltsUtils SDK does not export `Salts::Capture`. The code must
never fetch/bundle a second miniaudio or silently open another audio backend.

## Strict callbacks and lifecycle

The integration remains **caller-driven**; no new executor, thread, Session,
Tool Authority owner or Provider lifetime exists.

- The host creates/retains one SpeechInput and chooses an explicit
  `utterance_id`. The optional native adapter creates one Salts::Capture
  device and registers its callback.
- `salts_capture_start` begins native capture *after* the same SpeechInput
  Provider's `begin`. If the device fails to start, it is stopped/drained
  before Provider cancellation, with an explicit device error.
- The native callback passes only a borrowed `uint8_t` PCM view to the C11
  bridge. The bridge copies 320 signed samples, has an atomic first-error
  cell and a callback in-flight counter, and **never** allocates, blocks,
  calls a model or stops the native device.
- `EBUSY`, `ECAPACITY`, invalid PCM length/monotonic timestamp and stale
  generation are **fail-closed**. The control path must stop native capture
  and cancel rather than treat dropped microphone data as a valid final
  transcript.
- To complete: **stop bridge admission → native `salts_capture_stop` /
  callback drain → request SpeechInput finish → caller-driven Provider
  `pump()` → one FINAL or NO_SPEECH**. Destroy the native Capture only
  after the callback has settled; SpeechInput/Provider is borrowed and
  managed independently by the existing host owner.
- Only the host may submit an explicitly accepted FINAL to
  `turbo_agent_harness_start_text()`. PARTIAL remains display-only and
  no audio or transient transcript is durable Session/Journal state.
- Device IDs must be exact enumeration results; NULL or empty **explicitly**
  requests the native default device. Unknown/invalid IDs and permissions
  fail at the exact `salts_audio_capture_create/start` boundary.

## Evidence and remaining work

- M2 bridge deterministic tests exercise endian/signed sample conversion,
  borrower buffer reuse, frame-length mismatch, full queue, first-error
  winner, late callback refusal, stop-before-finish, cancel idempotence
  and absence of FINAL after lost/invalid frames.
- Linux/macOS/Windows strict C11 + source ASan+UBSan/TSan CI cover the
  bridge without needing physical devices. These tests are **not** hardware
  microphone/permission qualification.
- Native Salts capture target still needs a real installed SDK that
  exports `Salts::Capture` and platform-specific runner/hardware or
  permission tests. It also needs actual DSO stop/drain and permission
  negative conformance before integration approval.
- Whisper.cpp, SenseVoice, sherpa-onnx and cloud ASR are deliberately
  **not** loaded in M2. M3 qualifies one chosen, pinned optional model
  Provider separately.
