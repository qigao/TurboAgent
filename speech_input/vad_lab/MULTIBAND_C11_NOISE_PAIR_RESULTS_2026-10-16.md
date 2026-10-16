# Sparse four-group spectral VAD diagnostic — 2026-10-16

**Issue #184 / Draft PR #189 — DO NOT MERGE / DO NOT RELEASE / NOT IN PRODUCTION.**
This diagnostic does not change the stationary-power Guard, SpeexDSP
noise-suppressor defaults, Native SDK, M2/M1/Whisper Provider, Agent Tool
permissions or ownership lifecycles. No Whisper weight/model downloads
or real ASR inference occurred.

## Implementation: bounded aggregation, no source PCM retention

The same existing **strict C11 single Owner** with exact 16kHz mono
signed-S16 320-sample/20ms caller-input and 3,000-frame upper limit
now aggregates 12 **sparse** fixed-frequency Goertzel probes into
four coarse groups of spectral *sampled* energy. These are **not**
complete frequency-band filters and should never be described as mel
filterbanks or calibrated human-speech features.

- Group 0: 250, 450, 650Hz sampled probe frequencies.
- Group 1: 850, 1150, 1550Hz.
- Group 2: 2150, 2950, 3950Hz.
- Group 3: 5150, 6350, 7550Hz.

For above-floor PCM frames with nonzero sparse-probe energy, compute
the 4 normalized group shares (each >=0, total ~=1), normalized
Shannon entropy (0–1) and **total-variation / half-L1 normalized
spectral flux** between consecutive eligible 20ms frames. Any
digital-zero/below-floor frame resets temporal adjacency and cannot
produce a false transition from the previous active frame. Only
fixed-size aggregate numeric band sums, entropy/flux sums and the
immediately preceding **four normalized numbers** remain in the Owner;
no previous PCM, identity, reference transcript, raw runtime payload,
decoder model or per-frame spectral history is kept.

Synthetic-only C11 tests prove that an alternating ~250 / 5150Hz
tone is distinguishable in **band flux** from a static 250Hz tone,
even if total RMS energy does not noticeably change. They also check
silence adjacency reset, sparse-band 0..1 bounds/sum normalization,
one-shot settle, independently identical two-owner replay and the
old digital-zero non-speech admission boundary.

**Most importantly, the VAD decisions have NOT changed:**
`SPEECH_PRESENT`, `UNCERTAIN`, or `NON_SPEECH_CONFIDENT` with the
last reserved **ONLY** for >=50 accepted ALL-DIGITAL-ZERO frames.
Every nonzero input can only be `SPEECH_PRESENT` or `UNCERTAIN`,
so NO arbitrary nonzero road noise is rejected. This preserves
natural/unvoiced whispered speech rather than labelling missing
pitch as no human speech.

## Exact native Linux C11 acceptance

