# Private licensed speech corpus: strict WAV preparation

**Test-only Draft / no model download / no upload / no microphone / no release.**

This is a *local preprocessing step* before
[`evaluate_stationarity_corpus.py`](evaluate_stationarity_corpus.py).
The optional production C11 stationary-noise guard remains **OFF by default**.
It cannot establish acoustic VAD quality, speech recognition accuracy or speaker
independence.

## Rights and identity

Only use recordings that you acquired lawfully and have permission to process.
License labels and speaker groups are **caller-declared, not independently
verified by this tool**. Preserve the source release/page URL, license terms,
and origin checksum in a **private audit log**. If redistribution or rehosting
is restricted, do not put media, converted media, transcripts, manifests,
hashed speaker labels or provenance evidence into GitHub, CI logs or artifacts.
Never attempt to identify a speaker. Use independent opaque *group labels*
only when allowed by the actual dataset license and metadata contract.

Examples of candidate **source catalog pages**, not automatic downloads or
approved entries:

- [Wikimedia Commons Japanese reading, CC0](https://commons.wikimedia.org/wiki/File:%E6%98%A5%E3%81%AF%E3%81%82%E3%81%91%E3%81%BC%E3%81%AE.ogg)
  (approximately 15 seconds)
- [Wikimedia Commons Mandarin reading, CC0](https://commons.wikimedia.org/wiki/File:Mandarin_Chinese-%E5%8D%97%E5%9B%BD%E5%B1%B1%E6%B2%B3.ogg)
  (approximately 15 seconds)
- [Google FLEURS](https://huggingface.co/datasets/google/fleurs), card
  CC BY 4.0; verify exact revision, attribution and individual source use
- [Mozilla Common Voice](https://mozilladatacollective.com/), many datasets
  labeled CC0; data-page terms may prohibit rehosting and re-identification

None of these establishes *quiet/monotone speaker* coverage or code-switching;
those must be annotated from actual permitted recordings, not guessed.

## Exact bounded local preparation

Example on Linux/macOS with an already downloaded and rights-reviewed file:

~~~sh
set -eu
umask 077
source=/private/licensed-clip.ogg
input_sha="$(sha256sum "$source" | cut -d' ' -f1)"
ffmpeg_binary="$(realpath "$(command -v ffmpeg)")"
python3 speech_input/whisper/tools/prepare_stationarity_wav.py \
  --source "$source" \
  --source-sha256 "$input_sha" \
  --ffmpeg "$ffmpeg_binary" \
  --output-wav /private/speech-normalized/clip1.wav \
  --private-provenance /private/speech-normalized/clip1.provenance.json \
  --acknowledge-rights
~~~

All source and destination paths must be **absolute**. Both destination files
must **not** already exist. The tool writes with 0600 permissions, does not
overwrite, uses one bounded (at most 16 MiB) original byte snapshot and passes
these **exact** verified bytes through `ffmpeg` stdin.

The decoder is a selected local executable and is invoked without a shell,
with `-nostdin -protocol_whitelist pipe`; it cannot fetch external
protocols/URLs or read arbitrary secondary media referenced by a manifest.
CPU-only mono S16LE 16kHz output is capped and **clips longer than 60 seconds
are refused, not silently truncated**. Only the final *incomplete* 20ms frame
(if any) is discarded (at most 319 samples). This changes the source duration
by strictly less than 20ms and is recorded; it never appends artificial
zero-power frames and never trims leading silence or performs VAD.

The **private** provenance file records the SHA-256 of the original snapshot,
converted canonical WAV, decoded PCM and selected decoder executable; do not
publish it. Standard output returns only an OK classification, frame count and
the number of discarded last-frame samples, with no paths, source hashes,
speaker IDs or transcripts.

A conversion to mono or resampling may change stationarity and audio quality.
Therefore retain the original source byte fingerprint and exact decoder
fingerprint in your private audit. Do not compare different transcoding
versions as though their 20ms power envelopes were identical.

## Feed the offline evaluator

Take the `normalized_wav_sha256` from the private provenance JSON and put
the **converted** WAV path (relative to a chosen private `--audio-root`) and
its SHA into the schema 2 manifest described in
[`STATIONARITY_CORPUS.md`](STATIONARITY_CORPUS.md). Use private anonymous
`speaker_group` tokens for human voice recordings only; group tokens are
never raw names or an invitation to infer identity from audio.

~~~sh
python3 speech_input/whisper/tools/evaluate_stationarity_corpus.py \
  --audio-root /private/speech-normalized \
  --manifest /private/corpus-schema2.json \
  --report /private/stationarity-aggregate.json
~~~

The offline evaluator reports pre-decoder Guard OFF/ON stationarity outcomes
and per-group/Wilson statistics; it does **not** call Whisper or calculate
WER/CER or hallucination rates. These need separately approved pinned model
weights on the *same* samples, without silently changing Provider selection.

Do not turn the experimental guard on by default, merge, release, or publish
model weights/audio based on this preprocessing or on synthetic tests alone.
