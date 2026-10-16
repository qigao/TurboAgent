# RFC M4 — Cooperative SpeechOutput / TTS Provider (C11)

**DESIGN ONLY — DRAFT / DO NOT MERGE / DO NOT RELEASE.** See [Issue #164](https://github.com/qigao/TurboAgent/issues/164).
No TTS implementation, model download, voice/audio upload, playback activation or release is authorized by this RFC.

## Decision

Introduce optional **SpeechOutput** as a new C11 TTS interface, with a **host-owned Voice Coordinator** that composes the existing opt-in SpeechInput/ASR, the existing Harness/Session/Agent workflow and **SaltsUtils Salts::Playback**. SpeechOutput converts *approved user-visible FINAL text* into bounded PCM. It does not own the microphone, playback device, Agent, Session, tool authority or model network access.

Cooperation means caller-driven turn-taking and cancellation, not shared ownership or automatic provider fallback. An explicitly selected TTS Provider serves one utterance. Multiple providers may later be selected by voice/language at a drained boundary, but must never concurrently write the same native sink.

### Concrete existing contracts

The unmerged SpeechInput M1 header (speech_input/include/turbo_agent_speech_input.h) defines checked ASR FINAL, a borrowed Provider context, caller-driven pump, 320 samples of 16kHz S16 mono per 20ms and a bounded capture queue. Its PARTIAL cannot authorize Agent/tool invocation. The existing Harness exposes start_text, cancellation, execution and borrowed event sinks; the host explicitly controls whether ASR FINAL is submitted and which *approved visible* assistant FINAL is spoken.

SaltsUtils already defines **Salts::Playback** (playback/include/salts_playback.h; docs/architecture/media-playback-ownership.md). It owns the output device and bounded SPSC ring. Rates 8/16/24/48kHz, mono/stereo, S16/S32/F32 and an explicit 10–2000ms ring are supported by its source contract. There is **one producer** for playback_write; short successful writes are explicit backpressure. Producer must be quiescent before clear or destroy. Drain means buffered PCM was consumed, not guaranteed physically heard. **Independently verify that the target installed SaltsUtils.Native SDK exports Playback** before integrating: master-source availability does not prove the release pin.

Do **not** implement another native speaker/audio-device backend in TurboAgent.

## Data and authority flow

~~~text
 Salts::Capture (mic device)                  Host Voice Coordinator
           |                                            |
 SpeechInput / ASR -- checked FINAL only -------------> |
                                                        |
                                            Existing Session / Harness
                                        Agent, tools, authorization, FINAL
                                                        |
                                     approved user-facing readable text
                                                        |
                                 SpeechOutput C11 TTS (one generation)
                                                        |
                             one 20ms PCM staging block + short-write retry
                                                        |
                          Host-owned Salts::Playback (single PCM producer)
                                                        |
                                                  native speaker device
~~~

No audio device callback may execute ASR/TTS/CHttp/Agent/Session code. Voice Coordinator may observe user controls and terminal session events, but it does not copy model/tool internals to the speech synthesizer. The TTS Provider must not initiate any tool call or submit another session run.

## Proposed C11 v1 ABI sketch — for review, NOT an installed header

~~~c
#define TURBO_AGENT_SPEECH_OUTPUT_ABI_VERSION 1u
#define TURBO_AGENT_TTS_FRAME_MS 20u

typedef enum turbo_tts_poll_kind_e {
  TURBO_TTS_WOULD_BLOCK = 0,
  TURBO_TTS_PCM = 1,
  TURBO_TTS_SOURCE_DONE = 2,
  TURBO_TTS_CANCELED = 3,
  TURBO_TTS_FAILED = 4
} turbo_tts_poll_kind_t;

typedef struct turbo_tts_format_s {
  uint32_t sample_rate_hz; /* MVP 24000 only */
  uint32_t channels;       /* MVP mono */
  uint32_t bits_per_sample;/* MVP S16 */
  uint32_t frame_ms;       /* MVP 20 */
} turbo_tts_format_t;

/* Provider context is borrowed until generation is terminal/settled.
 * Host owns copied approved text, PCM staging and the Salts sink. */
typedef struct turbo_tts_provider_v1_s {
  size_t struct_size;
  uint32_t abi_version;
  void *context;
  int (*begin)(void *context, uint64_t generation_id,
               const char *approved_utf8, size_t text_bytes,
               const char *voice_id, const char *language,
               const turbo_tts_format_t *format);
  int (*poll_pcm)(void *context, uint64_t generation_id,
                  int16_t *dst_s16, size_t capacity_samples,
                  size_t *out_samples, turbo_tts_poll_kind_t *out_kind);
  void (*request_cancel)(void *context, uint64_t generation_id);
} turbo_tts_provider_v1_t;
~~~

The host facade should separately offer create, begin, pump, cancel, get_state and destroy. All mutating operations are serialized by its control owner. The generation ID increases monotonically; stale data is rejected. SpeechOutput holds one bounded, validated UTF-8 text copy through settlement. Provider text is borrowed for the scoped generation and may not outlive it or become persistent runtime state. The Provider copies any output into the one host-supplied PCM staging buffer; it may not retain that buffer or write the native sink.

Poll returns one bounded frame, WOULD_BLOCK or terminal. WOULD_BLOCK must not spin in the audio callback. A cooperative cancellation request is **not a hard real-time interruption** of a blocking inference vendor. Real noncooperative vendors require a separately reviewed process isolation and hard-stop mechanism, with true lifecycle evidence.

### Exact states / ownership

~~~text
IDLE -- begin --> SYNTHESIZING -- source DONE --> PLAYBACK_DRAINING -- ring empty --> SETTLED
                      |
                      +-- request_cancel --> CANCEL_PENDING -- producer quiesced
                                                        + provider settled --> CANCELED
                      +-- error --------------------------------------------> FAILED
~~~

SOURCE_DONE is not PLAYBACK_DRAINED. For cancellation: refuse further new PCM, settle/join any Provider worker, quiesce the *sole* playback writer, discard its staging suffix, then clear retained Salts Playback PCM on the control owner. Provider DSO may be unloaded only after no borrowed text, PCM or in-flight callback remains. Do not introduce a second arbitrary Owner or raw-audio persistence path.

## Output format, bounds and backpressure

MVP: exactly **24kHz mono S16**, 20ms = **480 samples / 960 bytes**. The last partial block can contain fewer *complete samples* without implicit padding. The host configures Salts Playback to the same format with a **100ms** logical ring (4800 bytes). One pending 960-byte staging block plus that 4800-byte logical ring means **at most 120ms** of staged/output PCM; vendor-internal synthesis memory needs its own separately proven cap. Proposed upper bounds: 4096 checked UTF-8 bytes of input text and 30 seconds of synthesized output per generation.

Handle short playback_write success by preserving and retrying the remaining frame-aligned suffix. Do not call Provider poll again until it has been fully submitted. No drop, overwrite, unbounded PCM queue, silent resampler or implicit device/provider fallback. Device/PCM format mismatch must return explicit unsupported/error. Playback drain is bounded and never declares that sound was physically heard.

## Cooperative turn-taking: half duplex first

1. Start Capture + one ASR generation. On explicit end-of-utterance, stop/join the mic callback, finish/pump SpeechInput, take the checked FINAL exactly once.
2. The host explicitly admits FINAL to the existing Harness/Session. Session performs any permitted tools. ASR PARTIAL can be rendered, never submitted as a tool-capable command.
3. After terminal Agent execution, host chooses a *reviewed user-visible answer projection*: not speculative SSE tokens, hidden reasoning, tool JSON, secrets, markup or untrusted SSML/URLs.
4. Select one authorized TTS voice and Provider. Pump 20ms chunks from the control/worker side, submit the single staging block through the one playback writer, respect short writes and wait for ring drain.
5. Only after output settles does the host resume a *new* Capture generation. Report PLAYBACK_DRAINED, not HEARD_BY_USER.

### Explicit interruption

Push-to-talk or user Stop can cancel TTS even mid-playback. Mark the output generation canceled; prevent new Provider admissions; wait for synth worker settle; quiesce the sole PCM writer; clear the Salts ring; then start a new ASR generation. A stale TTS frame must **never** be offered as new user microphone audio. No DSO unload before callback/worker settlement.

### Optional full duplex (not M4 MVP)

Speaker playback leaking into microphone may be hallucinated as ASR speech and trigger an Agent run. The existing #146 real-acoustic gate is unfinished. Do NOT automatically enable full duplex or treat Whisper's test-only NO_SPEECH results as reliable barge-in detection. Full duplex separately needs proven AEC / output-reference synchronization, device-specific echo testing and false-wake evidence. Explicit UI push-to-talk is the first safe cooperative interruption.

## Content and Provider policy

Plain text first, strict UTF-8 scalar and embedded-control validation; no arbitrary SSML injection, URL/file fetching, tool calls or background cloud synth authorization. Explicit voice/language selection (en/ja/zh as initial candidates); no auto-fallback to another engine, network service or license. Voice cloning/identity matching is out of scope without separate lawful consent. Sentence-streaming may come later only for *already committed* user-facing response chunks, never for raw model stream deltas.

Provider selection and scheduling do not change the LLM Provider/Agent runtime. One output owner and one playback writer remain authoritative regardless of how many future TTS backends exist.

## M4 milestone and CI gates

| Phase | Acceptance before promotion |
|---|---|
| M4.0 Provider contract | Pure fake TTS, public C11 + C++17 installed compile, struct-size/ABI, invalid UTF-8/control/voice/format, bounded text+samples, exact frame/final partial, repeated cancel, stale generation, callback reentry refusal |
| M4.1 Output bridge | Confirm pinned installed SaltsUtils Playback, real null-device backend, 24k/mono/S16, logical 100ms ring, short-write/backpressure with no drop/reorder, producer quiescence before clear/destroy, device error and drain timeout |
| M4.2 Collaboration | Checked ASR FINAL → explicit Harness execution → approved assistant FINAL → TTS → Playback. PARTIAL/tool internals never invoke TTS/Agent tools. Push-to-talk mid-output cancellation, late-frame denial, no cross-turn echo |
| M4.3 Real TTS provider | Exact licensed model + runtime/source fingerprint, memory/CPU bounds, fail-closed device/model, source-instrumented TSan/ASan/UBSan, true DSO load/cancel/drain/unload. Weight download only after separate authorization |
| M4.4 Optional full duplex | AEC and physical microphone/speaker tests, genuine voices/languages, barge-in false positives/negatives, latency p50/p95, RSS, noncooperative inference hard isolation |

Delivery targets: independent option **ENABLE_SPEECH_OUTPUT=OFF**, no modification of ASR ABI, no silent default enable. CI includes Linux/Windows/macOS/Android/iOS installed-native consumer checks as applicable, with exact SaltsUtils Playback Registry pin. Separate existing full native SDK Linux PraktorTools hidden hash_bytes issue #150 is not a license to bypass link validation.

**This RFC is review-only.** No model download, native playback activation, implementation merge or release is authorized. Keep #137/#146 and all existing SpeechInput Draft branches unchanged.
