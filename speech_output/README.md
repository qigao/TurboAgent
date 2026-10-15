# TurboAgent SpeechOutput M4.0 — opt-in C11 fake TTS

**DRAFT / DO NOT MERGE / DO NOT RELEASE. DEFAULT OFF.**

Tracks [design #164](https://github.com/qigao/TurboAgent/issues/164)
and [M4.0 #166](https://github.com/qigao/TurboAgent/issues/166).
This implements the independent output C11 ABI and deterministic fake Provider
only. No real TTS weights, model, native sound device, microphone, Agent
execution or cloud endpoint is enabled.

## Standalone (no Salts dependency)

    cmake -S speech_output -B build/speech-output -G Ninja -DBUILD_TESTING=ON
    cmake --build build/speech-output
    ctest --test-dir build/speech-output --output-on-failure
    cmake --install build/speech-output --prefix /tmp/tts-m4-install

The integrated root option **ENABLE_SPEECH_OUTPUT** defaults OFF.
The standalone installed public header and archive are independently
linkable from strict C11 and C++17. GCC/Clang, TSan and ASan/UBSan CI
checks run actual native code, not only compile guards.

## Ownership and staging semantics

1. The host has already selected **approved user-visible plain text**. TTS
   cannot inspect Agent tools, speculative SSE or ASR PARTIAL. begin() copies
   up to 4096 validated UTF-8 bytes; its Provider only borrows that copy
   until the generation reaches a true settled terminal.
2. Provider callbacks and begin/pump/cancel/destroy are serialized on one
   caller-controlled thread. Monotone generation IDs reject stale writes.
   Provider context and any DSO image remain owned by the host.
3. Exactly one **480-sample / 960-byte** S16 mono staging block exists per
   output object (20ms at 24kHz); a final block may have 1..480 samples.
   peek_pcm() borrows its unconsumed suffix, consume_pcm() advances only
   by samples that a future sole Salts Playback producer has **actually
   accepted**. Short writes are retried, never dropped or reordered. The
   native audio callback must never invoke Provider, Agent or network APIs.
4. **STAGING_DRAINING is not native PLAYBACK_DRAINING.** SETTLED means the
   Provider has finished and all staged PCM has been accepted by the host
   output writer. A future Voice Coordinator must separately wait for
   Salts Playback ring drain before claiming ring consumption. A human
   hearing the resulting sound is not verifiable by either operation.
5. cancel() immediately discards host staging and prevents new output.
   Provider cancellation may complete later; late PCM is dropped.
   destroy() stays EBUSY until a terminal Provider acknowledgment, so no
   borrowed input, worker or DSO can be prematurely unloaded.
6. TEXT/PCM/voice fields are wiped at terminal/destroy. Raw samples are not
   durable state and there is no second PCM queue or fallback audio device.

## Explicit format/permission rules

- Exact selected format: 24000Hz / 1 channel / S16 / 20ms, not 48k
  stereo. Mismatch returns explicit EUNSUPPORTED; **no implicit resampler,
  remote fallback or voice substitution**.
- Input text is scalar UTF-8 with exact declared byte count. Embedded NUL,
  malformed UTF-8, C0/C1, DEL, unsafe bidi overrides and SSML-like angle
  markup are rejected before Provider.begin. Voice identifiers are bounded
  ASCII and language must be explicitly one of en/ja/zh.
- Audio duration 20..30000ms in whole 20ms increments. One pending 960-byte
  block plus a proposed future 100ms Salts Playback logical ring of 4800
  bytes bounds host PCM to 120ms; real provider-internal memory needs its own
  bound. An approved text producing **zero PCM** fails closed, not success.
- WOULD_BLOCK is EAGAIN, not a spin loop. A malformed Provider frame or
  over-capacity result begins cancellation and retains its Provider until
  an actual settled terminal is reported. This does not hard-preempt a
  non-cooperating vendor: M4.3 must prove process isolation if required.

## Collaboration and next gates

M4 MVP is **half-duplex**: Capture/ASR checked FINAL → host-approved
Harness/Session Agent turn → host-selected displayable assistant FINAL
→ TTS Provider → one Salts Playback writer → drain → new Capture turn.
Push-to-talk can explicitly interrupt TTS after producer/provider settlement,
quiesce the writer, clear Playback PCM, and start a new ASR generation.

M4.1 is separately responsible for the **installed** SaltsUtils Playback
export/Registry pin, null-device backpressure/clear/drain and device error
checks. M4.2 coordinates ASR/Agent/TTS without a second owner or tool
authority. M4.3 requires independent permission/voice/model and true
DSO drain/unload proof before any actual TTS engine. Automatic full-duplex
barge-in is a later M4.4 AEC + real-echo qualification, not implied by the
currently incomplete #146 ASR acoustic results.

This README is NOT an authorization to merge, enable a production provider,
download model weights, clone a voice, open an output device or publish a
release.
