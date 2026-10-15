# Multilingual SpeexDSP NS-only C11 admission — 2026-10-16

**Draft-only #179 / DO NOT MERGE / DO NOT RELEASE. Stationarity Guard OFF by default.**

Exact HEAD [`d709b80`](https://github.com/qigao/TurboAgent/commit/d709b801ba587c49b45b13518d109c133e153aaa) independently passed [speech C11/CLI CI #38779122392](https://github.com/qigao/TurboAgent/actions/runs/38779122392) 2/2 jobs, following an initial failed revision with an unexpanded Debian package-version placeholder. This failure occurred **before speech processing**; the placeholder was repaired without relaxing native package/ELF SHA pinning.

## Original licensed human audio, not fabricated PCM

Four original independently sourced Ogg files were verified in their
respective CI-private temp directories against **exact byte size, public
SHA-1, pre-established original SHA-256, and previously validated
canonical 16kHz mono S16LE WAV SHA-256**:

| Category | Source, rights and attribution | Complete 20ms frames | Original WAV SHA-256 |
|---|---|---:|---|
| Japanese | [春はあけぼの](https://commons.wikimedia.org/wiki/File:%E6%98%A5%E3%81%AF%E3%81%82%E3%81%91%E3%81%BC%E3%81%AE.ogg), すじにくシチュー, **CC0 1.0** | 740 | `ccb16e8ea766bd2f4bf40ed09e5666ea6700edbd79343eb961fb13e9cafd6a03` |
| Mandarin | [南国山河](https://commons.wikimedia.org/wiki/File:Mandarin_Chinese-%E5%8D%97%E5%9B%BD%E5%B1%B1%E6%B2%B3.ogg), Ngguls, **CC0 1.0** | 749 | `f246dded9eb91796c515f21b37f8e41b3357bdef3f6fe19552568c12653f0421` |
| English (normal) | [Do you speak English?](https://commons.wikimedia.org/wiki/File:En-uk-Do_you_speak_English.ogg), **Association Shtooka / Judith Franck, CC BY 3.0 US**, modified only for private 16k mono S16LE analysis | 95 | `458ce0d1086ae8a5381ccc09878a630c016ebed42303a859e2030e2efbbe339e` |
| English (extremely short, *quietly spoken*) | [No](https://commons.wikimedia.org/wiki/File:English_male_voice_speech_word_No.ogg), **Anatoly larkin, CC BY-SA 4.0**, converted only privately for analysis | 18 | `6f4f58227166088ba2745d51af05553b01be557b5a2a9d2e04edcbff21d9730a` |

Original byte sizes/SHA-1/SHA-256 are hard-coded in
[`run_multilingual_three_arm_c11_ci.sh`](tools/run_multilingual_three_arm_c11_ci.sh),
with source provenance separately documented in
[`HUMAN_VOICE_CC0_LICENSED_EVIDENCE.md`](../whisper/tools/HUMAN_VOICE_CC0_LICENSED_EVIDENCE.md)
and [`NATURAL_WHISPER_QUIET_EVIDENCE.md`](../whisper/tools/NATURAL_WHISPER_QUIET_EVIDENCE.md).
The source licenses and attribution are preserved here; **none of the original
or converted audio, private files or transcripts is in this repository or a
GitHub upload-artifact**.

## Native C11 NS-only and Provider acceptance

The exact Linux SpeexDSP 1.2.1 distro version + shared ELF digest remained
pinned; each input was processed through independently initialized native
strict-C11 SpeexDSP owners at **-12dB and -18dB** with VAD/AGC disabled,
single-frame 20ms priming compensation and explicit tail settlement. The
original arm was byte-identical to the canonical WAV; all arms retained
their source frame counts.

| Human source | Original nonzero 20ms frames | Originally nonzero frames that became all-zero after NS -12 | Same after NS -18 | Original/NS frames | 6 C11 terminal outcomes (3 arms × Guard OFF/ON) |
|---|---:|---:|---:|---:|---|
| ja | 740 | **1** | **1** | 740 | **6 FINAL / 0 NO_SPEECH** |
| zh | 745 | **0** | **0** | 749 | **6 FINAL / 0 NO_SPEECH** |
| en | 95 | **0** | **0** | 95 | **6 FINAL / 0 NO_SPEECH** |
| quiet en | 18 | **0** | **0** | 18 | **6 FINAL / 0 NO_SPEECH** |
| **Total** | **1598** | **1** | **1** | **1602** | **24 FINAL / 0 NO_SPEECH** |

All tested WAVs also had `dynamic_or_unknown` frame-power hypotheses.
For each input, the **actual strict C11 M2 Capture → M1 SpeechInput →
Whisper Provider source** was invoked with Guard OFF and ON. The
vendor inference engine was a **test-only deterministic stub** returning
the constant text ` utterance` to prove the Provider reached it. No
model weights were loaded or downloaded. Among the 24 native calls,
**12 had Guard ON** and all 12 reached `FINAL`; the other 12 were
the corresponding Guard OFF controls.

The short 0.36-second quiet word was deliberately designated **an
exploratory short-duration observation**: it is far below the 50-frame
stationarity admission threshold, and its absence of `NO_SPEECH` says
nothing about natural sustained soft-spoken sentences. The observed
post-NS retained energy/nonzero frame count also **cannot** establish
consonant/phoneme content retention, transcription accuracy or hallucination
prevention.

## Remaining hard quality gates

- Real licensed/pinned Whisper model inference on **paired identical
  original/-12/-18 WAVs** and actual WER/CER, weak-consonant/word dropout
  and noise hallucination tests under explicit single-use model authorization.
- Independently sampled sustained low-volume, monotone and mixed-language
  genuine voices, plus real natural room noise at known recording SNR.
- Real-device Capture first-packet timestamp/20ms NS latency, one-shot tail
  settlement, cancel-first, NativeIO and DSO ownership/drain tests.
- Complete source-instrumented SpeexDSP Sanitizers and six native installed
  SDK library/package digest pins.
- Main native Linux PraktorTools hidden `hash_bytes` failure remains
  independent [#150](https://github.com/qigao/TurboAgent/issues/150).

**No production NS/VAD or default Guard change, no new Owner/tool authority,
no merge, no release, no model download.**
