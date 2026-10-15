# SpeexDSP controlled multi-noise / clean-only NS A-B matrix

**Experimental Draft / DO NOT MERGE / DO NOT RELEASE / no production DSP / no model weights.**

This extension to [PR #169](https://github.com/qigao/TurboAgent/pull/169)
addresses two gaps left by the original single-level white-noise reference:
the strength/spectrum of injected noise and speech distortion caused by the
denoiser **even when no noise is present**.

## Reproducible inputs

All cases start from the identical, SHA256-pinned, original CC0 English
whispering recording (Mx. Granger). Original SHA256:
`bbdbe95dfe4742e27845c03fba3a8caf9f0f5ad65e34e512fd5609d155e6304e`.
Its converted mono 16kHz S16LE reference is exactly
`c81cf6b75800daf1070d3001f094877581bc68100f3c64cb43638491f89988d0`.

The test prepends 50 digital-zero 20ms frames (1s) **only to give the
adaptive noise suppressor a measurable noise-only run-in**. All real
speech frames then remain in their original order; no voiced frame is
spliced or flattened. It adds two **generated, non-real-room** noise types:

- `white`: uniformly sampled bounded S16 noise, seed 20261015.
- `lowpass`: controlled causal first-order smoothing of the same
  bounded random innovations (no recorded HVAC/fan/noise metadata).

Peak innovation amplitudes are strictly **100, 200, 400 S16 counts**;
any S16 clipping fails the run. Noise has no speaker/voice identity and
is not included in human false-rejection denominators.

For each of these **six** noisy PCM sources, both SpeexDSP NS-only
settings **-12dB and -18dB** run independently with the exact same
original clean reference and experimental 20ms tail drain/primer drop.
There are **12 distinct mixed-input comparisons**. Two additional
clean-only input passes (one for each NS level) quantify intrinsic
filter changes without the added synthetic noise.

## Reported metrics

- Speech reference power, known injected error power, post-NS residual +
  speech distortion error power.
- Speech-portion `SER_before` / `SER_after` and `SER_delta`.
  A **negative** delta is valid evidence the filter made this condition
  worse, not a CI failure to conceal. Do not claim all noise spectra
  have improved until every result is observed.
- `clean_only_filter_error_power`, `clean_only_filter_SER`, and
  count of originally nonzero voice frames made fully zero by NS on the
  clean-only reference. This tests a different property from mixed
  input noise removal.
- Explicit `noise_profile`, `noise_max_amplitude`, private file
  hashes and bounded frame duration, no audio/transcript artifact output.

## Constraints and decisions

This is an **oracle reference** (we know the clean signal and our
injected noise), not a measured natural room/microphone SNR. It does not
independently establish auditory intelligibility, weak consonant
preservation, language CER/WER, ASR hallucination prevention, or real
speaker-population false rejection. The referenced SpeexDSP binary remains
the exact Linux Ubuntu package/hash pinned in [#163](https://github.com/qigao/TurboAgent/pull/163).
The adapter is C11, standalone, sanitizer tested; this does not fully
instrument the third-party SpeexDSP source under ASan/UBSan/TSan or
qualify installed SDK for other platforms.

**Not integrated with production**: no `SpeechNoiseSuppressor` ABI, no
Capture latency/Owner/DSO handoff, no microphone and no automatic NS
fallback, no model download, no default VAD or energy guard change.

A SpeexDSP mode can be recommended for further testing only after
reporting **all** SER deltas, clean-only voice-damage controls, real
noise/room conditions, and independently approved Whisper model-backed
transcript/quality metrics. Keep #137/#146/#167 Open, #163/#169 and this
stacked PR Draft and the current guard OFF.
