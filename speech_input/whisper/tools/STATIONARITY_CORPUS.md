# SpeechWhisper stationarity corpus evaluation (experimental)

**Draft-only, no production VAD certification, no merge or release.**
Tracked in [#146](https://github.com/qigao/TurboAgent/issues/146),
stacked on [#147](https://github.com/qigao/TurboAgent/pull/147).
The optional \`stationary_noise_guard\` remains **OFF by default**.

The standalone \`evaluate_stationarity_corpus.py\` program characterizes what
the existing 20 ms frame-power rule *would* reject before ASR. It **does not**
invoke a Whisper model, run inference, measure WER/CER, detect hallucinations,
record speech, download model/audio assets, or verify corpus consent. An exact
SHA-256 and a caller-declared rights basis are required for every input file.

## Private manifest (schema 1)

Place the raw corpus WAVs in a private local \`audio-root\` directory and
write a JSON manifest outside the repository. Do not commit/upload the
audio, transcripts, model weights, manifest paths, or speaker identities.

~~~json
{
  "schema": 1,
  "samples": [
    {
      "path": "voice/quiet-en.wav",
      "sha256": "REPLACE_WITH_THE_ACTUAL_64_CHARACTER_LOWERCASE_SHA256",
      "source_kind": "human_voice",
      "language": "en",
      "condition": "low_volume",
      "rights_basis": "creator_consent"
    },
    {
      "path": "noise/fan.wav",
      "sha256": "REPLACE_WITH_THE_ACTUAL_64_CHARACTER_LOWERCASE_SHA256",
      "source_kind": "real_noise",
      "language": "none",
      "condition": "fan",
      "rights_basis": "public_license"
    }
  ]
}
~~~

These are illustrative records, not included samples. Every SHA-256 must match
the actual WAV **bytes**, not the PCM samples alone. Language choices for
\`human_voice\`: \`en\`, \`ja\`, \`zh\`, \`mixed\`.
Valid speech conditions: \`ordinary\`, \`low_volume\`, \`monotone\`,
\`sustained_vowel\`, \`low_snr\`, \`code_switch\`.
For \`real_noise\` use \`language: "none"\` and one of \`fan\`, \`hvac\`,
\`traffic\`, \`room\`, \`impulse\`, \`intermittent\`, \`music\`, \`silence\`.
For \`synthetic\` use \`language: "none"\`, \`rights_basis:
"generated_by_test"\`, and \`stationary\`, \`varying\`, \`zero\`,
\`equal_power\`, or \`other\` as condition. Synthetic controls must never be
counted as human voice evidence.

\`creator_consent\` and \`public_license\` are **caller declarations**, not
legal or identity verification. Obtain recording permission and confirm the
actual redistribution/license rights independently. Keep private recordings
out of public GitHub Actions/artifacts.

## Offline invocation

~~~bash
python3 speech_input/whisper/tools/evaluate_stationarity_corpus.py \
  --audio-root /private/audio-root \
  --manifest /private/stationarity-corpus.json \
  --report /private/stationarity-summary.json
~~~

Only explicit caller-supplied files are read. Every WAV must be 16 kHz, mono,
signed PCM16, complete 20 ms frames, at most 60 seconds, at most 4 MiB, with
an exact hash match. The tool bounds its corpus at 256 files / 128 MiB.
Missing files, digest mismatch, unsafe paths, symbolic links, unsupported
format or duplicate manifest keys fail closed without publishing raw data.

The report contains aggregate counts and, where the denominator is positive,
a Wilson 95% confidence interval. \`human_voice.guard_on_false_rejection\`
means a **caller-labelled** human recording would be classified as no speech
before the model is invoked; it is **not** an observed Whisper transcription
error rate. The \`real_noise\` metrics describe pre-decode screening only;
noise that still reaches the decoder has **not** automatically hallucinated.
The \`synthetic_controls\` group is reported separately. Empty language/noise
cohorts are not silently reported as 0% errors.

## Next independent acceptance

Evaluate a rights-cleared set of multiple real speakers with low-volume,
monotone and long-vowel speech; zh/ja/en and mixed-language samples; real
stationary, impulsive and intermittent noise. Report denominators by stratum,
the Guard OFF/ON paired difference, and confidence intervals. Then, only with
separate explicit model-asset authorization, use the exact pinned Whisper CPU
binary/weights to measure WER/CER, hallucinations, p50/p95 latency, RSS and
cancel/unload behavior on the **same samples**.

Keep #137/#146 OPEN and #144/#145/#147/#148 Draft until independent quality
and hardware/permission gates are resolved. Passing synthetic tests or a
single pinned English JFK utterance cannot authorize default-ON or release.
