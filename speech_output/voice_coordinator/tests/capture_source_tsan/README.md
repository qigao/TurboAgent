# Original Salts Capture Linux source TSan (virtual-only)

This **independent opt-in Linux Draft** continues #206. It rebuilds the
**unchanged SaltsUtils v4.3.0-rc.11 Capture C11 Linux production source**
with `-fsanitize=thread`, alongside the **unchanged upstream miniaudio
0.11.22 native audio backend**, then runs the actual original TurboAgent M1
SpeechInput and M2 `SpeechCaptureSalts` adapter from immutable SHA
`9e51d377bd84ee91ce5a47c94f24f73e4e01bba7`.

The source build deliberately includes original video/screen dispatchers:
`capture_video.c`, `capture_linux.c`, `capture_audio_miniaudio.c`,
`capture_video_linux.c`, `capture_screen_linux.c` and
`miniaudio_impl.c`. All are production implementations from the
tag, **not test stand-ins for `salts_capture_*` functions**. They link
original Linux PipeWire/X11 runtime libraries; only the audio path is
opened in this test.

## Strict input isolation

A separate private PulseAudio daemon with **no default modules** exposes
one local UNIX socket, one software `ta_ci_sink` null sink, **only one**
`ta_ci_sink.monitor` source, and **zero** hardware cards. Before any
`salts_capture_start`, the original #203 test refuses a missing,
multiple, physical or mismatched native source and passes the **exact
enumerated device ID**. Only bounded digital-zero PCM is fed into the
software sink. No user microphone/speaker, external model, recorded bytes,
network service or implicit device fallback is allowed.

The test validates at least six consecutive full sessions: actual
`salts_audio_capture_create/start`, the miniaudio callback's borrowed
PCM lifetime, `salts_capture_stop` and native callback thread join,
no post-stop callback; an intentionally **held in-flight callback**
must keep a competing `stop` call blocked until explicitly released,
then stop and destroy settle safely. The original M2 native adapter must
refuse early destroy, copy precisely-sized S16LE frames into the single
M1 bounded queue, stop/join before original M1 one-shot FINAL, and
ensure cancellation is terminal with no FINAL.

The executable has `DT_NEEDED libsalts_capture.so` whose loader path
must be **the newly source-built TSan library**, never the installed
uninstrumented published DSO. CI asserts `__tsan_init` inside this
library and its genuine `salts_capture_start/stop/create` definitions.
The original M1/M2 functions are defined locally, without a substitute
Capture API. `TSAN_OPTIONS=halt_on_error=1:exitcode=66` and **no
suppressions**; a race or deadlock fails the job.

## Explicit non-claims

- This is source-level TSan **of the original Capture and miniaudio code**,
  **not** a new officially published binary nor a physical microphone test.
- The upstream Salts.Native Core/Platform SDK and OS PipeWire/PulseAudio
  dependencies are installed binaries, not rebuilt with TSan here.
- The test-only ASR Provider emits deterministic text and never performs
  Whisper/model-backed inference. It cannot establish acoustic accuracy,
  noise/AEC/barge-in, licensed TTS or a real voice interaction.
- Production defaults remain OFF, no new PCM queues, raw recording
  persistence, suppression, fallback, model download, merge or release.
- #206 original Harness/Agent/TTS worker and native Playback source TSan
  results remain separate required gates. Main full SDK Linux hidden
  `hash_bytes` and Windows CLI storage timeout remain separate.

**DRAFT / DO NOT MERGE / DO NOT RELEASE**; parent M4 issue #164 remains OPEN.
