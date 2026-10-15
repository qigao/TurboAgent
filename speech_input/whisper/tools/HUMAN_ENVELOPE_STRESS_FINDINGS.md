# Experimental speech-origin stationary-envelope stress — #146

**Status: Draft-only, 2026-10-15. DO NOT MERGE / RELEASE / ENABLE GUARD.**

## Independent evidence, exact workflow

At [CI #38736101182](https://github.com/qigao/TurboAgent/actions/runs/38736101182),
the three verified source recordings already accepted in
[`HUMAN_VOICE_CC0_LICENSED_EVIDENCE.md`](HUMAN_VOICE_CC0_LICENSED_EVIDENCE.md)
were downloaded only into private runner workspace and their original and
converted WAV SHA-256 were reverified. No audio, transcript or speaker token
was uploaded or committed. No Whisper model weights or real-model inference
was invoked.

Each *unaltered* spoken file is classified `dynamic_or_unknown` by the
existing no-model power guard and **not** `NO_SPEECH`. Uniform attenuation by
**1/4**, **1/16** or **1/64** of amplitude preserves the frame timeline and
does not change the Guard decision in any of these three cases. Small S16
quantization differences are inevitable and explicitly included in the result.

An intentionally **adversarial, unnatural** derivative then selects up to 120
nonzero 20-ms frames and makes the RMS energy of each one approximately 512
S16 amplitude units. It keeps the waveform *inside each selected frame* but
changes the natural amplitude envelope and may splice non-adjacent moments,
removing zero frames. **No claim is made that the edited output is audible,
intelligible or a representative human monotone speaker.**

| Source language | Source's 20-ms frames | Source/1÷4/1÷16/1÷64 predecode NO_SPEECH | Selected edited frames | Edited-frame CV | Edited power ratio | Edited Guard ON |
|---|---:|---|---:|---:|---:|---|
| Chinese (Mandarin) | 749 | 0 / 0 / 0 / 0 | 120 | 0.000069 | 1.000308 | **NO_SPEECH** |
| Japanese | 740 | 0 / 0 / 0 / 0 | 120 | 0.000251 | 1.001665 | **NO_SPEECH** |
| English | 95 | 0 / 0 / 0 / 0 | 95 | 0.000064 | 1.000328 | **NO_SPEECH** |

Because the Guard uses only whole-frame RMS variation, a constant scaling
of all PCM samples leaves CV and max/min ratio nearly unchanged; making
each frame power uniform forces both measured statistics inside the existing
opt-in refusal thresholds (CV <=0.12 and power ratio <=2.0). This is an
**energy-only information-loss counterexample**, not real-world VAD
performance. The fact that three intentionally modified speech-origin
inputs are discarded says nothing about the natural-speaker false rejection
rate. The three unmodified voice clips **0/3** previously observed
rejections remain a very narrow real-audio positive, not certification.

## What has and has not been measured

- Model-free original/attenuated/equalized classification: **measured** on
  3 pinned licensed speech source files; tests report only numeric metadata.
- Direct C11 Provider execution on these exact PCM files: **not measured**.
  Native C11 source/ABI and separate threshold/ownership fixtures remain
  independently source-instrumented under sanitizers.
- Language-level CER/WER, whether the processed recording can still be
  understood, natural monotone/soft speaker behavior, room noise and
  real ASR hallucination: **not measured**.
- Case samples are not independent randomly selected voices; **do not**
  include the processed derivatives in natural human FRR denominators.

## Implementation choice

Do **not** change `stationary_noise_guard` defaults or loosen energy
thresholds based on this test. Energy-only features are structurally
insufficient to establish the absence of speech; a useful next isolated
candidate would measure acoustic structure/voicing with a bounded strict-C11
implementation, then demonstrate on independently licensed unmodified voice
and real-noise corpora that it reduces false rejection without increasing
noise-triggered transcript hallucinations.

Continue preserving one Provider owner, no second unbounded queue, strict
local-only inference, no callback-time inference, bounded cancellation/DSO
drain, no model fallback and no tool authorization from PARTIAL output.
All #144/#145/#147/#148/#149/#151/#153/#155/#156 stay Draft while
#137/#146 stay Open and guard OFF.
