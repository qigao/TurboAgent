# Experimental strict-C11 tri-state acoustic VAD (Issue #184)

**DRAFT / DO NOT MERGE / DO NOT RELEASE / NOT WIRED INTO PRODUCTION.**

This subtree is intentionally **absent from** `speech_input/CMakeLists.txt`,
the Whisper Provider, TurboAgent Native SDK packages, CaptureBridge,
Agent Tool authorization and the CLI's default audio chain. The current
`stationary_noise_guard` remains OFF by default. The separate SpeexDSP
noise suppressor also remains OFF and uninstalled.

## Architectural contract

- **One serialized C11 Owner per utterance**. Exact 16kHz mono S16LE,
  **320 samples / 20ms** per frame, **maximum 3000 frames / 60 seconds**.
  Invalid frame/NULL, capacity overflow, double settlement and further
  input after settlement fail closed. No fallback, microphone, callbacks,
  threads, DSO ownership, queues, global mutable state or persisted PCM.
- The Owner retains fixed-size **aggregate numerical observations only**:
  frame power, cross-frame power modulation, sparse fixed-frequency
  Goertzel features and normalized positive lag-correlation (40–160
  samples). This is exploratory acoustic evidence, **not** a trained
  classifier, linguistic validation, voice identity or ASR model.
- `SPEECH_PRESENT`, `NON_SPEECH_CONFIDENT`, `UNCERTAIN` are separate
  values. **UNCERTAIN MUST NOT cause `NO_SPEECH`**; it must preserve
  a separately authorized decoder's opportunity to process the audio.
  At present the only high-confidence non-speech certificate is
  **>=50 complete 20ms frames that are ALL byte-exact digital zero**.
  Nonzero sound, even with stationary frame power or lack of periodic
  voicing, is never classified confidently absent. In particular,
  naturally whispered/unvoiced consonants can have no detectable pitch.
- This deliberately **cannot** reject real dynamic road-warning noise.
  It is not permissible to claim a VAD improvement on noise-only false
  acceptance merely because some speech is labelled `SPEECH_PRESENT`.
- Linux private raw-probe accepts only absolute regular **nonsymlink
  0600 S16LE** input, max 1,920,000 bytes, on a private CI runner.
  Outputs aggregate numeric JSON, never PCM, filenames or transcripts.
  The core C11 library has no external dependencies beyond math/libc.
- No native SpeexDSP, Whisper weight download, model approval label
  or external third-party VAD implementation is introduced by this PR.

## First exact real-source evidence

[CI #38835959000](https://github.com/qigao/TurboAgent/actions/runs/38835959000)
**SUCCESS**, strictly compiled C11 owner and source ASan+UBSan both PASS,
plus six exact licensed public original audio tests (original byte length,
SHA-1, full SHA-256 and canonical WAV SHA-256 separately verified).

| Original recording | 20ms frames | Mean peak normalized lag correlation | Mean sparse spectral concentration | Modulation events | Observed tri-state |
|---|---:|---:|---:|---:|---|
| Japanese reading, すじにくシチュー, CC0 | 740 | 0.633431 | 0.621627 | 351 | **SPEECH_PRESENT** |
| Mandarin reading, Ngguls, CC0 | 749 | 0.724198 | 0.668810 | 256 | **SPEECH_PRESENT** |
| English Shtooka/Judith Franck, CC BY 3.0 US | 95 | 0.753739 | 0.599935 | 50 | **SPEECH_PRESENT** |
| Genuine whispered English, Mx. Granger, CC0 | 128 | 0.402421 | 0.641333 | 37 | **UNCERTAIN** |
| Quietly spoken English “No”, Anatoly larkin, CC BY-SA 4.0 | 18 | 0.814768 | 0.631047 | 11 | **SPEECH_PRESENT** |
| Recorded reversing/truck warning noise, Ke4roh, CC BY-SA 4.0 | 375 | 0.289306 | 0.491236 | 25 | **UNCERTAIN** |

The classifier **did not prematurely reject any of the five original
real-human recordings**, including whisper. The road-warning recording is
still **UNCERTAIN** and therefore **not screened out**; this is an
explicit unresolved noise-only false accept. None of these samples is
a speaker-representative cohort or a genuine paired Whisper WER/CER
test. The quiet 18-frame word is not evidence about sustained soft
speakers. Altered/power-equalized speech must not be mixed into natural
human false-rejection denominators.

**Deterministic safety fixture:** 49 all-zero frames => `UNCERTAIN`;
50 all-zero frames => `NON_SPEECH_CONFIDENT`;
60 frames containing just one nonzero S16 sample => *not* confidently
non-speech. 3000-frame capacity and one-shot settlement were tested.

Original voice/noise attribution and SHA fingerprints:
- [Human original corpus](../whisper/tools/HUMAN_VOICE_CC0_LICENSED_EVIDENCE.md)
- [Natural whisper and short quiet word](../whisper/tools/NATURAL_WHISPER_QUIET_EVIDENCE.md)
- [Real Ke4roh recorded vehicle noise](../noise_lab/RECORDED_ROAD_NOISE_C11_GUARD_2026-10-16.md)

All original Ogg, converted WAV, extracted PCM and provenance are
ephemeral private CI files deleted by an exit trap. **No audio assets or
model weights were committed or uploaded.** Test code reports numeric
aggregates only.

## Next independent diagnostic acceptance

1. Apply tri-state C11 probe to **exact same licensed human+recorded-noise
   software overlays and separate noise-only controls**, optionally after
   isolated SpeexDSP -12/-18 with 20ms priming compensation. A good
   result must **never** reject known speech. A noise-only UNCERTAIN
   must be reported as **not screened**, not rewritten as success.
2. Expand independently consented/public-licensed natural acoustic
   conditions (sustained soft/monotone, whispered zh/ja, code switch,
   HVAC, fan, keyboard, real speech babble, far-field reverberation).
   Report speaker- and condition-independent denominators.
3. Calibrate and cross-validate broader acoustic VAD features **without
   trusting energy/pitch alone**; only evaluate high-confidence nonzero
   noise rejection after negative tests demonstrate strong speech safety.
   Do not bake a threshold tuned only for the single road sample into
   production.
4. Independently authorized SHA-pinned **real** Whisper weights for
   WER/CER and noise-only transcript/hallucination analysis. No stale
   single-use model-label authorization inheritance.
5. NativeIO Capture clock, effect tail, cancellation-first and DSO
   unload/drain, strict C11 TSan, six-platform native Installed SDK
   exact third-party binary/ABI/source sanitizer evidence.
6. Resolve unrelated [Linux hidden `hash_bytes` #150](https://github.com/qigao/TurboAgent/issues/150)
   and [Windows CLI storage timeout #180](https://github.com/qigao/TurboAgent/issues/180)
   on their own owner/CI tracks.

**Current decision: DRAFT-only conservative diagnostic.** Do not merge,
tag, publish, download weights, install a second Owner, or enable either
SpeexDSP NS or the old Stationarity Guard in production.
