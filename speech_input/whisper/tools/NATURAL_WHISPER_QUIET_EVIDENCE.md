# Genuine whisper and quiet short-voice acceptance — #146

**Draft / DO NOT MERGE / DO NOT RELEASE / guard default OFF.**

Two separately sourced, unmodified (before deterministic sample-rate conversion)
human voice recordings were evaluated with the same test-only offline
20ms-frame stationary-power diagnostic as #153. The labels "whispered" and
"spoken quietly" are **taken from the uploader's source descriptions**;
we did not independently audit their speaker identity, decibel level or
intelligibility. The original media, converted WAV, anonymous group tokens
and private provenance never leave the ephemeral CI runner as artifacts.

## Exact source and licensing

| Original | Licensing / attribution | Bytes | Source SHA-1 | Original full SHA-256 |
|---|---|---:|---|---|
| [Whispering-example.ogg](https://commons.wikimedia.org/wiki/File:Whispering-example.ogg) | Own work by **Mx. Granger**, genuine American English whisper, **CC0 1.0** | 30,283 | `cc3b98f93353cf6ed8bb453669987afe7dd748bd` | `bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e` |
| [English male voice speech word No.ogg](https://commons.wikimedia.org/wiki/File:English_male_voice_speech_word_No.ogg) | Own work by **Anatoly larkin**, says "No" **quietly**, **CC BY-SA 4.0** (or GFDL option); conversion to 16k mono signed PCM16 only for analysis, no derivative distributed | 12,926 | `9d7986ce0257056b679a6b199c42447b5df82962` | `3725cf00284943531b0e899e3f7161117397cd3be8f6310a7beca26fbd6e0e3d` |

All original-byte sizes and public SHA-1 values were taken from the
corresponding Commons file records. The full original SHA-256 values were
calculated from exact fetched originals in the initial run
[CI #38737147999](https://github.com/qigao/TurboAgent/actions/runs/38737147999)
**2/2 SUCCESS**. They are now pinned in the following independent
repetition; never silently accept a different uploaded revision.

## No-model predecode results (initial run)

| Recording | Converted WAV SHA-256 | Complete 20ms frames | Duration | CV | Max/min power | Guard OFF | Guard ON |
|---|---|---:|---:|---:|---:|---:|---:|
| Real whispered word | `c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0` | 128 | 2.56s | 3.887678 | undefined (zero-power frames) | 0/1 rejected | **0/1 rejected** |
| Explicitly quiet "No" | `6f4f58227166088ba2745d51af05553b01be557b5a2a9d2e04edcbff21d9730a` | 18 | 0.36s | 1.223346 | 25,018.527214 | 0/1 rejected | **0/1 rejected** |

**Interpretation:**
- The original whispered clip is long enough for the Guard's 50-frame
  stationarity test, yet has strongly variable frame energy and some zero-power
  frames. It was **not rejected**. This is a real-world whispered-speech
  countercheck, not general whisper-quality certification.
- The actually quiet clip is far **shorter than the guard's one-second
  threshold**. It was not rejected. This is a valuable boundary regression,
  but gives **no evidence for long sustained quiet speech**.
- These are two *additional* real-source files to the previously checked
  Japanese, Mandarin, and ordinary English original recordings. All five are
  unique originals, but not a representative speaker cohort; no meaningful
  general FRR estimate follows from observing no rejection.
- Unlike the **artificially flattened speech-origin counterexamples** in
  [`HUMAN_ENVELOPE_STRESS_FINDINGS.md`](HUMAN_ENVELOPE_STRESS_FINDINGS.md),
  the two originals here were not deliberately flattened or spliced.
  Resampling to the 16k S16LE input ABI necessarily modifies representation,
  and the exact converted-WAV hash is provided to preserve reproducibility.

**What remains OPEN:** low-volume **1–10s+** natural utterances across
independently sampled speakers, naturally monotone prosody, Japanese/Chinese
whispering, code switching, real room-noise overlaps; real pinned model
WER/CER/hallucination and actual C11 Provider execution on these particular
audio samples. No Whisper weights were used here, and no source media were
bundled, re-hosted, or exposed in public artifacts.

Keep #137/#146 OPEN, #158 and the complete stacked speech chain Draft,
production stationary guard OFF, and no merge, release or model download.
