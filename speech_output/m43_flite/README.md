# M4.3: optional CMU Flite 2.2 SpeechOutput Provider

**Optional candidate / DEFAULT OFF / DO NOT RELEASE.**
Stacked on TurboAgent #208. Tracks [voice M4 #164](https://github.com/qigao/TurboAgent/issues/164).

This is the first *real-engine-capable* provider implementation, not an
assertion that real model/voice assets have been accepted or distributed.
No Flite dependency is built, fetched, downloaded or loaded by default.
Ordinary TurboAgent and M4.0/M4.1/M4.2 builds remain unchanged.

## Engine and voice provenance

Candidate: [CMU/FestVox Flite](https://github.com/festvox/flite)
2.2, ANSI C, CMU US Kal16 diphone voice. The core and shipped voices have
BSD/X11-style licenses subject to preserving attribution and the
individual [COPYING](https://github.com/festvox/flite/blob/master/COPYING)
notices; review the exact source/compiled voice license separately before
distribution. A reviewer must supply exact **already installed** Flite
2.2 headers and four absolute library paths plus four SHA-256 digests
(voice, usenglish, cmulex, core), and explicitly opt in to voice licensing.
The default CI does not invoke a package manager or download any voice.

The Flite C API `flite_text_to_wave()` with Kal16 creates native
**16 kHz mono S16**. Only the optional, independently approved real helper
calls Flite's `cst_wave_resample()` to **explicitly** convert it to
**24 kHz mono S16**. It then streams 480-sample/20ms frames with a short
final frame. No hidden fallback to a different voice/language/provider,
no Flite `flite_voice_select()` (which can fetch file:/http: assets),
no SSML/URL evaluation, and no physical audio devices.

## Ownership and enforced limits

- Host retains the exact existing `turbo_tts_provider_t` ABI1, with
  `begin/poll_pcm/request_cancel`. No public Salts/CHttp/CMeta ABI change.
  `VoiceCoordinator` continues to be the only Playback producer.
- Helper executable is an explicit **absolute path**. It is launched with
  `posix_spawn` (no shell or PATH search); approved text is written to a sealed anonymous Linux memfd and passed as
  child stdin; stdout carries bounded framed PCM over a pipe. One child per
  generation, 128 text bytes max.
  Only the exact English `cmu_us_kal16` voice and `en` label are accepted.
- A native-endian-independent length-checked request and reply framing
  carries S16 little-endian PCM. Parent keeps at most one 960-byte frame
  buffer; the OS pipe is bounded, never a second unbounded PCM queue.
  All PCM is copied into caller-owned memory; no disk persistence.
- Vendor holds its **whole waveform in the child**. The helper imposes
  `RLIMIT_AS=256MiB`, `RLIMIT_CPU=5s`, no file creation and no core
  dumps, then rejects output over 30 seconds. Host separately enforces
  wall-clock 10s default and SIGTERM→SIGKILL cancel escalation.
  A library can fail under these limits: **fail closed**, do not retry a
  different voice/provider or increase limits silently.
- Cancellation is nonblocking. `poll_pcm` yields `POLL_CANCELED` only
  after `waitpid` actually reaps the child, and no later PCM is forwarded.
  Owner/DSO destroy refuses EBUSY before child settlement. A final DONE
  frame is not enough: check real EOF, process exit status and reaping.
- POSIX/Linux only in M4.3.1. This is resource isolation, **not** an OS
  security sandbox; no seccomp, namespace or network-denial guarantee is
  claimed. Other operating systems require separate native acceptance.

## Default model-free test

```sh
cmake -S speech_output/m43_flite -B build/m43 -G Ninja
cmake --build build/m43
ctest --test-dir build/m43 --output-on-failure
```

The fake executable exercises exactly the same process protocol as the
Flite helper, but contains **no synthesizer, no weights and no voice**.
Tests check 480+120 ordered PCM, strict generation and voice admission,
missing helper refusal, cancel before first PCM, cancel after PCM,
SIGTERM-ignoring worker SIGKILL/reap, malformed 481-sample reply, and
nonzero/early-exit failure. GitHub CI repeats this with ASan/UBSan.

A *separate, real* Linux ELF Provider image
`libturbo_tts_m43_flite_provider_dso.so` compiles the exact same
process adapter under hidden visibility and `-z,defs`. Its only exported
entry is `turbo_m43_flite_dso_entry`. A standalone native `dlopen`
test constructs the borrowed SpeechOutput Provider **from that image**,
refuses destroy/unload while the helper child is alive, then verifies
normal completion, SIGTERM-ignoring cancel escalation, malformed frame
settlement, true child reaping and exactly one DSO unload destructor.
`nm -D` rejects unresolved `turbo_tts_*` host symbol imports. All
tests are still **model-free**; none is a real licensed Flite positive.

## Opt-in real vendor helper (manual, authorization required)

Configure only after separately auditing/download-authorizing a voice,
the exact release-tagged Flite 2.2 runtime and copied copyright notices:

```sh
cmake -S speech_output/m43_flite -B build/m43-real -G Ninja \
  -DM43_ENABLE_REAL_FLITE=ON -DM43_VENDOR_LICENSE_APPROVED=ON \
  -DM43_FLITE_INCLUDE_DIR=/reviewed/flite/include \
  '-DM43_FLITE_LIBRARIES=/reviewed/lib/libflite_cmu_us_kal16.so;/reviewed/lib/libflite_usenglish.so;/reviewed/lib/libflite_cmulex.so;/reviewed/lib/libflite.so' \
  '-DM43_FLITE_LIBRARY_SHA256=<voice-sha>;<lang-sha>;<lex-sha>;<core-sha>' \
  -DM43_FLITE_RELEASE_TAG=v2.2 \
  -DM43_FLITE_HEADER_SHA256=<installed-flite-h-sha> \
  -DM43_FLITE_LICENSE_FILE=/reviewed/flite/COPYING \
  -DM43_FLITE_LICENSE_SHA256=<reviewed-core-copying-sha> \
  -DM43_FLITE_VOICE_LICENSE_FILE=/reviewed/kal16/VOICE-LICENSE.txt \
  -DM43_FLITE_VOICE_LICENSE_SHA256=<reviewed-kal16-license-sha>
cmake --build build/m43-real --target turbo_m43_flite_real_helper
```

The caller must also record the exact tagged source/headers, approved
voice/attribution and binary origin. The above CMake verifies **local byte
digests** of four libraries, the public `flite.h`, the Flite core COPYING,
and the separate CMU Kal16 voice license/notice;
those checks do *not* cryptographically prove the runtime was compiled
from the declared release tag. Source-to-binary provenance and other
included Flite headers still require an independent review.

This optional local build does **not** install the provider into
TurboAgent.Native, authorize actual user audio, or claim cross-platform
SDK delivery. The real engine must be exercised through
Provider → VoiceCoordinator → installed Salts::Playback using an explicit
hardware-free test before claiming integrated positive acceptance. Existing `Salts::Playback` device and
VoiceCoordinator retain all native device ownership.

## M4.3.2 explicit real-voice E2E — manual only

A separately authorized reviewer can test **real Flite helper → actual
`dlopen` Provider image → unchanged M4.2 VoiceCoordinator/Playback Bridge →
bounded simulated output callback**. This does NOT open a speaker/mic, and
it is not an Agent or Whisper recognition test. The Host asserts an
artificial checked ASR FINAL and approved `Hello world.` text, then the
standalone real voice must synthesize 24k mono S16 frames. The test proves
source termination, drain, release of the Provider process, worker reap
and exactly one DSO unload. It checks the **independently reviewed**
PCM sample count and FNV64 digest; it may not auto-learn a passing value
from an untrusted candidate. No plaintext/audio is written to disk.

Only after authorizing exact Flite core + CMU voice licenses, obtaining
the installed pinned headers, SHA-256-matched libraries and approved
reference PCM digest, configure additionally with:

```sh
# In addition to ALL explicit M43_ENABLE_REAL_FLITE,
# M43_VENDOR_LICENSE_APPROVED, source-tag, library/header/license pins
# shown above:
-DM43_REAL_VOICE_ACCEPTANCE=ON
-DSALTS_PLAYBACK_INCLUDE_DIR=/reviewed/SaltsUtils.Native/sdk/linux-x64/include
-DM43_REAL_EXPECTED_PCM_SAMPLES=<independently-reviewed-positive-integer>
-DM43_REAL_EXPECTED_FNV64=<independently-reviewed-16-hex-byte-FNV64>
```

A model-free C11 compile test `m43_process_oracle_definitions`
exercises **the exact same CMake helper** for sample/hash definitions,
using harmless synthetic numeric constants. This prevents a broken
function-style compiler macro from silently leaving the manually
authorized test unbuildable. It is not real PCM evidence.

No CLI or GitHub workflow silently supplies those values. Failure to
supply a pinned published Playback header, expected sample count,
fingerprint, Flite release tag, separate reviewed core and voice license
files/digests, or any library digest
must abort CMake configuration. The real E2E test must explicitly
be run under an approved local/native toolchain and recorded against
the exact binary+voice provenance. The **normal model-free CI never
enables this target**.

The Playback callback is still a **test-owned, bounded software sink**;
separate real native Salts::Playback DSO virtual-device tests on #208
remain required. Never equate successful buffer consumption with
physical hearing, AEC performance, or user audio permission.

## Gates before approval

1. Independently licensed and digest-pinned real Flite 2.2/voice with
   offline 24k PCM positive verification, limits, and source provenance.
2. Actual loaded Provider image + process lifecycle and Windows/macOS/
   Linux/Android/iOS installed SDK consumers where supported.
3. Production-quality latency, cancellation tails, real licensed TTS,
   hardware consent and any acoustic AEC must be evaluated separately.
4. The existing #653/#658 native Capture fix and #208 combined
   Harness/Capture/Playback source TSan remain independent required gates.

**Do not merge/release or flip the default voice path based on synthetic
CI or one local vendor trial.**


## Upstream source-tag audit — 2026-10-17

The canonical [festvox/flite](https://github.com/festvox/flite)
repository has an actual Git reference **`refs/tags/v2.2`**.
The literal `2.2` is not an upstream Git tag and is rejected by the
manual real vendor configure gate. The exact tagged source's `COPYING`,
`include/flite.h` and `include/cst_wave.h` were independently read
through GitHub and match the reviewed public Flite licensing/API
materials. This documents **tag identity only**: an installed binary's
origin remains a separate, user-supplied reproducibility/attestation
task. All four binary digests, `flite.h` digest, Flite core `COPYING` digest,
and separate CMU Kal16 voice license/notice digest must be provided before
real helper compilation.


## TTS engines and voice/model acquisition (Flite is one option, not the architecture)

The SpeechOutput Provider ABI is engine-neutral. **Flite/Kal16 is one small, English-only candidate** used to validate a standalone ANSI-C engine path; it is not the required engine, the only supported model, or the default. Each engine/voice is explicitly selected and independently accepted. Never silently substitute another voice, model, language, local runtime, or cloud service when the requested one is unavailable.

### Candidate comparison

| Candidate | What to download / where | Strengths and trade-offs | Integration status here |
|---|---|---|---|
| **Flite 2.2 + CMU Kal16** | Source tag [`v2.2`](https://github.com/festvox/flite/tree/v2.2); build/install locally from reviewed source and preserve core + voice notices. | Small C engine, relatively simple CPU path; this Kal16 voice is English diphone speech, not a modern neural voice. | Optional M4.3 process-isolated candidate only; real positive E2E still not executed. |
| **Piper voices (ONNX)** | [Voice catalog and download instructions](https://github.com/OHF-Voice/piper1-gpl/blob/main/docs/VOICES.md); each voice normally needs `.onnx` + `.onnx.json` and its model card. | Local, CPU-friendly neural voices across many languages. The current `OHF-Voice/piper1-gpl` engine repository is GPL-3.0-or-later; review each voice's separate license. Do not assume the older `rhasspy/piper` fork and current code have identical licensing. | Alternative provider candidate; not integrated or approved by this PR. |
| **Kokoro-82M** | [Upstream model and weights](https://huggingface.co/hexgrad/Kokoro-82M) or the [Kokoro runtime project](https://github.com/hexgrad/kokoro). For a packaged ONNX option, see [sherpa-onnx Kokoro model release](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models). | Neural TTS with a relatively compact model; model repo states Apache-2.0. Runtime/dependencies and the exact selected artifact still need pinning and verification. | Alternative provider candidate; not integrated or approved by this PR. |
| **sherpa-onnx TTS model catalog** | [Official TTS model releases](https://github.com/k2-fsa/sherpa-onnx/releases/tag/tts-models) and [model index](https://k2-fsa.github.io/sherpa/onnx/tts/all/). Download one specific archive and inspect its included license/model card. | A catalog/runtime route covering multiple model families, languages and sizes (including VITS/Piper-derived, Kokoro and others). It is a runtime layer, not a single model; do not bundle the whole catalog. | Candidate for a separately reviewed adapter; its runtime/ABI, binary size, licenses and target-platform support must be evaluated independently. |
| **Coqui XTTS-v2** | [Official Hugging Face model repository](https://huggingface.co/coqui/XTTS-v2) or [Coqui model docs](https://docs.coqui.ai/en/latest/models/xtts.html). Download the exact revision and all required files from the model repository; record hashes. | Multilingual neural TTS including Japanese and Chinese, with voice-cloning features; large model and Python/PyTorch-oriented runtime. The model uses the Coqui Public Model License, not a blanket permissive license. Voice cloning requires separate rights/consent review. | Research candidate only; not integrated or approved by this PR. |
| **eSpeak NG** | [Upstream source and build instructions](https://github.com/espeak-ng/espeak-ng); alternatively use the package from the target OS distribution and pin the package version. | Small rule-based/formant synthesis, many languages, tiny footprint; noticeably less natural than neural models. No neural weight file is needed. | Separate selectable engine candidate; not a fallback and not integrated by this PR. |
| **Hosted TTS APIs** | Obtain SDK/API access from the chosen provider's official documentation; no model weights are downloaded locally. | Can offer high-quality voices with network, credentials, recurring cost and provider data-retention/terms constraints. | Separate explicitly configured remote provider only; never an implicit fallback for a local provider. |

These are options to evaluate, **not a claim that all models have passed quality, security, license, latency, offline, or cross-platform acceptance**. For Japanese/Chinese, shortlist a model that explicitly supports the language and inspect real samples before selecting it. The Kal16 Flite test cannot stand in for that evaluation.

### Download and pin workflow

Downloads are an explicit maintainer/reviewer action; CI and ordinary builds must not fetch voice assets or model weights. Select one exact engine, voice/model, runtime release and target architecture first.

**Flite source (source checkout only; does not fetch or authorize a voice separately):**

```sh
git clone --branch v2.2 --depth 1 https://github.com/festvox/flite.git third_party/flite-src
git -C third_party/flite-src rev-parse HEAD
sha256sum third_party/flite-src/COPYING third_party/flite-src/include/flite.h
```

Then follow the upstream build instructions for the target OS, install the four required Flite libraries and headers into a reviewed local prefix, and independently identify the CMU Kal16 voice license/notice. Do not run this as part of the default CMake build. The local real-voice gate requires separate SHA-256 pins for each library, `flite.h`, core COPYING and voice license/notice; tag identity alone does not attest installed binary provenance.

**Piper voice example (downloads the selected model and its configuration via the upstream tool):**

```sh
python3 -m venv .venv-piper
. .venv-piper/bin/activate
python -m pip install 'piper-tts==1.8.0'
python -m piper.download_voices en_US-lessac-medium --download-dir ./models/piper
```

Use the documented command from the exact pinned Piper release; verify that the voice model card permits the intended distribution/use. The command above is an example of a manual acquisition path, not an approved runtime dependency for TurboAgent.

**sherpa-onnx packaged model example (Kokoro multilingual):**

```sh
mkdir -p models/sherpa-onnx && cd models/sherpa-onnx
curl -fL --retry 3 -o kokoro-multi-lang-v1_0.tar.bz2 \\
  https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/kokoro-multi-lang-v1_0.tar.bz2
sha256sum kokoro-multi-lang-v1_0.tar.bz2
tar -xjf kokoro-multi-lang-v1_0.tar.bz2
```

Before accepting the archive, check the release page, record the exact release asset URL and SHA-256, inspect the extracted model card/license, and pin every runtime dependency separately. A hash recorded after download is an audit record; compare it with a trusted independently published digest or reviewed provenance before treating it as authenticity evidence.

**Coqui XTTS-v2 model example (large Hugging Face artifact):**

```sh
python -m pip install -U huggingface_hub
hf download coqui/XTTS-v2 --revision <reviewed-commit-sha> --local-dir ./models/xtts-v2
```

Replace `<reviewed-commit-sha>` with an explicitly reviewed immutable commit. Review the Coqui Public Model License and all downloaded files before use; do not use a mutable `main` revision for a reproducible acceptance run.

For every candidate, retain a manifest with engine/runtime version, immutable source or model revision, exact voice/model ID and language, source URL, file sizes and SHA-256 digests, licenses/notices, provenance, expected PCM format, latency/resource limits, and an independently reviewed positive PCM oracle. Do not commit large model/voice files to this repository. Download into a user-controlled local model directory or a separately managed artifact store only after approval.

### Provider acceptance rule

A model becomes a supported option only after a **separate explicit Provider/voice selection** and its own license/provenance review, bounded execution/cancellation/unload tests, language and audio-format validation, and real positive PCM acceptance. The shared output contract remains bounded host-owned PCM and the existing `VoiceCoordinator → Salts::Playback` ownership chain. Neural models may emit a sample rate other than 24 kHz; resampling must be an explicit, tested part of that adapter, never an invisible global fallback. No model in this table changes the production default.
