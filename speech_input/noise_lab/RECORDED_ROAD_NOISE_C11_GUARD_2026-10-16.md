# Genuine road-warning noise: strict C11 admission with / without speech

**2026-10-16 — DRAFT #183 / DO NOT MERGE / NO REAL ASR MODEL / NO RELEASE**

## Why this test matters

SpeexDSP is a noise suppressor (NS), not a voice-activity detector (VAD).
The current opt-in `stationary_noise_guard` checks coarse 20ms frame-power
stationarity, and cannot be expected to screen dynamic environmental noise.
This test uses the **actual TurboAgent strict C11 PCM/Capture M2/
SpeechInput M1/Whisper Provider source**, and calls a deterministic
test-only **fake Whisper vendor** that always returns a fixed known
utterance IF inference would have been reached.

This test is **not** genuine Whisper word recognition, false-transcript
generation, actual hallucination rate, microphone AEC, or physical ANC.

## Audio provenance and consent

Two previously independently pinned, publicly licensed Commons recordings,
never committed or rehosted in this repository:

- **Voice**: genuine English whispered word, Mx. Granger,
  [Whispering-example.ogg](https://commons.wikimedia.org/wiki/File:Whispering-example.ogg),
  **CC0 1.0**, original **30,283 bytes**, SHA-1
  `cc3b98f93353cf6ed8bb453669987afe7dd748bd`,
  SHA-256
  `bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e`;
  independently verified canonical 16kHz/mono/S16LE WAV SHA-256
  `c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0`.
- **Field noise**: real independently recorded recycling-truck
  reverse-warning white-noise beeper, Ke4roh,
  [Back-up white noise.ogg](https://commons.wikimedia.org/wiki/File:Back-up_white_noise.ogg),
  **CC BY-SA 4.0**, original **120,796 bytes**, SHA-1
  `71500b016280d285ac6854bd46eef6373847247e`,
  SHA-256
  `7d70594ef7ba1bd7a34136ab7140420d016fd730acc6c124a1a02fe6c75337d0`;
  canonical WAV SHA-256
  `cd9023210066ea6ad618fe386d86c6e9469d4b9a19ba170d5032eb76fb30c808`.

Every source/converted file was checked against its full exact digest.
Media, raw PCM, case directories, dummy vendor transcripts and private
provenance lived only in a mode-0700 CI temp directory; all removed on exit.
No audio or model uploaded via GitHub Artifacts.

## Exactly defined experiment

Construct a **178-frame** (3.56s) clean signal:
50 complete 20ms silent frames + the original **128 spoken 20ms frames**.
Overlay the **contiguous first 178 frames** of the independently
recorded field noise with S16 signed samples divided **toward zero**
by **8** or **16**. No source looping, disjoint splicing, time stretch,
clipping, adaptive fallback, microphone recording or invented transcript.

For each divider:

- Human-voice input conditions: clean reference, mixed recorded noise,
  mixed NS -12dB, mixed NS -18dB.
- **Noise-only** controls: same originally recorded contiguous field-noise
  segment (at identical divider), noise-only NS -12dB and -18dB.
- The NS arms are processed through separate strict-C11 SpeexDSP 1.2.1
  owners with DENOISE enabled, AGC/VAD OFF, **one 20ms priming frame
  compensated and explicit tail drain**. Native package version and
  installed shared ELF digest fixed to the previously reviewed Linux
  1.2.1-1ubuntu3 candidate.
- Every output is exactly **178 complete 320-sample S16 frames**, wrapped
  into private mono 16kHz WAV **without resampling or padding**.
- Each case calls real C11 WAV/M2/M1/Provider with Guard OFF and ON.
  The vendor engine is a **dummy** returning one known utterance if the
  provider calls it. Final output is not a genuine audio transcript.

## Results — exact CI evidence

Functional head [`0c01519`](https://github.com/qigao/TurboAgent/commit/0c0151923dfd718f9b0f404009719093d4220aa6):
[independent SpeechWhisper CI #38834291978](https://github.com/qigao/TurboAgent/actions/runs/38834291978)
**2/2 SUCCESS** (offline tooling and actual strict C11 CLI with fake
vendor), exact **28 of 28 terminal outcomes** satisfied.

| Condition | Number of arms across both recorded-noise divisors | Guard OFF | Guard ON | What is established |
|---|---:|---:|---:|---|
| Known human speech (clean, mixed, -12dB, -18dB) | 8 | **8 FINAL** | **8 FINAL** | These 8 actual speech-origin variants reached the fake vendor, no premature `NO_SPEECH` |
| Recorded real noise only (original, NS -12dB, NS -18dB) | 6 | **6 FINAL** | **6 FINAL** | No nonstationary noise-only signal was screened by the opt-in coarse energy Guard |

The total is **28 native C11 calls**. Guard ON on the 6 noise-only
conditions always allows dummy inference. This is **NOT proof of
real-model hallucination** because the vendor is programmed to output
text for any accepted audio. Conversely, 8 real human-speech
Guard-ON positives do not establish WER/CER or weak-consonant retention.

The [independent reference error experiments in #181](https://github.com/qigao/TurboAgent/pull/181)
on the same two licensed originals found **+3.3894 to +3.6310 dB**
post-NS clean-reference SER improvements over two divider settings;
those errors include both residual noise and NS-induced speech
distortion. That signal-level benefit **does not make NS a VAD**, and
does not measure ASR correctness.

## Remaining mandatory gates before any production decision

1. An explicitly separately authorized, SHA-pinned real Whisper model
   must compare the **same exact clean/noisy/NS12/NS18 PCM** for English
   WER/CER, weak consonants and isolated noise-only hallucinations.
   Never reuse a previously applied one-shot model-download label.
2. Independent speakers and more natural field recordings: HVAC/fans,
   keyboard, speech babble, true room reverberation and simultaneous
   clean/noisy microphone captures. These software overlays are not
   representative acoustic microphone SNR measurements.
3. Evaluate an actual bounded, provider-neutral C11 acoustic VAD
   candidate *separately* from NS, with an **UNCERTAIN** option rather
   than treating low energy or stationary spectra as definite silence.
4. Full-source SpeexDSP sanitizer coverage, six-platform installed
   SDK ABI binary/SHA pins, real NativeIO/Capture timestamps, 20ms
   priming/end-of-stream settlement and cancel-first/DSO unload races.
5. Independent [Linux PraktorTools hidden `hash_bytes` #150](https://github.com/qigao/TurboAgent/issues/150)
   and [intermittent Windows CLI storage timeout #180](https://github.com/qigao/TurboAgent/issues/180)
   remain unrelated to this speech acoustic experiment.

**Decision:** maintain `stationary_noise_guard` DEFAULT OFF, keep NS
out of production, no second C11 Owner or alternative model fallback,
and keep #137/#146/#167 open and all speech PRs Draft. No merge,
tag/release or model-weights download authorized by this evidence.
