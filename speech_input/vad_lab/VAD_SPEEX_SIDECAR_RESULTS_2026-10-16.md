# Independent C11 VAD × SpeexDSP real-noise sidecar — 2026-10-16

**DRAFT PR #187 / DO NOT MERGE / NO PRODUCTION NS/VAD / NO MODEL WEIGHTS.**

Exact original code acceptance HEAD
[`990515a`](https://github.com/qigao/TurboAgent/commit/990515a7571f608033c85d3945f5a06e24957ec5):
[CI #38836564112](https://github.com/qigao/TurboAgent/actions/runs/38836564112)
**2/2 SUCCESS**. One job independently rebuilt and tested the
strict-C11 VAD with ASan+UBSan, the other built SpeexDSP 1.2.1 native
C11 NS and ran the *same VAD* on 18 exact offline signal conditions.

## Sources, license and signal correspondence

- Mx. Granger genuine English whispered word,
  [Wikimedia Commons CC0 1.0](https://commons.wikimedia.org/wiki/File:Whispering-example.ogg),
  original full SHA-256
  `bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e`,
  128 complete source 20ms frames. Canonical WAV SHA-256
  `c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0`.
- Ke4roh actual independently recorded truck/reversing warning noise,
  [Wikimedia Commons CC BY-SA 4.0](https://commons.wikimedia.org/wiki/File:Back-up_white_noise.ogg),
  original full SHA-256
  `7d70594ef7ba1bd7a34136ab7140420d016fd730acc6c124a1a02fe6c75337d0`,
  375 complete 20ms frames. Canonical WAV SHA-256
  `cd9023210066ea6ad618fe386d86c6e9469d4b9a19ba170d5032eb76fb30c808`.
- Two **software** overlays (not truly simultaneously microphone-recorded):
  50 frames silence + **unchanged** 128 whispered-source speech frames,
  with the first **contiguous 178 frames** of the recorded noise scaled
  towards zero by divisor **8** or **16**. The matching noise-only controls
  use exactly the same section, no human speech at all.
- A fully separate SpeexDSP v1.2.1 **NS-only** experimental C11 Owner
  processes separately initialized -12 and -18 dB modes; VAD, AGC and
  dereverb OFF. 20ms source/output matching, one priming frame
  compensated and one-shot tail settled. Linux installed shared ELF SHA
  pinned as in [NS lab #163](https://github.com/qigao/TurboAgent/pull/163).
- Every original source Ogg + WAV independently checked with original
  byte-size, SHA-1, SHA-256 and canonical WAV digest. Every private
  S16LE raw file is 0600 in a 0700 runner workspace and deleted at
  job exit. No model, ASR transcript, source bytes, audio, speaker
  information or private path uploaded.

## Measured three-state decisions

| Group | Actual 20ms frames | Exact cases | C11 tri-state |
|---|---:|---|---|
| Unmodified authentic whispered voice | 128 | clean human | UNCERTAIN |
| Same original whisper, NS -12 or -18 | 128 each | human NS-only variants ×2 | UNCERTAIN (both) |
| Original full genuine recorded warning noise | 375 | noise alone | UNCERTAIN |
| Clean reference (one-second silence + 128 whisper frames) | 178 each | ÷8 / ÷16 controls ×2 | UNCERTAIN (both) |
| Real recorded warning-noise overlay on that voice | 178 each | mixed ÷8 / ÷16 ×2 | UNCERTAIN (both) |
| Same recorded-noise-plus-voice overlays after Speex NS | 178 each | ÷8 / ÷16 × NS -12/-18 = 4 | UNCERTAIN (all four) |
| Matching real warning-noise only | 178 each | noise-only ÷8 / ÷16 ×2 | UNCERTAIN (both) |
| Matching noise-only after Speex NS | 178 each | ÷8 / ÷16 × NS -12/-18 = 4 | UNCERTAIN (all four) |

Total **18 observations**, including **11 voice-source or voice-overlay
conditions** and **7 purely recorded-noise conditions**, all returned
`UNCERTAIN`. These are **not 11 independent speakers or real-world
population samples**: the 11 voice-bearing conditions share the same
Mx. Granger original and include transformed software mixtures.

All 11 known-voice conditions avoided `NON_SPEECH_CONFIDENT` (desired
safety for this diagnostic); **0 of 7 noise-only conditions were
confidently rejected** (unresolved noise false acceptance, no screening).

## Why crude pitch/energy thresholds would be dangerous

| Specific paired case | Mean C11 positive lag autocorrelation | Spectral concentration | Decision |
|---|---:|---:|---|
| Clean source whisper | 0.402421 | 0.641333 | UNCERTAIN |
| Full original truck recording | 0.289306 | 0.491236 | UNCERTAIN |
| Speech + road noise ÷8, before NS | **0.301460** | 0.461308 | UNCERTAIN |
| Noise-only control ÷8, before NS | **0.306014** | 0.458192 | UNCERTAIN |
| Speech + road noise ÷16, before NS | **0.303306** | 0.469474 | UNCERTAIN |
| Noise-only control ÷16, before NS | **0.306002** | 0.458132 | UNCERTAIN |
| Speech + road noise ÷8, NS -12 | 0.299397 | 0.469542 | UNCERTAIN |
| Matching noise only ÷8, NS -12 | 0.306639 | 0.465071 | UNCERTAIN |

The noisy genuine-human audio and matched **noise-only** sample have
**nearly identical** energy modulation, positive correlation and sparse
spectral ratios. Calibrating a binary accept/reject threshold on these
values without much broader data **could confidently reject real
whispered speech while allowing noise**. This is not a classifier
separation success. A speaker's weak consonants can lack pitch.

The unmodified whisper had **111 nonzero power frames**, after NS
-12dB **110**, after NS -18dB **106**; these are numeric full-frame
power observations only, **not word/phoneme preservation evidence**.

## Hard next stages

1. Broader independently licensed human/noise cohorts, including **actual
   simultaneously captured** far-field/babble/HVAC/sustained low-volume
   and monotone speech, voiced/unvoiced phonemes and code-switching.
2. Evaluate a genuinely discriminative acoustic VAD (e.g. independently
   reviewed strict-C11 multi-band statistical VAD) **in a separate
   diagnostic**; preserve `UNCERTAIN` when the noise/voice features
   overlap. No model fallback or second runtime Owner.
3. Get independently authorized, exact SHA-pinned real Whisper weights
   for paired **WER/CER, weak consonant deletion, noise-only hallucination,
   latency, CPU and memory**. The fake-vendor text from earlier C11
   Provider tests is **not** recognition evidence.
4. Finish C11 TSan/source cross-platform, full dependency source
   ASan/UBSan and six-RID SDK pinning; real Capture timestamps,
   20ms tail drain, cancel-first, hot DSO unload.
5. Preserve separate existing [Linux `hash_bytes` #150](https://github.com/qigao/TurboAgent/issues/150)
   and [Windows storage timeout #180](https://github.com/qigao/TurboAgent/issues/180)
   gates; do not bypass them to merge a speech candidate.

**Keep #137/#146/#167/#184 open, #186/#187 and all upstream speech
stack PRs DRAFT / DO NOT MERGE / DO NOT RELEASE.** No model downloads;
NS and stationary energy Guard production DEFAULT OFF, VAD lab not
linked from any production library or SDK.
