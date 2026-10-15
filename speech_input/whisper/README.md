# SpeechWhisper M3a — optional offline Whisper.cpp C API

**Draft, not yet a working voice assistant.** This is an explicitly selected
C11 Provider DSO; ordinary TurboAgent SDKs retain no Whisper, GGML, model
weight, C++ inference or native microphone dependency.

The upstream source is exactly `ggml-org/whisper.cpp v1.9.5` at
`d1be6fde11ac6e0407606b4e42fe72d34add8037`. The default build never
downloads inference source or model files. With a separately installed
CPU-only Whisper v1.9.5 SDK and an installed Salts CMeta runtime:

```sh
export SALTS_ROOT=/path/to/Salts.Native/v2.3.0-rc.16/sdk/linux-x64
export TURBO_WHISPER_SDK_ROOT=/path/to/pinned/whisper-1.9.5-sdk
cmake -S speech_input -B build/whisper-m3 \
  -DBUILD_TESTING=ON -DENABLE_SPEECH_WHISPER=ON
cmake --build build/whisper-m3
ctest --test-dir build/whisper-m3 --no-tests=error --output-on-failure
```

Full TurboAgent enables `ENABLE_SPEECH_INPUT=ON` plus
`ENABLE_SPEECH_WHISPER=ON`. Absent/mismatched C API SDK or Salts CMeta
capability **fails configuration**. The Provider DSO is not exported in
ordinary TurboAgentTargets and requires explicit host lifecycle admission.

## Model integrity and memory

The host supplies the **absolute approved local model path**, a full
**32-byte SHA-256 digest** for the exact file contents, language
(EN/ZH/JA/explicit AUTO), CPU threads 1–16, max 1–3000 utterance frames,
and 1–4096 transcript bytes. Default model limit is 128 MiB; caller may
explicitly raise it up to 1 GiB. Unknown weights, relative paths, missing
or oversized files and SHA mismatch fail before model init, with no model
download or fallback to another model.

The C11 Provider hashes a single bounded file snapshot via existing
Salts `cmeta_sha256_stream_*`, and passes those **same verified bytes**
to `whisper_init_from_buffer_with_params`. It retains the model image until
Whisper context teardown. This prevents a hash-then-reopen file replacement
race but increases peak RAM by the explicitly approved model file size.

Exactly one Provider owns one Whisper context, one bounded PCM float32 tensor
and one transcript buffer. Max 3000 × 320 samples = **3.84 MiB** float32;
one original fixed M1 capture queue remains the only audio input queue.
`accept_frame` converts 320 S16 samples into [-1,1) float on the caller's
control executor, not the microphone callback; `whisper_full` is invoked
once in synchronous `finish`. GPU, cloud upload, other CPU backend fallback
and durable raw PCM remain absent.

## Cancellation, text, and authority

Whisper v1.9.5 exposes a real abort callback. Only
`turbo_agent_speech_whisper_request_abort()` may be called concurrently,
setting an atomic flag. The host must await the caller-driven Provider
`finish()` return, settle M1 SpeechInput and its DSO callback lease, then
destroy/unload. `destroy` returns EBUSY while active or decoding.

Whisper segments must fit the configured text bound **without truncation**.
M1 validates strict UTF-8 and allows one FINAL only. All-zero digital PCM
returns explicit NO_SPEECH without decoding; this is **not acoustic VAD**.
Background-noise hallucinations and streamed voice turn-taking remain
separate quality gates. PARTIAL/model-proposed instructions are untrusted
data, never a Tool Authority or automatic Agent submission.

## What CI does and does not prove

The real pinned upstream C++ inference SDK is built separately CPU-only.
Tests link the **real Whisper v1.9.5 C API** and verify invalid/mismatched
model bytes, SHA, size and ABI errors without any model weights. A separate
deterministic mock engine compiles the **same production C11 Provider**
against the original `whisper.h` and exercises S16→float, M1 final
text admission, silence and cross-thread abort.

A green model-free test is **not** acoustic ASR accuracy: positive real
Chinese/Japanese/English speech, CER/WER, latency/RSS, model license and
weights SHA, and real microphone permission/DSO unload remain explicitly
OPEN until a legally approved model fixture and platform tests exist.
Source license is upstream MIT; weights are a **separate** artifact/licensing
decision. No model files are bundled by this PR.
