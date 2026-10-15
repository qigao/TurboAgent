# First separately sourced human-voice stationarity evidence

**Draft-only / 2026-10-15 / Issue #146.** This document records only
public-media source provenance and numerical results from a model-free
pre-decode stationarity diagnostic. There are **no original audio files,
converted audio, transcripts, private speaker IDs, model weights or
downloadable audio artifacts in this repository**.

## Sources and exact original media

| Language | Commons source and attribution | Public license | Bytes | Source SHA-1 | Source SHA-256 |
|---|---|---|---:|---|---|
| ja | [春はあけぼの.ogg](https://commons.wikimedia.org/wiki/File:%E6%98%A5%E3%81%AF%E3%81%82%E3%81%91%E3%81%BC%E3%81%AE.ogg), すじにくシチュー, self-published reading, 2019-03-12 current file | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | 264858 | `c7992ec2188ee94102f83e4eff22c4f9b9365d39` | `cad2f1545a3ac898362590ea636b8c09f1d77391b506790ae909e0e1ff1441f7` |
| zh | [Mandarin Chinese-南国山河.ogg](https://commons.wikimedia.org/wiki/File:Mandarin_Chinese-%E5%8D%97%E5%9B%BD%E5%B1%B1%E6%B2%B3.ogg), Ngguls, own-work reading, 2019-11-11 current file | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) | 156728 | `2fb567da2edd9fc18160769b1c803bd9d072c273` | `304f11fa91ec51b2a2b9e4c11424f9944c6bdf3ee909440923511a86b5585b22` |
| en | [En-uk-Do you speak English.ogg](https://commons.wikimedia.org/wiki/File:En-uk-Do_you_speak_English.ogg), **Association Shtooka / Judith Franck**, UK female-voice pronunciation, 2010-01-18 current file | [CC BY 3.0 US](https://creativecommons.org/licenses/by/3.0/us/); attribution and modification disclosure required | 21562 | `79b6f88e3db116fab8bc068cd523420406bef1c4` | `10829789c781d912ccbfdd9a4f5d4b3dbb2dac0a42af34610f7cf4497ad25deb` |

Original files were fetched from the fixed Commons upload URL only by an
isolated opt-in pull-request job, verified against exact byte length,
SHA-1, and independently recorded SHA-256. Each clip was converted by
the existing test-only private FFmpeg preflight with selected binary
SHA-256 checked before and after conversion. Temporary media,
PCM data and private provenance were removed by job exit trap. **Do
not copy these assets into CI artifacts or checked-in source trees.**

The English file is NOT CC0; keep the exact Shtooka/Judith Franck
CC BY 3.0 US attribution and state that the audio was converted to
16 kHz mono S16LE for analysis. Mere access to a Commons audio page
does not validate an independently sampled speaker population.

## Actual per-clip pre-decode gate results

| Language | Converted WAV SHA-256 (public-source derivative only) | 20ms frames | Duration | Guard OFF NO_SPEECH | Guard ON NO_SPEECH |
|---|---|---:|---:|---:|---:|
| ja | `ccb16e8ea766bd2f4bf40ed09e5666ea6700edbd79343eb961fb13e9cafd6a03` | 740 | 14.80s | 0/1 | 0/1 |
| zh | `f246dded9eb91796c515f21b37f8e41b3357bdef3f6fe19552568c12653f0421` | 749 | 14.98s | 0/1 | 0/1 |
| en | `458ce0d1086ae8a5381ccc09878a630c016ebed42303a859e2030e2efbbe339e` | 95 | 1.90s | 0/1 | 0/1 |
| **Total** | 3 separate source recordings, not independently verified speakers | **1584** | **31.68s** | **0/3** | **0/3** |

Evidence:
- [#38732158834](https://github.com/qigao/TurboAgent/actions/runs/38732158834) — initial Japanese first human voice candidate, Source+conversion+numeric guard PASS
- [#38732624071](https://github.com/qigao/TurboAgent/actions/runs/38732624071) — Japanese repeated with exact SHA-256, Mandarin SHA-256 discovery
- [#38733067205](https://github.com/qigao/TurboAgent/actions/runs/38733067205) — Japanese+Mandarin independently SHA-256 pinned, English initial SHA-1+size provenance; all source & guard checks PASS

These classifications come from the **existing model-free Python 20ms
frame-power evaluator**, whose thresholds and boundary cases are
separately cross-checked against the native strict C11 Provider source
using fake inference under sanitizers. These WAVs were not themselves
decoded by actual Whisper weights. Therefore `0/3` is neither a WER/CER
score nor a statistically representative false-rejection rate, and it
does **not** prove that the C11 production Provider will never reject
quiet/monotone speech.

There is no confirmed recording of low-volume/monotone human speech,
code-switching, true real-room/impulsive noise, or multiple independently
verified speakers per language in this 3-clip cohort. **Default
stationary guard stays OFF.** #137/#146 remain OPEN; the complete
stacked PR chain remains Draft / DO NOT MERGE / DO NOT RELEASE.
