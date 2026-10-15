# SpeexDSP 1.2.1 NS-only C11 laboratory (NOT production)

**DRAFT / DO NOT MERGE / DO NOT RELEASE / DO NOT ENABLE BY DEFAULT.**

The user-facing TurboAgent audio chain is unchanged. This directory is
*deliberately not included* by `speech_input/CMakeLists.txt`, the native SDK,
Whisper Provider, CaptureBridge or runtime. The executable and static library
here are opt-in *diagnostics*, not another Provider Owner or production ABI.
No RNNoise weights, network/ASR model, agent tools or microphone are used.

## Upstream and license

- SpeexDSP's upstream **1.2.1** stable source release:
  https://github.com/xiph/speexdsp/releases/tag/SpeexDSP-1.2.1
- Upstream copyright/license in `COPYING`:
  https://github.com/xiph/speexdsp/blob/SpeexDSP-1.2.1/COPYING
  — permissive BSD-3-Clause terms. Source/binary redistributors must preserve
  relevant copyright, notice, disclaimer and non-endorsement conditions.
- Native API: `speex_preprocess_state_init(320, 16000)`,
  `speex_preprocess_run` and `speex_preprocess_ctl` for explicit DENOISE,
  NOISE_SUPPRESS, AGC, VAD and DEREVERB settings. Upstream doc:
  https://www.speex.org/docs/api/speex-api-reference/group__SpeexPreprocessState.html
- This draft's Ubuntu installed development package must have pkg-config
  version **exactly 1.2.1**; the CI displays its distro package revision.
  That does not pin upstream binary contents to one digest or instrument the
  full SpeexDSP source with sanitizers. A real release additionally requires
  registry/source and artifact digest pins for every platform.

## Bounded processing contract

- **One C11 state / one mono channel / one utterance**; exactly 320 S16
  samples at 16 kHz (20 ms) per call, maximum 3,000 frames = 60 seconds.
- **No fallback** when native dependency, control settings or processing fail.
  Output is not committed for rejected calls; no raw PCM or transcript stored
  inside the owner between calls (the external DSP maintains its own adaptive
  filter/model state). All caller activity is serialized.
- Denoise **ON**, noise suppression fixed to explicitly selected **-12 dB**
  or **-18 dB**; AGC **OFF**, VAD **OFF**, dereverb **OFF**. Read back every
  setting via SpeexDSP control API at construction. Noise suppression is
  **not** a `NO_SPEECH` decision or permission to execute an Agent Tool.
- The return value of `speex_preprocess_run` is **not used as VAD
  classification** when VAD is disabled. This experiment leaves the existing
  Whisper `stationary_noise_guard` **OFF by default**.
- The independent Linux-only raw S16LE tool writes PCM into an exclusive
  private 0600 file for analysis, bounded at 1,920,000 bytes. It emits numeric
  aggregate power only and refuses symlinks/traversal at its direct file
  arguments. CI cleans up media and provisional output at exit.
- C11/ASan+UBSan owner tests exercise deterministic replay with two separate
  owners, short/invalid frame rejection and 60s capacity. **SpeexDSP distro
  binary is not sanitizer-instrumented**: this is source-sanitized for our
  adapter only, not dependency-full Sanitizer acceptance.

## Experimental comparisons

| Source | Why | Important limitation |
|---|---|---|
| generated constant-level broadband noise (4 s) | adaptive noise floor, compare -12/-18 dB modes | synthetic; says nothing about ambient rooms |
| verified original English whispered word, **Mx. Granger**, CC0 | check that denoise does not trivially mute real quiet speech | not a population whisper/ASR measure |
| verified original recycling-truck reverse-warning noise, **Ke4roh**, CC BY-SA 4.0 | real nonstationary warning noise, compare residual energy | suppressing noise does not prove hallucination absence |

WAV preparation uses the existing private SHA-pinned `prepare_stationarity_wav.py`.
Original media are downloaded only by isolated opt-in PR CI, all raw/processed
PCM and original audio are removed on exit, and no raw artifacts or transcripts
are uploaded. For real speech and real noise, the metrics are *descriptive*
energy and sample differences, **not** evidence that WER/CER improves, that
voice remains intelligible, or that final hallucinations are prevented.

## How to reproduce in an explicitly configured Linux environment

```sh
pkg-config --modversion speexdsp         # must be 1.2.1
cmake -S speech_input/noise_lab -B build/speex-ns-lab -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/speex-ns-lab
ctest --test-dir build/speex-ns-lab --output-on-failure
# Private, caller-provided absolute S16LE paths only; output must not exist:
build/speex-ns-lab/ta_speex_ns_raw_probe \
  /private/source-16k-s16le.raw /private/processed-16k-s16le.raw -12
```

## Next gates (not completed by this draft)

1. Source and installed SDK binary digests pinned for Linux, macOS, Windows,
   Android and iOS; rebuild complete dependency source under ASan/UBSan/TSan.
2. Robust numerical paired measurements for diverse real room noise + soft,
   whispered, monotone and code-switching voices at known recording SNR.
3. Actual independently approved Whisper model weights for **paired WER/CER,
   word drops, spurious speech, latency, peak memory and CPU** on original
   versus denoised PCM. Disallow automatic fallback to unfiltered input.
4. Preserve M1/M2/Provider Owner and DSO drain lifecycles and keep any
   produced `NO_SPEECH` independent of the NS return bit.
5. Only after independent quality gates consider an explicit optional
   `SpeechNoiseSuppressorV1` effect boundary; **no integration now**.

Physical active noise cancellation (speaker antiphase) additionally requires
a calibrated acoustic feedback/reference path and device-specific physical
geometry, so it is *not implemented by this software filter*. AEC likewise
requires an aligned playback reference and is a separate experiment.
