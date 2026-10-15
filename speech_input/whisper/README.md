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

Whisper v1.9.5 exposes a real abort callback. Each inference has a
caller-configured **monotonic time budget** (`max_decode_ms`: 1–120,000 ms,
default 30,000 ms). The Provider itself stops at the deadline when Whisper
next polls its callback; **this is cooperative, not forced thread termination**.
Only `turbo_agent_speech_whisper_request_abort()` may additionally be called
concurrently, setting an atomic flag. The host must still impose an outer
execution/watchdog deadline for engine operations that fail to poll abort.

The host must await the caller-driven Provider `finish()` return, settle M1
SpeechInput and its DSO callback lease, then destroy/unload.
`destroy` returns EBUSY while active or decoding. An expired deadline
produces an error terminal, never a partially published FINAL.

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


## Preview-only local WAV CLI — opt in

An explicit file-to-text entrypoint is available for real offline ASR **only**
when the operator already has a locally authorized Whisper GGML model and its
exact SHA-256. This command **does not open a microphone, create an Agent
Session, call tools, authorize commands, perform network access, select another
model or download weights**.

Enable the CLI with the existing optional isolated Whisper provider and
the M2 bounded S16LE bridge:

```sh
cmake -S speech_input -B build/whisper-cli \
  -DENABLE_SPEECH_WHISPER=ON \
  -DENABLE_SPEECH_CAPTURE_BRIDGE=ON \
  -DENABLE_SPEECH_WHISPER_CLI=ON \
  -DBUILD_TESTING=ON
cmake --build build/whisper-cli --target turboagent-whisper-transcribe
build/whisper-cli/whisper/turboagent-whisper-transcribe \
  --model /absolute/approved/model.ggml.bin \
  --sha256 EXACT_64_HEX_SHA256 \
  --wav /absolute/approved/utterance.wav \
  --language zh \
  --threads 2 --timeout-ms 30000 --model-max-mib 128
```

Accepted WAV: RIFF/WAVE, PCM format 1 (no float/compression), signed
**16-bit little-endian**, exactly **16,000 Hz mono**, byte rate 32,000,
block align 2, an exact RIFF length, and a final `data` chunk with **whole
640-byte / 20ms audio frames**. Max 60s, no sample truncation, implicit
resampling, automatic channel selection, trailing data, or model fallback.
Other metadata chunks before `data` may be skipped if they are bounded.
Malformed/missing WAV returns a separate explicit failure before loading
the model. Paths must be absolute and authorization is the caller's duty.

The command pumps one 20ms frame through M2 and M1 at a time, bounding queue
admission; only FINAL is written to stdout. PARTIAL and engine errors cannot
become a transcript. NO_SPEECH produces no stdout. On failure, no transcript
is admitted; this is a preview for operator review, **not** an Agent input.

`--language` is required and must explicitly select `en`, `zh`, `ja`
or `auto`; optional CPU threads 1..16, cooperative decode deadline
1..120000ms, and explicit maximum model MiB 1..1024. Decode deadline
is cooperative, not forced interruption of non-polling vendor code.

The dedicated CI builds **real** reviewed Whisper.cpp v1.9.5 CPU shared
libraries and this CLI, tests negative model and WAV admission **without
weights**, then links the *same production CLI/M1/M2/M3 C11 source* to a
**test-only fake Whisper engine** for positive WAV-to-FINAL. That positive fixture
is intentionally **not real speech recognition**. Real transcription metrics,
licensed multilingual weights, user-provided model SHA and acoustic/permission
tests are still pending. Do not auto-run a model download in CI.
