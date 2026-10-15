# M4.1 — TTS PCM to Salts::Playback bridge (experimental)

**DRAFT / DO NOT MERGE / DO NOT RELEASE.** Tracks [TurboAgent #164](https://github.com/qigao/TurboAgent/issues/164), stacked on C11 M4.0 [PR #171](https://github.com/qigao/TurboAgent/pull/171). This does not create a native output device, load a TTS model, enable the default build or perform any Agent call.

## Exact installed package boundary

Requires the public SaltsUtils.Native **4.3.0-rc.11** archive and the exact paired Salts.Native **2.3.0-rc.16** release. CI downloads these *published artifacts*, validates byte counts and release SHA256 values **before** extraction, then checks the real installed Salts::Playback header, CMake target export and link library per supported RID. No source-master header, bundled miniaudio or unpinned alternative is accepted.

SaltsUtils rc.11 SHA256: 0d15b3dba534b55a42069dc665070cf177062d0bd029b31639dfb2bb2d4bdf64

Salts rc.16 SHA256: 0b8592cb0b27e91fb5a3ee2c5a888ca53c7593cf9eb8cd5adfbc38fac43659b7

RIDs: Linux x64, Windows x64, macOS arm64, Android arm64-v8a and iOS arm64; Linux arm64 intentionally excludes Capture/Playback from its smaller release profile. The linked CMake consumer imports the **actual published Salts::Playback**, without opening a device. A mock-sink test uses **the exact same published salts_playback.h**. Its fake functions are test-only, never installed as a native backend.

## Exact ownership

The host alone owns SpeechOutput and Salts Playback. The bridge borrows them until bridge_destroy succeeds. It holds exactly one 480-sample / 960-byte host PCM staging block and no second ring, device, inference context or worker. Only a serialized control owner calls the bridge and is the sole Salts playback_write producer.

Every short write retains an exact frame-aligned suffix; no new Provider poll is allowed before the suffix is fully written. A zero write reports BACKPRESSURE without spinning. A generated-source TERMINAL with all pending bytes written means SOURCE_DONE, not played; only the separate drain after ring consumption reports PLAYBACK_DRAINED, not necessarily heard by a person.

Cancellation prevents new writes, quiesces the sole writer, discards pending samples, invokes Salts playback_clear in the host owner, and continues polling the borrowed TTS Provider until true terminal settlement. Late PCM never enters the ring. Early bridge destroy returns EBUSY. A broken/non-polling vendor is NOT force-preempted or allowed to unload; future process isolation remains a separate M4.3 decision.

## Deterministic tests and non-goals

Tests cover a 480-sample full block plus an 80-sample tail, exact FIFO order, backpressure with 160-byte and zero short writes, frame alignment, SOURCE_DONE versus independent device drain, cancel-first/late PCM, device disappearance, and no unexpected use of playback_clear or duplicate writer. Address/Undefined/Thread source sanitizers and one installed Salts public-header consumer are required.

Real playback hardware, true TTS acoustic quality, model weights, voice cloning, Agent/Session orchestration, full duplex and echo cancellation are all outside M4.1. Main TurboAgent's unrelated Linux PraktorTools hidden hash_bytes issue remains tracked by #150; no symbol-visibility workaround or SDK release is authorized.
