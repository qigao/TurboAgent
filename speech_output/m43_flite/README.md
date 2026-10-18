# M4.3: isolated optional CMU Flite 2.2 SpeechOutput Provider (DRAFT)

**DRAFT / DO NOT MERGE / DO NOT RELEASE / DEFAULT OFF.**
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
  -DM43_FLITE_LICENSE_SHA256=<reviewed-copying-sha>
cmake --build build/m43-real --target turbo_m43_flite_real_helper
```

The caller must also record the exact tagged source/headers, approved
voice/attribution and binary origin. The above CMake verifies **local byte
digests** of four libraries, the public `flite.h` and reviewed COPYING;
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
fingerprint, Flite release tag, reviewed COPYING or library digest
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
task. All four binary digests, `flite.h` digest and `COPYING` digest
must be provided before real helper compilation.
