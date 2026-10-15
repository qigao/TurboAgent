# Private Whisper ASR three-arm paired evaluation (NO MODEL DOWNLOAD)

**Draft / DO NOT MERGE / DO NOT RELEASE / NO DEFAULT-ON.**

This is the next evaluation gate for [#167](https://github.com/qigao/TurboAgent/issues/167)
and the isolated [SpeexDSP NS-only candidate #163](https://github.com/qigao/TurboAgent/pull/163).
It does NOT add noise suppression to production, nor change M1/M2, the C11
Whisper Provider, Provider/DSO ownership, installed SDK, or any Agent Tool
admission.

The evaluator in `tools/evaluate_whisper_paired.py` is **entirely local**.
There are no model URLs, HTTP clients, external model loader fallbacks,
background downloads, microphone acquisition or hidden weights. You must
**already possess and be authorized to use** a licensed local Whisper GGML
model with exact SHA-256 and an independently built local C11
`turboagent-whisper-transcribe` CLI from the pinned Whisper.cpp v1.9.5
configuration.

The existing separate GitHub label-gated model workflow remains unchanged:
**do not add or reuse `speech-model-download-approved`** based on this
diagnostic. The CI for this Draft uses only mocked inference results and
synthetic short PCM fixtures; it cannot establish real ASR performance.

## Three exact input arms

For a *single* rights-reviewed utterance, prepare:

1. `original.wav`: the exact original, converted privately to strictly mono
   S16LE, 16kHz, 320-sample (20ms) complete frames, at most 3,000 frames.
2. `ns12.wav`: the **same original decoded PCM** passed through the isolated
   SpeexDSP 1.2.1 C11 owner with DENOISE ON, AGC/VAD/dereverb OFF, -12 dB.
3. `ns18.wav`: **same original decoded PCM** but -18 dB NS, independently
   initialized Owner (do not reuse a state from the -12dB arm).

The NS lab's Linux `ta_speex_ns_raw_probe` creates raw S16LE with
`--align-one-frame`, which drops a priming output and appends one
settled/drained frame. Re-wrap that exact raw output with a 16k mono WAV
header **without resampling/padding**, using e.g. Python `wave` under
a private `umask 077`. The output must have the same exact 20ms frame count
as the original; the evaluator refuses mismatches. Retain every original
and derived SHA-256 privately. An external source being resampled or
overwritten during the comparison invalidates the result.

Do not assume a 20ms real-device latency guarantee from this offline
compensation: physical Capture timestamps, cancel-first, clock drift and
DSO draining are independently open acceptance gates.

## Local invocation only

Replace these illustrative paths and hashes with separately verified
absolute paths and **full 64-character lowercase SHA-256** values:

~~~sh
set -eu
umask 077
python3 speech_input/noise_lab/tools/evaluate_whisper_paired.py \
  --cli /private/sdk/bin/turboagent-whisper-transcribe \
  --cli-sha256 "$PINNED_CLI_SHA256" \
  --model /private/approved-model/ggml-tiny.bin \
  --model-sha256 "$AUTHORIZED_MODEL_SHA256" \
  --model-max-mib 128 \
  --original-wav /private/pairs/original.wav \
  --original-sha256 "$ORIGINAL_WAV_SHA256" \
  --ns12-wav /private/pairs/ns12.wav \
  --ns12-sha256 "$NS12_WAV_SHA256" \
  --ns18-wav /private/pairs/ns18.wav \
  --ns18-sha256 "$NS18_WAV_SHA256" \
  --reference-file /private/pairs/reference.txt \
  --reference-sha256 "$PRIVATE_REFERENCE_SHA256" \
  --language en \
  --critical-word "your-actual-target-word" \
  --report /private/pairs/paired-asr.metrics.json
~~~

The reference is **caller-verified verbatim text** and is not independently
validated by the evaluator; the example target word is a placeholder and
must actually be present in the reference. Use CER as the primary signal
for `zh`/`ja`; English WER is whitespace/word-oriented. Both metrics are
NFKC/casefold normalizations, so punctuation and spaces are excluded from
CER. They do not certify phoneme accuracy or consonant intelligibility.

A separate bounded child CLI execution is made **for each exact arm**, always
with `--stationarity-guard 0` and the same model/language/thread/decode
settings. No automatic fallback from filtered to raw input is allowed.
A non-zero CLI exit, deadline, malformed UTF-8, unsafe control character
or changed SHA-256 aborts the whole comparison with **no report**. If an
arm explicitly returns `NO_SPEECH` with Guard OFF, it is recorded as
`dropped_speech_before_scoring=true` with WER/CER **null**, not falsely as
0%; this requires human investigation.

The private report (new file only, 0600, limited metadata size) and stdout
contain **numerical WER/CER for each arm and deltas versus original**,
critical-word retention **counts only**, status and elapsed milliseconds.
They never contain raw transcripts, user-reference text, source paths,
model weights, tokens or file hashes. Parent directories and platform
execution isolation are the caller's responsibility. Do not upload the
private original WAV, processed WAV, references or model/CLI binaries as
GitHub Action artifacts.

## Decision framework (not enabled)

A small positive SER gain is insufficient to approve production NS.

- Compare paired CER/WER and critical-word retention on **multiple independent
  speakers** at different genuine background SNRs, whispered speech, long
  soft/monotone speech, Japanese, Chinese and language mixing.
- Count any `NO_SPEECH` on known genuine human speech as a dropout, never
  a successful VAD decision. Validate real noise/hallucination separately.
- Compare actual CPU, peak memory, model loading cost, 20ms NS latency
  and tail settlement; approve and pin the third-party binary for all SDK
  platforms with dependency-source sanitizer validation.
- Only when all gates pass should a separate C11 effect-boundary/Provider
  ownership integration be proposed. **Stationarity Guard remains OFF**,
  the NS candidate is non-production and all stacked PRs remain Draft.

**Important:** the unit tests in CI use an in-memory *fake model response*,
not real Whisper weights. Their WER/CER expectations certify only that this
scorer compares three conditions without leaking transcripts or permitting
silent fallback. They are NOT acoustic-quality measurements.
