# Native C11 SpeexDSP versus recorded truck warning noise — 2026-10-16

**Test-only, Draft PR #181 / DO NOT MERGE / DO NOT RELEASE.**
No real Whisper model weights, microphone, transcription, VAD or Agent Tool
were involved. No production NS, SDK, owner, Guard default or ABI changed.

## Provenance and exact test design

[Independent CI #38833282439](https://github.com/qigao/TurboAgent/actions/runs/38833282439)
**SUCCESS**: strict C11 SpeexDSP 1.2.1 owner conformance, adapter
ASan/UBSan, 34/34 offline Python unit tests (6 new real-noise overlay
tests), and pinned native CPU processing. The separate 10/10 mocked
Whisper paired-score semantic tests also passed. SpeexDSP's **external
Ubuntu library itself is not fully source-instrumented** by these tests.

The experiment uses two **separately licensed source recordings**, neither
committed nor uploaded to artifacts:

- Real English whispered voice:
  [Mx. Granger, CC0 1.0](https://commons.wikimedia.org/wiki/File:Whispering-example.ogg),
  original SHA-256
  `bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e`,
  previously pinned 16kHz mono S16LE canonical WAV with **128 × 20ms
  frames**. Original audio remains unmodified as the clean reference.
- Real independently recorded recycling-truck reverse-warning noise:
  [Ke4roh, CC BY-SA 4.0](https://commons.wikimedia.org/wiki/File:Back-up_white_noise.ogg),
  original SHA-256
  `7d70594ef7ba1bd7a34136ab7140420d016fd730acc6c124a1a02fe6c75337d0`,
  16kHz mono S16LE **375 × 20ms** frames.

The private clean signal consists of **50 × 20ms silence frames**, followed
by the **same 128 source whispered voice frames**. A contiguous span of
**the FIRST 178 complete 20ms frames** of the real warning-noise recording
is mixed IN SOFTWARE with that clean signal: no looping, time compression,
synthetic noise generator or disjoint noise edits. Every S16 sample is
divided **toward zero** by exactly 8 or 16; the mixture must not clip.
This is **NOT a simultaneous real-world microphone recording or physical ANC**.

For each of the two fixed mixed inputs, strict native **C11 SpeexDSP
1.2.1** is instantiated independently at -12dB and -18dB:
NS ON, VAD/AGC/dereverb OFF, exactly 320 signed 16-bit samples per 20ms
frame, one-frame priming compensation and explicit final settlement.
The score compares each private 178-frame output to the **same exact
clean speech reference** during the 128 speech frames. Error counts
BOTH residual road noise and distortion introduced by NS.

## Recorded-noise clean-reference SER (not acoustic intelligibility)

| Recorded-noise divisor | Original/mixed SER | NS -12dB SER | Improvement -12dB | NS -18dB SER | Improvement -18dB |
|---|---:|---:|---:|---:|---:|
| ÷8 | -10.8400 dB | -7.4104 dB | **+3.4296 dB** | -7.2090 dB | **+3.6310 dB** |
| ÷16 | -4.8157 dB | -1.4262 dB | **+3.3894 dB** | -1.2313 dB | **+3.5843 dB** |

SER is a **signal-to-error ratio relative to the same original clean
whisper**; values can remain negative. A gain in dB means lower
samplewise reference error but cannot independently certify intact
consonants, perceptual quality, word recognition, or hallucination
suppression.

This real recorded-noise overlay shows only **approximately +3.4 to
+3.6dB** SER improvement, less than the earlier
[controlled pseudo-random white-noise sweep #172](https://github.com/qigao/TurboAgent/pull/172)
(+7.3 to +10.7dB over four injected levels) for the SAME original
whisper. The two noise families and mixing scales are **not matched for
identical signal-to-noise spectra or acoustic loudness**, so these
differences are illustrative, NOT a controlled proof that one algorithm
treats a particular noise type worse at identical SNR.

NS -18dB here exceeds -12dB by only **about 0.19–0.20 dB in SER**.
Separately, the earlier unmodified voice-only [#163](https://github.com/qigao/TurboAgent/pull/163)
showed **5 versus 1** initially nonzero whisper 20ms frames becoming
all-zero after -18 vs -12dB. That remains a real preservation warning;
**the small recorded-noise SER advantage does not authorize -18dB as
production default**.

## Still required before any adoption

- Real previously authorized SHA-pinned Whisper model WER/CER and
  hard-to-recognize consonant/keyword retention across identical clean,
  recorded-noise-mixed, -12 and -18 WAVs; separately report noise-only
  hallucinations. Do not use synthetic fake-vendor transcripts as WER.
- More distinct **nonlooped real recorded noise sources** (HVAC, fans,
  speech babble, keyboard impulses), room/reverberation effects and
  genuine simultaneous microphone captures with known permissions and
  independent speaker cohort tracking.
- Strict C11 NativeIO Capture timestamps, 20ms first-frame handling and
  tail settlement, cancel-first/DSO drain, full-source SpeexDSP
  sanitizers and exact six-RID installed SDK ABI/binary hashes.
- The independent Linux native PraktorTools hidden `hash_bytes`
  link blocker [#150](https://github.com/qigao/TurboAgent/issues/150)
  and intermittent Windows CLI storage timeout
  [#180](https://github.com/qigao/TurboAgent/issues/180)
  remain separate from these successful audio tests.

**Decision:** NS and stationary Guard remain OFF in production; all
related PRs stay DRAFT, no merge, no release, no model download. This
test produces only aggregate numbers and recorded-noise source digests;
private WAV/RAW, model material and source transcripts are never
committed or uploaded.
