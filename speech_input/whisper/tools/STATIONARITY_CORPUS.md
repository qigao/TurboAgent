# SpeechWhisper stationarity corpus evaluation (experimental)

**Draft-only, no production VAD certification, no merge or release.**
Tracked in [#146](https://github.com/qigao/TurboAgent/issues/146),
stacked on [#147](https://github.com/qigao/TurboAgent/pull/147).
The optional `stationary_noise_guard` remains **OFF by default**.

The standalone `evaluate_stationarity_corpus.py` program characterizes what
the current ABI2 20-ms frame-power **and lag-1 correlation** rule would reject before ASR. It **does not**
invoke a Whisper model, run inference, measure WER/CER, detect hallucinations,
record speech, download model/audio assets, or verify corpus consent. An exact
SHA-256 and a caller-declared rights basis are required for every input file.

## Private manifest (schema 1)

Place the raw corpus WAVs in a private local `audio-root` directory and
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
`human_voice`: `en`, `ja`, `zh`, `mixed`.
Valid speech conditions: `ordinary`, `low_volume`, `monotone`,
`sustained_vowel`, `low_snr`, `code_switch`.
For `real_noise` use `language: "none"` and one of `fan`, `hvac`,
`traffic`, `room`, `impulse`, `intermittent`, `music`, `silence`.
For `synthetic` use `language: "none"`, `rights_basis:
"generated_by_test"`, and `stationary`, `varying`, `zero`,
`equal_power`, or `other` as condition. Synthetic controls must never be
counted as human voice evidence.

`creator_consent` and `public_license` are **caller declarations**, not
legal or identity verification. Obtain recording permission and confirm the
actual redistribution/license rights independently. Keep private recordings
out of public GitHub Actions/artifacts.

## Schema 2: private anonymous speaker grouping (recommended for human evaluation)

To avoid counting multiple clips from one speaker as independent people, change
`"schema": 1` to `"schema": 2` and add a **private, opaque** `speaker_group`
token to each `human_voice` sample. Example only (the WAV is not included):

~~~json
{
  "schema": 2,
  "samples": [
    {
      "path": "voice/quiet-en.wav",
      "sha256": "REPLACE_WITH_REAL_LOWERCASE_64_CHARACTER_SHA256",
      "source_kind": "human_voice",
      "language": "en",
      "condition": "low_volume",
      "rights_basis": "creator_consent",
      "speaker_group": "random_group_0001"
    }
  ]
}
~~~

Do not use names, accounts, or any identifying information as tokens. Tokens
are not printed or included in the aggregate report; they are **caller
declared**, not proof of personhood. Do not attempt to re-identify speakers,
including contributors to public speech corpora. For noise and synthetic
samples, `speaker_group` is forbidden. Schema 1 remains supported for
existing private manifests, but its speaker-level statistics are unknown.

Schema 2 reports the number of *declared* speaker groups, the fraction of
groups with any pre-decode false rejection and an equally weighted mean of
per-group false-rejection rates. Clip-level Wilson 95% intervals remain
explicitly contingent on independence assumptions; even the group counts
cannot certify population accuracy without independently curated speakers.

`by_duration` bins for declared human speech are `lt_1s`, `1_to_3s`,
`3_to_10s`, `10_to_60s`. Empty bins display a null rate rather than
masquerading as 0%. **No automatic default-ON decision exists.**

## Source integrity and licensing

The evaluator hashes and decodes the **same bounded WAV byte snapshot**. It
rejects repeated identical PCM samples even if a file's RIFF header,
metadata, filename or language label is changed to produce a different
file-level SHA-256. Neither raw PCM fingerprints nor private speaker tokens
are emitted in reports.

Check individual dataset release terms, contributor consent and
redistribution restrictions before use. A dataset with a permissive license
may still prohibit rehosting raw speaker clips or attempting to identify
speakers. Keep raw human audio, per-speaker tokens, source file names and
model output **outside public CI and GitHub artifacts**. The scripts neither
download nor validate rights to external datasets.

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
format, duplicate manifest keys, duplicate WAV digests **and duplicate decoded
PCM samples** fail closed without publishing raw data. Repeating one recording
with a modified RIFF header, path or language label cannot inflate the sample
denominator.

The report contains aggregate counts and, where the denominator is positive,
a Wilson 95% interval under an **independent-recordings assumption**
that this tool cannot verify (multiple clips from one speaker may be highly
correlated; speaker-level population uncertainty remains unknown).
`human_voice.guard_on_false_rejection`
means a **caller-labelled** human recording would be classified as no speech
before the model is invoked; it is **not** an observed Whisper transcription
error rate. The `real_noise` metrics describe pre-decode screening only;
noise that still reaches the decoder has **not** automatically hallucinated.
The `synthetic_controls` group is reported separately. Empty language/noise
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

## ABI2 correlation contract (integration qualification)

The live opt-in C11 guard (default **OFF**) also requires absolute
lag-1 adjacent PCM correlation strictly **below 0.12**, in addition to
at least 50 full 20-ms frames, each nonzero frame power, CV at most
0.12 and max/min frame-power ratio at most 2.0. The offline evaluator
must use the same unrounded full-snapshot statistics. A perfectly flat-
power but coherent square wave is **not** rejected as stationary noise;
an uncorrelated constant-power synthetic waveform is. This is a narrow
pre-decode test, **not acoustic VAD**, real noise hallucination evidence,
or proof a declared human voice actually contains intelligible speech.

The stacked ancestor drafts (#145–#149) were qualified against an older
power-only Provider; their historical CI success must not be projected
onto this reconciled correlation-aware integration head. Real human voice
and natural noise quality evidence remains absent. No merge or release.

## Potential independent real-voice input: Mozilla Common Voice v27.0

For the still **OPEN** real-speaker acceptance matrix, one possible locally
managed source is the official Mozilla Data Collective's **Common Voice
Scripted Speech 27.0**, released in September 2026. Its public dataset
catalog lists English (`en`), Japanese (`ja`) and Chinese (`zh-CN`)
collections as **CC0-1.0**. Actual eligibility must be verified on the
*exact downloaded release*, not assumed from a different version or mirror:

- Official catalog: https://mozilladatacollective.com/organization/cmfh0j9o10006ns07jq45h7xk
- Official Japanese data sheet: https://mozilladatacollective.com/datasets/cmu5xbhnf00fmnq07z9p3aa70
- Version and license announcement: https://discourse.mozilla.org/t/common-voice-scripted-speech-v27-0-and-spontaneous-speech-v5-0-released/149490

**Restrictions matter despite the CC0 label:** the Japanese v27.0 data
sheet explicitly forbids speaker identity determination and dataset
re-hosting/re-sharing. Follow each acquired locale's terms. The *private*
evaluator neither acquires these files nor attempts to identify speakers,
and it must not publish audio, full manifests, paths, raw labels,
speaker tokens or model transcripts. Corpus downloads require a separate
deliberate user action outside ordinary GitHub PR CI.

Suggested operator-side protocol:

1. Acquire exact release through the publisher under applicable terms, locally.
   Record version, access date, documented rights and original source checksums
   in a private provenance log. Never infer a participant's real identity.
2. Keep original MP3 and metadata off public GitHub. Convert **locally** into
   strictly 16-kHz mono S16LE, whole 320-sample/20-ms-frame WAVs with a
   pinned conversion tool and documented padding/crop choices. Rehash the
   **final WAV bytes**, check every sample is <=60 s, and never conflate the
   source checksum with the converted WAV checksum.
3. Populate private schema-2 manifests with `language`, `source_kind`,
   `condition`, `rights_basis` and opaque speaker grouping *only when
   legitimately provided by corpus metadata*. Unknown grouping must remain
   unknown; do not invent speaker independence or reconstruct identity.
4. Separately source properly permitted real fan/HVAC/traffic/intermittent
   room audio; do not label synthetic noise as a real recording. Ensure
   conditions reflect the actual samples and no double-use of identical PCM.
5. Run the offline evaluator **locally**, inspect its explicit `coverage_gaps`
   and null empty-cohort denominators. Consider live-model WER/CER,
   hallucination, false-refusal, latency and DSO lifetime only in a separately
   authorized private environment. No raw assets enter public artifacts.

Even multiple real-speaker samples do not automatically validate a
production VAD. Keep the experimental guard DEFAULT OFF, #137/#146 OPEN,
and all stacked PRs Draft / DO NOT MERGE / DO NOT RELEASE.

## Integrated private normalization and corpus parity gate (#153)

The independent normalization tool from [Draft #151](https://github.com/qigao/TurboAgent/pull/151)
is now wired into a separate Draft [#153](https://github.com/qigao/TurboAgent/pull/153),
stacked on #152. For already authorized **local** source recordings:

1. Keep the original recording and its true source SHA-256 private.
2. Pass the verified byte snapshot to a caller-reviewed absolute FFmpeg binary
   using its explicitly pinned executable SHA-256 and `--acknowledge-rights`.
   Conversion creates a private 0600 canonical WAV and private 0600 provenance;
   existing destinations and overlength/invalid results fail closed.
3. Use the *converted* WAV's pinned SHA in the private schema 1/2 manifest.
   The ABI2 evaluator reads and scores a single immutable WAV snapshot.
   Sample aliases that decode to the same PCM are rejected as duplicates.
4. Read explicit unknown-rate / uncovered-language/noise strata in the report.
   Do **not** interpret silent/unseen cohorts as 0% error. The report measures
   the **predecode lag-one-plus-frame-power** opt-in filter only, not actual
   speech recognition or noise hallucination.

Public CI uses exclusively deterministic synthetic WAV/Ogg fixtures to check
the **actual CPU FFmpeg -> private WAV -> SHA -> lag-1 corpus** handoff.
Neither human voices, speaker identities, raw audio, private provenance,
model weights, nor transcripts are fetched or uploaded. Run the private
data pipeline locally under the source dataset's consent/license terms.
All rights and speaker-group declarations are unverified by the software.
The experimental filter stays **OFF** by default; no merge or publication.
