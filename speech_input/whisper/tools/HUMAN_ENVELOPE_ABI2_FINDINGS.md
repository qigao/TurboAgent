# Speech-origin gain/flat-energy stress: strict ABI2 interpretation

**Draft-only test tooling; no real model download, no automatic human audio
acquisition, no change to default-OFF speech policy.** Tracks #137/#146.

The separately qualified [#156](https://github.com/qigao/TurboAgent/pull/156)
characterized three known licensed JA/EN/ZH voice-origin signals on an
**older frame-power-only** candidate. The observations cannot be promoted
to latest C11 ABI2: the updated Provider also enforces **strict weak lag-1
PCM correlation** before it can issue `NO_SPEECH`.

This Draft integration reuses the same locally pinned WAV source diagnostic
tool but scores the *full current* offline C11-compatible decision. Two
distinct hypotheses are recorded:

- `legacy_energy_only_would_reject`: at least 50 nonzero 20ms frames,
  CV <= 0.12 and max/min power <= 2.0, ignoring sample coherence.
- `correlation_aware_abi2_would_reject`: same conditions **plus**
  `100 * abs(sum(frame-local adjacent S16 sample products))
  < 12 * sum(squared S16 samples at positions 1..319)`.
  Equality must fail open to decoding.

Purely synthetic coherent alternating-sign and similar signals, including
processed constant-RMS derivatives, show that energy-only refusal need
**not** imply ABI2 refusal. A sparse one-nonzero-sample-per-frame
counterexample shows that some intentionally unnatural flattened signals
still meet both predicates and are screened.

For every caller-provided source the tool needs immutable pinned original
16kHz/mono/S16LE WAV bytes and never writes source or derived PCM, transcripts,
speaker IDs or model artifacts. Gain reduction 4/16/64 and frame-RMS flattening
are *synthetic transformations of the same source*, **not** independently
sampled natural low-volume / monotone voices, and no intelligibility or WER
measurement is performed. FFmpeg audio intake, WAV/PCM dedup and private
rights/provenance remain under #154.

No real-world speaker false-refusal rate or hallucination rate has been
certified, and there is **no** permission to enable or ship the guard. See
[#157](https://github.com/qigao/TurboAgent/pull/157) for exact 12/100
integer threshold tests and [#158](https://github.com/qigao/TurboAgent/pull/158)
for independent, narrow real whispered/quiet voice predecode CI from an
older source stack. Those observations require separate current-HEAD
qualification before population claims.