Functional [`125d88b`](https://github.com/qigao/TurboAgent/commit/125d88b018d61ae2e277587c69cb1b0495c2c885),
[CI #38837849859](https://github.com/qigao/TurboAgent/actions/runs/38837849859)
**2/2 SUCCESS**:

- Separate C11 standard-build **2/2 CTest PASS**, ASan+UBSan-built
  **2/2 CTest PASS**.
- Five SHA-256-pinned genuine, rights-reviewed original spoken recordings
  (Japanese CC0, Mandarin CC0, English normal CC BY 3.0 US with
  Association Shtooka/Judith Franck credit, natural English whisper
  CC0 by Mx. Granger, short quietly spoken "No" CC BY-SA 4.0 by
  Anatoly larkin) and independently recorded Ke4roh CC BY-SA 4.0
  recycling-truck warning noise. All bytes/SHA1/original SHA256/
  16k mono S16 WAV SHA256 checked before C11 analysis.
- Side-by-side strict C11 VAD × actual native SpeexDSP 1.2.1
  (explicit Linux package revision/shared ELF SHA) separately tested
  **18 conditions**: 11 correlated genuine-human-source or software
  overlay variants, 7 genuine recorded noise-only or processed controls.
  Everything stays mode-0700/0600 in an ephemeral CI-only private
  directory and is removed on exit. No original/processed audio,
  speaker identities, transcripts or weights uploaded.

## Observed sparse-band features on original licensed recordings

| Source | Total complete frames | Frames passing feature-floor admission | Mean four group energy shares | Mean band entropy | Mean band flux | VAD tri-state |
|---|---:|---:|---|---:|---:|---|
| Japanese reading CC0 | 740 | 308 | [0.742, 0.154, 0.065, 0.039] | 0.297425 | 0.155936 | SPEECH_PRESENT |
| Mandarin reading CC0 | 749 | 665 | [0.732, 0.159, 0.062, 0.046] | 0.287020 | 0.149271 | SPEECH_PRESENT |
| Normal English CC BY 3.0 US | 95 | 95 | [0.753, 0.075, 0.114, 0.058] | 0.331611 | 0.149986 | SPEECH_PRESENT |
| Original English whisper CC0 | 128 | **24** | [0.167, 0.251, 0.293, 0.289] | 0.378861 | 0.279571 | UNCERTAIN |
| Quiet English "No" CC BY-SA 4.0 | 18 | 13 | [0.885, 0.086, 0.028, 0.001] | 0.210796 | 0.143467 | SPEECH_PRESENT |
| Recorded truck warning CC BY-SA 4.0 | 375 | 373 | [0.455, 0.279, 0.130, 0.136] | 0.606008 | 0.235667 | UNCERTAIN |

**Feature-selection caveat:** only **24 of the original 128 whisper
frames** enter the above-floor spectral feature mean, versus 176/178
mixed-source frames in the loud recorded-noise overlay. Therefore an
aggregate contrast can reflect **which frames survive the diagnostic
floor**, not just the presence or absence of actual speech. This is
not evidence that whispered consonants are unimportant or that
the remaining 104 source frames are silent. The PCM itself is never
dropped by the VAD lab.

## Matched voice-overlaid/noise-only feature contrast

The pair for each row uses exactly the same contiguous **first 178
frames** of genuinely recorded truck-warning noise, optionally mixed
IN SOFTWARE with 128 original genuine whispered voice frames after
one second of leading silence (source/noise divisors 8 or 16).
These are not natural simultaneous microphone sessions. The four-band
numbers are compared via **L1** distance (four absolute differences
summed; range 0–2). A small value means similar *aggregate* spectral
distributions, **not** proof that audio content is identical.

| Paired recorded-noise scale / processing | Four-band L1 | Voice-minus-noise entropy | Voice-minus-noise band flux | Voice decision | Noise-only decision |
|---|---:|---:|---:|---|---|
| ÷8, raw | 0.039233 | +0.000096 | +0.002422 | UNCERTAIN | UNCERTAIN |
| ÷8, Speex NS -12dB | 0.045275 | -0.004569 | +0.000802 | UNCERTAIN | UNCERTAIN |
| ÷8, Speex NS -18dB | 0.044972 | -0.004952 | +0.000222 | UNCERTAIN | UNCERTAIN |
| ÷16, raw | 0.062890 | -0.010100 | +0.007320 | UNCERTAIN | UNCERTAIN |
| ÷16, Speex NS -12dB | 0.058078 | -0.016467 | +0.003089 | UNCERTAIN | UNCERTAIN |
| ÷16, Speex NS -18dB | 0.062869 | -0.020976 | +0.000416 | UNCERTAIN | UNCERTAIN |

**Decision:** No reliable voice-vs-noise separation is established here,
especially in the louder overlay. Frequency-group features on these
correlated cases overlap too strongly to create any new
`NON_SPEECH_CONFIDENT` threshold. **11/11** voice-bearing source/
overlay variants avoided hard rejection, but **7/7** noise-only
controls were **NOT screened out**. These are heavily correlated
conditions from only one human source and one vehicle recording,
NOT a population false-rejection/false-acceptance rate.

## Next hard gates

1. Broader original/raw permitted real microphones and far-field
   simultaneous speech+noise with independent human speakers,
   sustained soft/monotone/unvoiced/whispered zh/ja/en and code switching.
2. Evaluate an independently reviewed strict-C11 multiband statistical
   VAD or other acoustic discriminative candidate **on a separate
   hidden-label holdout**; measure admission **and** false rejection
   before any nonzero-noise rejection. Abstention should remain
   `UNCERTAIN` and not bypass real-model inference or Tool permission.
3. Separately approved licensed SHA-pinned real Whisper weights for
   original vs NS12/NS18 WER/CER, weak-consonant recall and
   noise-only transcript hallucination; **none acquired or run here**.
4. Real NativeIO capture timestamps, the 20ms NS primer/tail,
   cancel-first, DSO unload; complete source sanitizer and six-RID
   SDK pinning. Independent Linux PraktorTools `hash_bytes` #150
   and Windows CLI storage timeout #180 remain open.

**No release, merge, new runtime Owner, real-model download or
production VAD/NS/Stationarity Guard toggle.**
